// Windows raw capture; optional stdin session protocol separates warmup from T0/T1.
#include "transport/Dca1000Control.h"
#include "transport/Dca1000Reassembler.h"
#include "transport/MmwaveCfg.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void onSignal(int) { interrupted = 1; }
std::uint64_t clockNs() {
  LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
  return (t.QuadPart/f.QuadPart)*1000000000ULL + (t.QuadPart%f.QuadPart)*1000000000ULL/f.QuadPart;
}
std::mutex outputMutex;
void emit(const std::string &line) {
  std::lock_guard<std::mutex> lock(outputMutex); std::cout << line << std::endl;
}
void event(const char *name, std::uint64_t stamp = 0) {
  emit(std::string(name) + " " + std::to_string(stamp ? stamp : clockNs()));
}
struct Commands {
  bool start = false, stop = false;
  std::uint64_t formalAfter = 0, durationNs = 0;
  std::string pending;
  void poll(bool managed) {
    if (interrupted) stop = true;
    HANDLE h=GetStdHandle(STD_INPUT_HANDLE);
    if (GetFileType(h)!=FILE_TYPE_PIPE) return;
    DWORD n=0;
    if (!PeekNamedPipe(h,nullptr,0,nullptr,&n,nullptr)) { stop=true; return; }
    if (!n) return;
    char data[256]; DWORD got=0;
    if (!ReadFile(h,data,sizeof(data),&got,nullptr)) {stop=true;return;}
    if (!managed) {stop=got>0;return;}
    pending.append(data,got);
    if (pending.size()>4096) {stop=true;return;}
    std::size_t pos;
    while ((pos=pending.find('\n'))!=std::string::npos) {
      std::istringstream line(pending.substr(0,pos));pending.erase(0,pos+1);
      std::string verb;line>>verb;
      if (verb=="start") start=true;
      else if (verb=="stop") stop=true;
      else if (verb=="formal" && !formalAfter) {
        std::uint64_t requested=0, duration=0;
        if (!(line>>requested>>duration) || duration>86400000000000ULL) {stop=true;continue;}
        formalAfter=std::max(requested,clockNs());durationNs=duration;
      } else if (!verb.empty()) stop=true;
    }
  }
};
struct Network {
  Network() { WSADATA d{};if(WSAStartup(MAKEWORD(2,2),&d))throw std::runtime_error("WSAStartup failed"); }
  ~Network() {WSACleanup();}
};
struct Socket {SOCKET value=INVALID_SOCKET;~Socket(){if(value!=INVALID_SOCKET)closesocket(value);}};
}

int main(int argc,char **argv) {
  std::signal(SIGINT,onSignal);std::signal(SIGTERM,onSignal);
  try {
    if(argc==3 && std::string(argv[1])=="--validate-cfg") {
      radar::MmwaveCfg check;std::string reason;
      if(!radar::loadMmwaveCfg(argv[2],check,reason))throw std::runtime_error(reason);
      std::cout<<"frameBytes="<<check.radar.bytesPerFrame<<"\nnumFrames="<<check.radar.numFrames
               <<"\nframePeriodMs="<<check.radar.framePeriodicityMs<<'\n';return 0;
    }
    char flag[8]{};
    const bool managed=GetEnvironmentVariableA("RADAR_SESSION_CONTROL",flag,sizeof(flag))>0 && std::string(flag)=="1";
    Network network;radar::CaptureConfig cfg;radar::CliOverrides cli;std::string error;
    if(!radar::resolveCaptureConfig(argc,argv,cfg,cli,error))throw std::runtime_error(error);
    radar::MmwaveCfg mmcfg;
    if(!cfg.cfg.empty() && !radar::loadMmwaveCfg(cfg.cfg,mmcfg,error))throw std::runtime_error(error);
    if(managed && mmcfg.radar.numFrames!=0)throw std::runtime_error("session mode requires frameCfg numFrames=0 (continuous)");
    const auto fromCfg=mmcfg.radar.bytesPerFrame;
    if(cfg.frameBytes && fromCfg && cfg.frameBytes!=fromCfg)throw std::runtime_error("frameBytes disagrees with cfg");
    const std::size_t frameBytes=fromCfg?fromCfg:cfg.frameBytes;
    if(!frameBytes || frameBytes>256u*1024u*1024u)throw std::runtime_error("invalid frame size");
    const std::uint64_t maxFrames=cfg.maxFrames?cfg.maxFrames:(cfg.noControl?0:mmcfg.radar.numFrames);
    const auto path=std::filesystem::u8path(cfg.output);
    if(std::filesystem::exists(path)||std::filesystem::exists(std::filesystem::u8path(cfg.output+".frames.csv")))throw std::runtime_error("output already exists");
    std::ofstream raw(path,std::ios::binary),index(std::filesystem::u8path(cfg.output+".frames.csv"));
    if(!raw||!index)throw std::runtime_error("cannot create output files");
    index<<"file_frame,wire_frame,host_rx_monotonic_ns\n";
    Socket data;data.value=socket(AF_INET,SOCK_DGRAM,0);
    if(data.value==INVALID_SOCKET)throw std::runtime_error("data socket failed");
    setsockopt(data.value,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&cfg.rcvbuf),sizeof(cfg.rcvbuf));
    sockaddr_in local{},source{};
    if(!radar::resolveIpv4(cfg.bindIp,cfg.dataPort,local,error)||!radar::resolveIpv4(cfg.dcaIp,cfg.dataPort,source,error))throw std::runtime_error(error);
    if(bind(data.value,reinterpret_cast<sockaddr*>(&local),sizeof(local)))throw std::runtime_error("cannot bind data socket: "+cfg.bindIp);
    std::atomic<bool> closing{false},failed{false},accepting{!managed&&cfg.noControl};
    std::atomic<std::uint64_t> saved{0},lastFrameNs{clockNs()};
    // One lock owns the formal window, so T1 is fixed before sending any stop command.
    std::mutex windowMutex;
    std::uint64_t after=0,duration=0,t0=0,t1=0,formalFrames=0,firstFormal=0;
    std::uint64_t packetNs=0,previousWire=0;unsigned consecutive=0;bool stableReported=false;
    std::string rxError;
    auto endWindow=[&](std::uint64_t stamp) { // caller holds windowMutex
      if(t0&&!t1){t1=stamp;event("FORMAL_END",t1);}
    };
    radar::Dca1000Reassembler reassembler(frameBytes,[&](std::uint64_t wire,const auto &frame) {
      if(!managed&&maxFrames&&saved.load()>=maxFrames)return false;
      std::lock_guard<std::mutex> lock(windowMutex);
      if(saved.load()==0)event("FIRST_FRAME",packetNs);
      consecutive=(saved.load()&&wire==previousWire+1)?consecutive+1:1;previousWire=wire;
      if(consecutive>=3&&!stableReported){event("STABLE",packetNs);stableReported=true;}
      if(managed && after && !t0 && packetNs>=after && consecutive>=3) {
        t0=packetNs;firstFormal=saved.load();
        emit("FORMAL_START "+std::to_string(t0)+" "+std::to_string(firstFormal)+" "+std::to_string(wire));
      }
      if(t0&&duration&&packetNs>=t0+duration)endWindow(t0+duration);
      raw.write(reinterpret_cast<const char*>(frame.data()),static_cast<std::streamsize>(frame.size()));
      index<<saved.load()<<','<<wire<<','<<packetNs<<'\n';
      if(!raw||!index)throw std::runtime_error("output write failed (disk full?)");
      ++saved;lastFrameNs=packetNs;
      if(t0&&packetNs>=t0&&(!t1||packetNs<t1)) {
        ++formalFrames;
        // Half-open interval includes the Nth frame timestamp exactly.
        if(maxFrames&&formalFrames>=maxFrames)endWindow(packetNs+1);
      }
      return managed || !maxFrames || saved.load()<maxFrames;
    });
    std::thread receiver([&] {
      try {
        std::uint8_t packet[65536];bool firstPacket=true;
        while(!closing) {
          fd_set ready;FD_ZERO(&ready);FD_SET(data.value,&ready);timeval wait{0,100000};
          const int result=select(0,&ready,nullptr,nullptr,&wait);
          if(result<0)throw std::runtime_error("data select failed");if(!result)continue;
          sockaddr_in from{};int len=sizeof(from);
          const int n=recvfrom(data.value,reinterpret_cast<char*>(packet),sizeof(packet),0,reinterpret_cast<sockaddr*>(&from),&len);
          packetNs=clockNs();
          if(n<0)throw std::runtime_error("data receive failed");
          if(!accepting||(!cfg.noControl&&from.sin_addr.s_addr!=source.sin_addr.s_addr))continue;
          if(firstPacket){event("FIRST_PACKET",packetNs);firstPacket=false;}
          if(!reassembler.consume(packet,static_cast<std::size_t>(n)))break;
        }
      }catch(const std::exception &e){rxError=e.what();failed=true;}
    });
    std::unique_ptr<radar::Dca1000Control> control;Commands commands;bool ok=true;
    try {
      if(!cfg.noControl) {
        auto options=radar::linkOptionsFrom(cfg);
        options.trace=[](const std::string &name){event(name.c_str());};
        control=std::make_unique<radar::Dca1000Control>(std::move(options));
        control->open();control->connect();control->configureFpga();control->setPacketDelay();control->stopRadarOrThrow();
        commands.poll(managed);if(commands.stop)throw std::runtime_error("cancelled during configuration");
        control->sendCfgCommands(mmcfg.commands);
      }
      event("ARMED");
      if(managed) {
        const auto deadline=clockNs()+60000000000ULL;
        while(!commands.start&&!commands.stop&&!failed) {
          commands.poll(true);if(clockNs()>deadline)throw std::runtime_error("start gate timed out");
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      }
      commands.poll(managed);if(commands.stop)throw std::runtime_error("cancelled before capture start");
      if(failed)throw std::runtime_error("receiver failed before capture start");
      accepting=true;
      if(control) {
        event("DCA_START_BEGIN");control->startRecording();event("DCA_START_ACK");
        event("SENSOR_START_BEGIN");control->radarStart();event("SENSOR_START_ACK");
      }
      lastFrameNs=clockNs();event("READY");
      auto report=std::chrono::steady_clock::now();const auto warmupDeadline=clockNs()+30000000000ULL;
      for(;;) {
        commands.poll(managed);
        {
          std::lock_guard<std::mutex> lock(windowMutex);
          if(commands.formalAfter&&!after){after=commands.formalAfter;duration=commands.durationNs;event("FORMAL_GATE",after);}
          if(t0&&duration&&clockNs()>=t0+duration)endWindow(t0+duration);
          if(commands.stop||failed)endWindow(clockNs());
          if(commands.stop||failed||(managed&&t1)||(!managed&&maxFrames&&saved.load()>=maxFrames))break;
          if(managed&&!t0&&clockNs()>warmupDeadline)throw std::runtime_error("no common T0 within 30 seconds");
        }
        if(clockNs()-lastFrameNs.load()>15000000000ULL)throw std::runtime_error("no complete radar frame for 15 seconds");
        if(std::chrono::steady_clock::now()-report>std::chrono::milliseconds(250)) {
          emit("PROGRESS "+std::to_string(saved.load())+" "+std::to_string(saved.load()*frameBytes));
          if(managed){std::lock_guard<std::mutex> lock(windowMutex);emit("FORMAL_PROGRESS "+std::to_string(formalFrames));}
          report=std::chrono::steady_clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }catch(const std::exception &e){emit(std::string("capture failed: ")+e.what());ok=false;}
    {std::lock_guard<std::mutex> lock(windowMutex);endWindow(clockNs());}
    if(control) {
      try{if(control->recording()){event("DCA_STOP_BEGIN");control->stopRecording();event("DCA_STOP_ACK");}}
      catch(const std::exception &e){emit(std::string("stop failed: ")+e.what());ok=false;}
      if(control->radarStarted()){event("SENSOR_STOP_BEGIN");if(!control->stopRadarBestEffort())ok=false;else event("SENSOR_STOP_ACK");}
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));closing=true;receiver.join();
    if(failed){emit(rxError);ok=false;}
    raw.flush();index.flush();
    std::ofstream stats(std::filesystem::u8path(cfg.output+".stats.txt"));
    stats<<"savedFrames="<<saved.load()<<"\nframeBytes="<<frameBytes<<"\nmissingPackets="<<reassembler.missingPackets()
         <<"\nlatePackets="<<reassembler.latePackets()<<"\nmalformedPackets="<<reassembler.malformedPackets()
         <<"\ndiscardedFrames="<<reassembler.discardedFrames()<<'\n';
    if(managed)stats<<"formalFrames="<<formalFrames<<"\nformalStartNs="<<t0<<"\nformalEndNs="<<t1
                    <<"\npreRollFrames="<<(t0?firstFormal:saved.load())<<'\n';
    stats.flush();emit("PROGRESS "+std::to_string(saved.load())+" "+std::to_string(saved.load()*frameBytes));
    if(managed)emit("FORMAL_PROGRESS "+std::to_string(formalFrames));
    return ok&&raw&&index&&stats&&saved&&(!managed||(t0&&formalFrames))&&!reassembler.missingPackets()&&
           !reassembler.discardedFrames()&&!reassembler.malformedPackets()?0:2;
  }catch(const std::exception &e){std::cerr<<"capture failed: "<<e.what()<<'\n';return 1;}
}
