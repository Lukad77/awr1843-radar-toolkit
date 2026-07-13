#ifndef UDP_RECEIVER_H
#define UDP_RECEIVER_H
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>
#include <vector>
#include <condition_variable>
#include "unlock_queue.h"

#pragma comment(lib, "ws2_32.lib")

// 默认数据包大小
#define PACKET_SIZE_DEFAULT 1466

// UDP数据包结构体
#pragma pack(push, 1)
typedef struct {
    uint32_t seqNum;          // 序列号
    uint8_t byteCnt[6];       // 字节计数
    uint8_t payload[PACKET_SIZE_DEFAULT - 10];  // 负载数据
} packet_t;
#pragma pack(pop)

// 接收统计信息结构体
struct ReceiveStats {
    uint32_t receivedPacketNum;
    uint32_t firstPacketNum;
    uint32_t lastPacketNum;
    uint32_t expectedPacketNum;
    uint32_t totalFrames;
    uint32_t errorCount;
};

class UDPReceiver {
public:
    // 接收模式枚举
    enum class ReceiveMode {
        BLOCKING,      // 阻塞模式
        ASYNC_THREAD,  // 异步线程模式
        QUEUE_BASED    // 基于队列的异步模式
    };

    // 构造和析构
    UDPReceiver();
    ~UDPReceiver();

    // 初始化UDP接收器
    bool Initialize(const std::string& localIP, uint16_t localPort,
        ReceiveMode mode = ReceiveMode::BLOCKING,
        size_t queueSize = 1000);

    // 开始接收数据
    bool StartReceiving();

    // 停止接收数据
    void StopReceiving();

    // 读取帧数据（阻塞方式）
    bool ReadFrames(uint32_t frameNum, int bytesInFrame, int packetSize,
        int timeout_s, bool sort, std::vector<uint8_t>& result);

    // 从队列获取帧数据（异步方式）
    bool GetFramesFromQueue(uint32_t frameNum, int bytesInFrame,
        int timeout_s, bool sort, std::vector<uint8_t>& result);

    // 状态查询
    bool IsReceiving() const { return is_receiving_; }
    ReceiveMode GetCurrentMode() const { return current_mode_; }
    ReceiveStats GetStatistics() const;

    // 配置选项
    void SetBufferSize(size_t bufferSize) { buffer_size_ = bufferSize; }
    void SetTimeout(int timeoutMs) { timeout_ms_ = timeoutMs; }

private:
    // 内部实现方法
    void ReceiveThreadFunc();
    void ProcessPacket(const packet_t& packet);
    bool ReadDataBlocking(uint32_t frameNum, int bytesInFrame, int packetSize,
        int timeout_s, bool sort, std::vector<uint8_t>& result);
    void SortPackets(const std::vector<uint8_t>& input, uint32_t frameNum,
        int bytesInFrame, int packetSize, int lastFrameRemainBytes,
        std::vector<uint8_t>& output);

    // 网络相关成员
    SOCKET socket_;
    sockaddr_in local_addr_;
    sockaddr_in remote_addr_;

    // 线程控制
    std::atomic<bool> is_receiving_;
    std::atomic<bool> stop_requested_;
    std::thread receive_thread_;

    // 数据缓冲区
    std::unique_ptr<UnlockQueue<packet_t>> packet_queue_;
    std::vector<uint8_t> frame_buffer_;
    mutable std::mutex buffer_mutex_;
    std::condition_variable data_ready_cv_;

    // 配置参数
    ReceiveMode current_mode_;
    size_t buffer_size_;
    int timeout_ms_;
    uint32_t max_packet_num_;

    // 统计信息
    mutable std::mutex stats_mutex_;
    ReceiveStats stats_;

    // 状态标志
    bool initialized_;
    bool winsock_initialized_;
};

#endif // UDP_RECEIVER_H