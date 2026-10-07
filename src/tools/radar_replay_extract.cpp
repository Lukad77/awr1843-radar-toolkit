// Offline session extraction. Never opens hardware or changes recorded files.
#include "transport/MmwaveCfg.h"
#include "core/BufferPool.h"
#include "core/FrameBuffer.h"
#include "pipeline/ParseStage.h"
#include "dsp/RangeFftStage.h"
#include "dsp/PhaseUnwrapStage.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
int run(const std::vector<std::string>& args) {
 try {
  if(args.size()!=5) throw std::runtime_error("usage: radar_replay_extract SESSION OUTPUT FIXED_BIN(-1=auto) DC(0/1)");
  const auto dir=std::filesystem::u8path(args[1]);
  radar::MmwaveCfg m;std::string err;
  if(!radar::loadMmwaveCfg((dir/"radar.cfg").u8string(),m,err))throw std::runtime_error(err);
  if(m.radar.bytesPerFrame<=0 || m.radar.bytesPerFrame>256*1024*1024)throw std::runtime_error("unsupported frame size");
  radar::PhaseUnwrapParams params;params.targetRangeBin=std::stoi(args[3]);params.followPeak=params.targetRangeBin<0;params.dcCompensation=args[4]=="1";
  if(params.targetRangeBin < -1 || params.targetRangeBin>=m.radar.numRangeBins)throw std::runtime_error("fixed bin outside range FFT");
  auto pool=[] {return radar::BufferPool<radar::FrameBuffer>::create([] {return std::make_unique<radar::FrameBuffer>();},{},4);};
  radar::ParseStage parse(m.radar,true,pool());radar::RangeFftStage range(m.radar,pool());
  auto phase=std::make_unique<radar::PhaseUnwrapStage>(m.radar,params);
  std::ifstream in(dir/"radar.bin",std::ios::binary), index(dir/"radar.bin.frames.csv");
  std::ofstream out(std::filesystem::u8path(args[2]));
  if(!in||!index||!out)throw std::runtime_error("cannot open session files");
  out<<std::setprecision(10)<<"file_frame,displacement_mm,track_bin,track_amp,segment\n";
  auto raw=std::make_shared<std::vector<std::uint8_t>>(m.radar.bytesPerFrame);
  std::string line;std::getline(index,line);std::uint64_t count=0,previous=0,segment=0;
  while(std::getline(index,line)){
   std::istringstream row(line);std::string file,wire;std::getline(row,file,',');std::getline(row,wire,',');
   auto id=std::stoull(wire);
   if(std::stoull(file)!=count)throw std::runtime_error("non-contiguous file frame index");
   if(count && id!=previous+1){++segment;phase=std::make_unique<radar::PhaseUnwrapStage>(m.radar,params);}
   previous=id;
   if(!in.read(reinterpret_cast<char*>(raw->data()),raw->size()))throw std::runtime_error("BIN shorter than index");
   radar::FrameContext ctx;ctx.frameSeq=count;ctx.raw=raw;
   if(!parse.process(ctx)||!range.process(ctx)||!phase->process(ctx)||!ctx.valid)throw std::runtime_error("invalid DSP frame");
   out<<count++<<','<<ctx.displacementMm<<','<<ctx.phaseTrackBin<<','<<ctx.phaseTrackAmp<<','<<segment<<'\n';
   if(count>30000)throw std::runtime_error("preview limited to 30000 frames");
  }
  if(in.peek()!=std::char_traits<char>::eof())throw std::runtime_error("BIN longer than index");
  out.flush();if(!out||!count)throw std::runtime_error("empty session or output write failed");
  std::cout<<"frames="<<count<<" range_bin_m="<<m.radar.rangeIdxToMeters<<" period_ms="<<m.radar.framePeriodicityMs<<'\n';
  return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
int main(int argc,char**argv){
 std::vector<std::string> args;
#ifdef _WIN32
 int n;auto wide=CommandLineToArgvW(GetCommandLineW(),&n);
 if(!wide)return 1;
 for(int i=0;i<n;++i){int size=WideCharToMultiByte(CP_UTF8,0,wide[i],-1,nullptr,0,nullptr,nullptr);std::string s(size,'\0');WideCharToMultiByte(CP_UTF8,0,wide[i],-1,s.data(),size,nullptr,nullptr);s.pop_back();args.push_back(s);}LocalFree(wide);
#else
 args.assign(argv,argv+argc);
#endif
 return run(args);
}
