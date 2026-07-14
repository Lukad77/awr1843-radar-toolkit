// test_udp_main.cpp — 跨平台 UDP 采集链路测试入口（不依赖串口/DCA1000 命令通道）
//
// 目的：配合 Python 回放泵(dca1000_replay_pump.py)，在没有实体雷达时验证
//       “UDP 接收 → seqNum 重组 → DataParser 解析” 这条链路。
//
// 它只用到已完成跨平台改造的组件：UDPReceiver / DataParser / UnlockQueue，
// 完全不碰 Windows-only 的 WzSerialport / AWR1843Controller，因此在 Mac(clang)
// 与 Windows(MSVC) 上都能整体编译。
//
// 与原 main 的区别：
//   * bind 地址默认 0.0.0.0（原代码硬编码 192.168.33.30，测试机上不存在）。
//   * 不发任何 DCA1000 配置命令、不开串口。
//   * 持续取帧解析并打印帧号/维度，Ctrl-C 优雅退出。
//
// 编译(Mac/Linux)：
//   clang++ -std=c++17 -pthread test_udp_main.cpp UdpReceiver.cpp DataParser.cpp -o test_udp
// 运行：
//   ./test_udp            # 默认 bind 0.0.0.0:4098, frame_bytes=65536
//   ./test_udp 0.0.0.0 4098 65536
//
// 然后另开终端跑回放泵：
//   python3 dca1000_replay_pump.py --bin your.bin --host 127.0.0.1 --port 4098 \
//           --frame-bytes 65536 --fps 30 --loop

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "UdpReceiver.h"
#include "DataParser.h"
#include "RadarParams.h"

namespace {
std::atomic<bool> g_running{true};
void on_signal(int) { g_running.store(false); }
}  // namespace

int main(int argc, char** argv) {
    // ---- 可配置参数（命令行覆盖）----
    std::string bindIP = "0.0.0.0";   // 测试机上应 bind 通配地址而非 192.168.33.30
    uint16_t    bindPort = 4098;      // 与接收端默认端口一致
    // 每帧字节数：默认 numRX(1) * numChirps(64) * numADCSamples(256) * 4 = 65536
    uint32_t    bytesPerFrame = 1u * 64u * 256u * 4u;

    if (argc >= 2) bindIP = argv[1];
    if (argc >= 3) bindPort = static_cast<uint16_t>(std::stoul(argv[2]));
    if (argc >= 4) bytesPerFrame = static_cast<uint32_t>(std::stoul(argv[3]));

    // ---- 雷达参数（与回放的 bin 一致；单 Rx 解析）----
    Radar::RadarParams params;
    params.numADCBits = 16;
    params.numADCSamples = 256;
    params.numChirpsEachFrame = 64;
    params.numRX = 4;
    params.rxIdx = 0;          // 解析第 0 个接收天线
    params.isReal = false;
    params.numFrame = 2000;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::cout << "[test_udp] bind " << bindIP << ":" << bindPort
              << ", bytesPerFrame=" << bytesPerFrame << std::endl;
    std::cout << "[test_udp] 按 Ctrl-C 停止" << std::endl;

    UDPReceiver receiver;
    if (!receiver.Initialize(bindIP, bindPort,
                             UDPReceiver::ReceiveMode::QUEUE_BASED, 4096)) {
        std::cerr << "[test_udp] UDPReceiver 初始化失败" << std::endl;
        return 1;
    }
    if (!receiver.StartReceiving()) {
        std::cerr << "[test_udp] 启动接收失败" << std::endl;
        return 1;
    }

    Radar::DataParser parser(params);
    Radar::FrameDataSingleRx frame;

    uint64_t okFrames = 0;
    uint64_t failFrames = 0;

    while (g_running.load()) {
        std::vector<uint8_t> frameData;
        // 从队列取 1 帧，超时 1s，开启 seqNum 排序重组
        if (receiver.GetFramesFromQueue(1, static_cast<int>(bytesPerFrame), 1, true, frameData)) {
            if (parser.parse_FrameData(frameData, frame) && !frame.empty()) {
                ++okFrames;
                // 每 30 帧打印一次维度，避免刷屏
                if (okFrames % 30 == 1) {
                    std::cout << "[test_udp] frame #" << okFrames
                              << " 解析成功: " << frame.size() << " chirps x "
                              << frame[0].size() << " samples" << std::endl;
                }
            } else {
                ++failFrames;
                std::cout << "[test_udp] frame 解析失败(#" << failFrames << ")" << std::endl;
            }
        }
        // GetFramesFromQueue 内部已带超时/等待，这里不额外 sleep
    }

    receiver.StopReceiving();
    auto stats = receiver.GetStatistics();
    std::cout << "\n[test_udp] 退出统计:" << std::endl;
    std::cout << "  解析成功帧: " << okFrames << std::endl;
    std::cout << "  解析失败帧: " << failFrames << std::endl;
    std::cout << "  收到包总数: " << stats.receivedPacketNum << std::endl;
    std::cout << "  接收错误数: " << stats.errorCount << std::endl;
    return 0;
}
