#ifndef UDP_RECEIVER_H
#define UDP_RECEIVER_H
#include "net_compat.h"   // 跨平台 socket 兼容层（内部按平台引入 winsock 或 BSD socket）
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>
#include <vector>
#include <condition_variable>
#include "unlock_queue.h"

// Ĭ�����ݰ���С
#define PACKET_SIZE_DEFAULT 1466

// UDP���ݰ��ṹ��
#pragma pack(push, 1)
typedef struct {
    uint32_t seqNum;          // ���к�
    uint8_t byteCnt[6];       // �ֽڼ���
    uint8_t payload[PACKET_SIZE_DEFAULT - 10];  // ��������
} packet_t;
#pragma pack(pop)

// ����ͳ����Ϣ�ṹ��
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
    // ����ģʽö��
    enum class ReceiveMode {
        BLOCKING,      // ����ģʽ
        ASYNC_THREAD,  // �첽�߳�ģʽ
        QUEUE_BASED    // ���ڶ��е��첽ģʽ
    };

    // ���������
    UDPReceiver();
    ~UDPReceiver();

    // ��ʼ��UDP������
    bool Initialize(const std::string& localIP, uint16_t localPort,
        ReceiveMode mode = ReceiveMode::BLOCKING,
        size_t queueSize = 1000);

    // ��ʼ��������
    bool StartReceiving();

    // ֹͣ��������
    void StopReceiving();

    // ��ȡ֡���ݣ�������ʽ��
    bool ReadFrames(uint32_t frameNum, int bytesInFrame, int packetSize,
        int timeout_s, bool sort, std::vector<uint8_t>& result);

    // �Ӷ��л�ȡ֡���ݣ��첽��ʽ��
    bool GetFramesFromQueue(uint32_t frameNum, int bytesInFrame,
        int timeout_s, bool sort, std::vector<uint8_t>& result);

    // ״̬��ѯ
    bool IsReceiving() const { return is_receiving_; }
    ReceiveMode GetCurrentMode() const { return current_mode_; }
    ReceiveStats GetStatistics() const;

    // ����ѡ��
    void SetBufferSize(size_t bufferSize) { buffer_size_ = bufferSize; }
    void SetTimeout(int timeoutMs) { timeout_ms_ = timeoutMs; }

private:
    // �ڲ�ʵ�ַ���
    void ReceiveThreadFunc();
    void ProcessPacket(const packet_t& packet);
    bool ReadDataBlocking(uint32_t frameNum, int bytesInFrame, int packetSize,
        int timeout_s, bool sort, std::vector<uint8_t>& result);
    void SortPackets(const std::vector<uint8_t>& input, uint32_t frameNum,
        int bytesInFrame, int packetSize, int lastFrameRemainBytes,
        std::vector<uint8_t>& output);

    // ������س�Ա
    SOCKET socket_;
    sockaddr_in local_addr_;
    sockaddr_in remote_addr_;

    // �߳̿���
    std::atomic<bool> is_receiving_;
    std::atomic<bool> stop_requested_;
    std::thread receive_thread_;

    // ���ݻ�����
    std::unique_ptr<UnlockQueue<packet_t>> packet_queue_;
    std::vector<uint8_t> frame_buffer_;
    mutable std::mutex buffer_mutex_;
    std::condition_variable data_ready_cv_;

    // ���ò���
    ReceiveMode current_mode_;
    size_t buffer_size_;
    int timeout_ms_;
    uint32_t max_packet_num_;

    // ͳ����Ϣ
    mutable std::mutex stats_mutex_;
    ReceiveStats stats_;

    // ״̬��־
    bool initialized_;
    bool winsock_initialized_;
};

#endif // UDP_RECEIVER_H