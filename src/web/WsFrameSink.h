#pragma once
// WsFrameSink.h — 把处理帧实时推送给浏览器的 WebSocket sink。
//
// 与 PhaseCsvSink 一样运行在流水线 worker 上（IResultSink 扇出），但网络
// 发送速度取决于客户端，绝不能让它反压 DSP：consume() 只做序列化 +
// SpscRing::try_push（满则丢帧并计数——显示旁路允许丢帧，数据路径的
// 无损原则不适用于此）；真正的 sendBinary 全部发生在本 sink 自有的
// 发送线程上（遵循 SpscRing.h 中"socket 线程绝不能阻塞"的既有语义）。
//
// 客户端接入时下发一次 meta JSON（文本消息，字段见 WireProtocol.h 的
// buildMetaJson）；之后每帧一条二进制包（布局见 WireProtocol.h）。

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "core/Interfaces.h"
#include "core/RadarConfig.h"
#include "core/SpscRing.h"

namespace ix {
class WebSocketServer;
}

namespace radar {

class WsFrameSink : public IResultSink {
public:
  // port: WebSocket 监听端口；fps 与 sourceName 仅用于 meta JSON。
  WsFrameSink(const RadarConfig &cfg, int port, float fps,
              std::string sourceName);
  ~WsFrameSink() override;

  // 监听端口并启动发送线程。失败（如端口被占）返回 false。
  bool start();
  // 关停：关队列、join 发送线程、停服务器。幂等。
  void stop();

  // worker 线程侧：编码 + 非阻塞入队。
  void consume(const FrameContext &ctx) override;
  void flush() override {} // 网络推送无"最终刷新"语义

  std::uint64_t framesSent() const noexcept { return sent_.load(); }
  std::uint64_t framesDropped() const noexcept { return dropped_.load(); }
  std::size_t clientCount() const;

private:
  void senderLoop();

  RadarConfig cfg_;
  int port_;
  std::string metaJson_;

  std::unique_ptr<ix::WebSocketServer> server_;
  SpscRing<std::vector<std::uint8_t>> q_{64}; // worker -> 发送线程
  std::thread sender_;
  std::atomic<bool> started_{false};
  std::atomic<std::uint64_t> sent_{0};
  std::atomic<std::uint64_t> dropped_{0};
};

} // namespace radar
