#include "transport/Dca1000Control.h"
#include <atomic>
#include <iostream>
#include <thread>
#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

int main() {
#ifdef _WIN32
  WSADATA w{}; if (WSAStartup(MAKEWORD(2,2), &w)) return 1;
#endif
  auto mock = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in addr{}; std::string error;
  radar::resolveIpv4("127.0.0.2", 0, addr, error);
  if (bind(mock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr))) return 1;
#ifdef _WIN32
  int length = sizeof(addr); DWORD timeout = 2000;
  setsockopt(mock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
#else
  socklen_t length = sizeof(addr); timeval timeout{2,0};
  setsockopt(mock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
  getsockname(mock, reinterpret_cast<sockaddr *>(&addr), &length);
  std::atomic<bool> passed{true};
  std::thread responder([&] {
    for (int i=0; i<3; ++i) {
      unsigned char packet[64]{}; sockaddr_in from{}; auto size=length;
      const auto n=recvfrom(mock,reinterpret_cast<char *>(packet),sizeof(packet),0,reinterpret_cast<sockaddr *>(&from),&size);
      if(n<8) {passed=false;return;}
      if (packet[2]==3 && (n!=14 || packet[7]!=2)) passed=false; // two LVDS lanes
      if (packet[2]==11 && (n!=14 || packet[6]!=0xc0 || packet[7]!=5 || packet[8]!=0x35 || packet[9]!=0x0c))
        passed=false; // 1472 bytes, 25 us = 3125 FPGA ticks
      const unsigned char reply[]{0x5a,0xa5,packet[2],packet[3],0,0,0xaa,0xee};
      sendto(mock,reinterpret_cast<const char *>(reply),sizeof(reply),0,reinterpret_cast<sockaddr *>(&from),size);
    }
  });
  try {
    radar::Dca1000LinkOptions o; o.bindIp="127.0.0.1";o.dcaIp="127.0.0.2";
    o.configPort=ntohs(addr.sin_port);o.lvdsLanes=2;o.packetDelayUs=25;
    radar::Dca1000Control control(o);control.open();control.connect();control.configureFpga();control.setPacketDelay();
  } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';passed=false;}
  responder.join();
#ifdef _WIN32
  closesocket(mock);WSACleanup();
#else
  close(mock);
#endif
  std::cout<<(passed ? "control protocol tests passed\n" : "control protocol tests failed\n");
  return passed ? 0 : 1;
}
