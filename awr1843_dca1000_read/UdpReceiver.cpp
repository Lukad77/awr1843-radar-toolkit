
#include "UdpReceiver.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cmath>

UDPReceiver::UDPReceiver()
    : socket_(INVALID_SOCKET)
    , is_receiving_(false)
    , stop_requested_(false)
    , current_mode_(ReceiveMode::BLOCKING)
    , buffer_size_(1024 * 1024) // 默认1MB
    , timeout_ms_(5000)
    , max_packet_num_(1000)
    , initialized_(false)
    , winsock_initialized_(false)
{
    // 初始化统计信息
    stats_ = { 0, 0, 0, 0, 0, 0 };

    // 初始化Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0) {
        winsock_initialized_ = true;
    }
}

UDPReceiver::~UDPReceiver() {
    StopReceiving();

    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    if (winsock_initialized_) {
        WSACleanup();
    }
}

bool UDPReceiver::Initialize(const std::string& localIP, uint16_t localPort,
    ReceiveMode mode, size_t queueSize) {
    if (initialized_) {
        std::cout << "[UDPReceiver] Already initialized" << std::endl;
        return true;
    }

    // 创建socket
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) {
        std::cerr << "[UDPReceiver] Failed to create socket: " << WSAGetLastError() << std::endl;
        return false;
    }

    // 设置socket选项
    int bufSize = buffer_size_;
    if (setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, (char*)&bufSize, sizeof(bufSize)) == SOCKET_ERROR) {
        std::cerr << "[UDPReceiver] Failed to set receive buffer size: " << WSAGetLastError() << std::endl;
    }

    // 绑定地址
    memset(&local_addr_, 0, sizeof(local_addr_));
    local_addr_.sin_family = AF_INET;
    local_addr_.sin_port = htons(localPort);

    if (localIP.empty() || localIP == "0.0.0.0") {
        local_addr_.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    else {
        
        local_addr_.sin_addr.s_addr = inet_addr(localIP.c_str());
    }

    if (bind(socket_, (sockaddr*)&local_addr_, sizeof(local_addr_)) == SOCKET_ERROR) {
        std::cerr << "[UDPReceiver] Bind failed: " << WSAGetLastError() << std::endl;
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }

    // 根据模式初始化队列
    if (mode == ReceiveMode::QUEUE_BASED) {
        packet_queue_ = std::make_unique<UnlockQueue<packet_t>>(queueSize);
    }

    current_mode_ = mode;
    initialized_ = true;

    std::cout << "[UDPReceiver] Initialized successfully on " << localIP
        << ":" << localPort << std::endl;
    return true;
}

bool UDPReceiver::StartReceiving() {
    if (!initialized_) {
        std::cerr << "[UDPReceiver] Not initialized" << std::endl;
        return false;
    }

    if (is_receiving_) {
        std::cout << "[UDPReceiver] Already receiving" << std::endl;
        return true;
    }

    stop_requested_ = false;
    is_receiving_ = true;

    // 启动接收线程（对于异步模式）
    if (current_mode_ == ReceiveMode::QUEUE_BASED) {
        receive_thread_ = std::thread(&UDPReceiver::ReceiveThreadFunc, this);
        std::cout << "[UDPReceiver] Started receiving in queue-based mode" << std::endl;
    }
    else {
        std::cout << "[UDPReceiver] Ready for blocking mode reception" << std::endl;
    }

    return true;
}

void UDPReceiver::StopReceiving() {
    if (!is_receiving_) return;

    stop_requested_ = true;
    is_receiving_ = false;

    // 通知等待的线程
    data_ready_cv_.notify_all();

    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }

    std::cout << "[UDPReceiver] Stopped receiving" << std::endl;
}

bool UDPReceiver::ReadFrames(uint32_t frameNum, int bytesInFrame, int packetSize,
    int timeout_s, bool sort, std::vector<uint8_t>& result) {
    if (!initialized_) {
        std::cerr << "[UDPReceiver] Not initialized" << std::endl;
        return false;
    }

    if (current_mode_ != ReceiveMode::BLOCKING) {
        std::cerr << "[UDPReceiver] This method is for blocking mode only" << std::endl;
        return false;
    }

    return ReadDataBlocking(frameNum, bytesInFrame, packetSize, timeout_s, sort, result);
}

bool UDPReceiver::GetFramesFromQueue(uint32_t frameNum, int bytesInFrame,
    int timeout_s, bool sort, std::vector<uint8_t>& result) {
   
    if (!initialized_ || !packet_queue_) {
        std::cerr << "[UDPReceiver] Not initialized or not in queue mode" << std::endl;
        return false;
    }
    
    uint32_t maxPacketNum = ((frameNum + 1) * bytesInFrame) / (PACKET_SIZE_DEFAULT - 10) + 1;
    size_t requiredBufferSize = maxPacketNum * PACKET_SIZE_DEFAULT;
    // 等待条件：缓冲区数据足够 或 接收停止
    auto timeout = std::chrono::seconds(timeout_s);
  
    // 准备缓冲区
    std::vector<uint8_t> tempBuffer(requiredBufferSize);
    packet_t* buf_ptr = reinterpret_cast<packet_t*>(tempBuffer.data());

    int lastFrameRemainBytes = 0;
    uint32_t ret;
    uint64_t bytesCnt;
    int lastFrameTransferedBytes;

    // 等待下一帧开始
    auto startTime = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(buffer_mutex_);
    do {
        bool ready = data_ready_cv_.wait_for(lock, timeout, [&]() {
            return !packet_queue_->empty() || !is_receiving_;
            });
        if (!ready || !is_receiving_) {
            std::cout << "[UDPReceiver] Queue timeout or stopped" << std::endl;
            return false;
        }
        ret = packet_queue_->Get(buf_ptr, 1);
        if (ret < 1) {
            std::cout << "[UDPReceiver] Queue timeout" << std::endl;
            return false;
        }

        bytesCnt = (buf_ptr->seqNum - 1) * (PACKET_SIZE_DEFAULT - 10);
        lastFrameTransferedBytes = bytesCnt % bytesInFrame;
        lastFrameRemainBytes = (bytesInFrame - lastFrameTransferedBytes) % bytesInFrame;

        // 检查超时
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime).count();
        if (elapsed > timeout_s * 1000) {
            std::cout << "[UDPReceiver] Operation timeout" << std::endl;
            return false;
        }
    } while (lastFrameRemainBytes >= PACKET_SIZE_DEFAULT - 10);

    uint32_t expectedPacketNum = ceil((lastFrameRemainBytes + frameNum * bytesInFrame) /
        static_cast<double>(PACKET_SIZE_DEFAULT - 10));

    // 获取剩余帧数据
    ret = packet_queue_->Get_wait(buf_ptr + 1, expectedPacketNum - 1, timeout_s * 1000);
    if (ret < expectedPacketNum - 1) {
        std::cout << "[UDPReceiver] Incomplete frame data" << std::endl;
    }

    // 处理排序
    if (sort) {
        SortPackets(tempBuffer, frameNum, bytesInFrame, PACKET_SIZE_DEFAULT,
            lastFrameRemainBytes, result);
    }
    else {
        result = std::move(tempBuffer);
    }

    // 更新统计信息
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.receivedPacketNum += ret + 1; // +1 for the first packet
        stats_.totalFrames += frameNum;
    }

    return true;
}

void UDPReceiver::ReceiveThreadFunc() {
    std::cout << "[UDPReceiver] Receive thread started,[tid]:"<<std::this_thread::get_id() << std::endl;

    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    memset(&src, 0, sizeof(src));

    // 设置非阻塞模式
    unsigned long mode = 1;
    if (ioctlsocket(socket_, FIONBIO, &mode) != 0) {
        std::cerr << "[UDPReceiver] Failed to set non-blocking mode: " << WSAGetLastError() << std::endl;
        return;
    }

    packet_t buffer;

    while (!stop_requested_) {
        int n = recvfrom(socket_, reinterpret_cast<char*>(&buffer), PACKET_SIZE_DEFAULT,
            0, (sockaddr*)&src, &src_len);

        if (n > 0) {
            ProcessPacket(buffer);

            // 更新统计
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.receivedPacketNum++;
            }

            // 通知等待的线程
            //data_ready_cv_.notify_one();
        }
        else if (n == 0) {
            // 连接关闭
            break;
        }
        else {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                std::cerr << "[UDPReceiver] Receive error: " << error << std::endl;
                {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    stats_.errorCount++;
                }
            }

            // 短暂休眠避免CPU占用过高
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        data_ready_cv_.notify_all();
    }

    std::cout << "[UDPReceiver] Receive thread stopped" << std::endl;
}

void UDPReceiver::ProcessPacket(const packet_t& packet) {
    if (packet_queue_) {
        packet_queue_->Put(&packet, 1);
        data_ready_cv_.notify_one();
    }
}

bool UDPReceiver::ReadDataBlocking(uint32_t frameNum, int bytesInFrame, int packetSize,
    int timeout_s, bool sort, std::vector<uint8_t>& result) {
    if (packetSize % 2 != 0) {
        std::cerr << "[UDPReceiver] Packet size must be even" << std::endl;
        return false;
    }

    uint32_t maxPacketNum = ((frameNum + 1) * bytesInFrame) / (packetSize - 10) + 1;
    size_t requiredBufferSize = maxPacketNum * packetSize;

    std::vector<uint8_t> tempBuffer(requiredBufferSize);
    int lastFrameRemainBytes = 0;

    // 设置超时
    int TimeOut = timeout_s * 1000;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, (char*)&TimeOut, sizeof(TimeOut));

    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    memset(&src, 0, sizeof(src));

    uint32_t seqNum = 0;
    uint64_t bytesCnt;
    int receivePacketLen = 0, lastFrameTransferedBytes;

    std::unique_lock<std::mutex> lock(buffer_mutex_, std::defer_lock);

    // 等待下一帧开始
    do {
        receivePacketLen = recvfrom(socket_, (char*)tempBuffer.data(), packetSize,
            0, (sockaddr*)&src, &src_len);
        if (receivePacketLen < 0) {
            std::cout << "[UDPReceiver] Receive timeout" << std::endl;
            return false;
        }

        seqNum = *reinterpret_cast<uint32_t*>(tempBuffer.data());
        bytesCnt = (seqNum - 1) * (packetSize - 10);
        lastFrameTransferedBytes = bytesCnt % bytesInFrame;
        lastFrameRemainBytes = (bytesInFrame - lastFrameTransferedBytes) % bytesInFrame;
    } while (lastFrameRemainBytes >= packetSize - 10);

    uint32_t expectedPacketNum = ceil((lastFrameRemainBytes + frameNum * bytesInFrame) /
        static_cast<double>(packetSize - 10));

    // 读取剩余帧数据
    for (uint32_t i = 1; i < expectedPacketNum; i++) {
        size_t idx = i * packetSize;
        receivePacketLen = recvfrom(socket_, (char*)&tempBuffer[idx], packetSize,
            0, (sockaddr*)&src, &src_len);
        if (receivePacketLen < 0) {
            std::cout << "[UDPReceiver] Incomplete data received" << std::endl;
            break;
        }
    }

    // 处理排序
    if (sort) {
        SortPackets(tempBuffer, frameNum, bytesInFrame, packetSize,
            lastFrameRemainBytes, result);
    }
    else {
        result = std::move(tempBuffer);
    }

    // 更新统计
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.receivedPacketNum += expectedPacketNum;
        stats_.totalFrames += frameNum;
        stats_.firstPacketNum = seqNum;
        stats_.lastPacketNum = seqNum + expectedPacketNum - 1;
        stats_.expectedPacketNum = expectedPacketNum;
    }

    return true;
}

void UDPReceiver::SortPackets(const std::vector<uint8_t>& input, uint32_t frameNum,
    int bytesInFrame, int packetSize, int lastFrameRemainBytes,
    std::vector<uint8_t>& output) {
    int payloadSize = packetSize - 10;
    uint32_t packetNum = ceil((lastFrameRemainBytes + frameNum * bytesInFrame) /
        static_cast<double>(packetSize - 10));

    output.resize(frameNum * bytesInFrame);
    uint32_t seqNum = 0, idx = 0;

    // 处理第一个数据包
    uint32_t firstPacketNum = *reinterpret_cast<const uint32_t*>(input.data());
    int cpySize = payloadSize - lastFrameRemainBytes;

    memcpy(output.data(), input.data() + lastFrameRemainBytes, cpySize);

    // 处理其余数据包
    for (uint32_t i = 1; i < packetNum; i++) {
        seqNum = *reinterpret_cast<const uint32_t*>(input.data() + i * packetSize);
        idx = seqNum - firstPacketNum;

        if (idx < 1 || idx > packetNum - 1) continue;

        if (idx == packetNum - 1) {
            memcpy(output.data() + (idx - 1) * payloadSize + cpySize,
                input.data() + i * packetSize + 10,
                frameNum * bytesInFrame - idx * payloadSize + lastFrameRemainBytes);
        }
        else {
            memcpy(output.data() + (idx - 1) * payloadSize + cpySize,
                input.data() + i * packetSize + 10, payloadSize);
        }
    }
}

ReceiveStats UDPReceiver::GetStatistics() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}