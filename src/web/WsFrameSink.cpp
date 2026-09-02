#include "web/WsFrameSink.h"

#include <utility>
#include <vector>

#include "ixwebsocket/IXNetSystem.h"
#include "ixwebsocket/IXWebSocket.h"
#include "ixwebsocket/IXWebSocketServer.h"

#include "web/WireProtocol.h"

namespace radar {

WsFrameSink::WsFrameSink(const RadarConfig &cfg, int port, float fps,
                         std::string sourceName)
    : cfg_(cfg), port_(port),
      metaJson_(wire::buildMetaJson(cfg, fps, sourceName)) {
  ix::initNetSystem(); // Windows 上初始化 Winsock；POSIX 为空实现
  server_ = std::make_unique<ix::WebSocketServer>(port_, "0.0.0.0");
  // 库以 USE_ZLIB=OFF 构建，permessage-deflate 不可用；显式关闭以免
  // 与客户端协商出双方都不支持的扩展。
  server_->disablePerMessageDeflate();
  server_->setOnClientMessageCallback(
      [this](std::shared_ptr<ix::ConnectionState>, ix::WebSocket &ws,
             const ix::WebSocketMessagePtr &msg) {
        if (msg->type == ix::WebSocketMessageType::Open)
          ws.sendText(metaJson_); // 接入即下发一次元数据
        // 首期忽略客户端消息；断开/错误由服务器内部清理连接表。
      });
}

WsFrameSink::~WsFrameSink() { stop(); }

bool WsFrameSink::start() {
  if (started_.exchange(true))
    return true;
  auto res = server_->listen();
  if (!res.first) {
    started_.store(false);
    return false; // 端口被占等；原因在 res.second，由调用方打印提示
  }
  server_->start();
  sender_ = std::thread(&WsFrameSink::senderLoop, this);
  return true;
}

void WsFrameSink::stop() {
  if (!started_.exchange(false))
    return;
  q_.close(); // 唤醒发送线程并排空
  if (sender_.joinable())
    sender_.join();
  server_->stop();
}

void WsFrameSink::consume(const FrameContext &ctx) {
  if (!started_.load())
    return;
  std::vector<std::uint8_t> pkt = wire::encodeFramePacket(cfg_, ctx);
  if (pkt.empty())
    return; // 无效帧/产物缺失/形状漂移：encodeFramePacket 已守门
  if (!q_.try_push(std::move(pkt)))
    dropped_.fetch_add(1); // 客户端太慢：丢帧但绝不阻塞 DSP worker
}

std::size_t WsFrameSink::clientCount() const {
  return server_ ? server_->getClients().size() : 0;
}

void WsFrameSink::senderLoop() {
  std::vector<std::uint8_t> pkt;
  while (q_.pop(pkt)) { // close() 且排空后返回 false
    auto clients = server_->getClients();
    if (clients.empty())
      continue; // 无人观看：直接消费掉，避免队列在后台涨满
    const std::string payload(reinterpret_cast<const char *>(pkt.data()),
                              pkt.size());
    for (auto &c : clients)
      c->sendBinary(payload); // 逐连接排队发送（库内部有各自缓冲）
    sent_.fetch_add(1);
  }
}

} // namespace radar
