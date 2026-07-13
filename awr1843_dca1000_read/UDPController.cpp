#include "UDPController.h"
#include <system_error>
#include <string>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <thread>   
namespace Radar {
    //初始化网络通信库
#ifdef _WIN32
    bool UDPController::wsaInitialized_ = false;
    void UDPController::initializeWSA() {
        if (!wsaInitialized_) {
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
                throw std::system_error(WSAGetLastError(), std::system_category(), "WSAStartup failed");
            }
            std::cout << "初始化库成功" << std::endl;
            wsaInitialized_ = true;
        }
    }
#endif
    void UDPController::_close()
    {
        if (_sockfd != INVALID_SOCKET) {
            closesocket(_sockfd);
            _sockfd = INVALID_SOCKET;
        }
    }

    UDPController::UDPController() {
#ifdef _WIN32
        initializeWSA();
#endif

        _sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (_sockfd == INVALID_SOCKET) {
            throw std::system_error(errno, std::system_category(), "Socket creation failed");
        }
        std::cout << "创建UDP SOCKET 成功" << std::endl;
        // 绑定本地IP和端口（新增代码）
        sockaddr_in localAddr{};
        localAddr.sin_family = AF_INET;
        localAddr.sin_port = htons(4096); // 本地端口，可根据需求修改
        if (inet_pton(AF_INET, _srcIp.c_str(), &(localAddr.sin_addr)) <= 0) {
            closesocket(_sockfd);
            throw std::runtime_error("Invalid local IP address");
        }
        if (bind(_sockfd, reinterpret_cast<sockaddr*>(&localAddr), sizeof(localAddr)) == SOCKET_ERROR) {
            closesocket(_sockfd);
            throw std::system_error(errno, std::system_category(), "Bind failed");
        }

        std::cout << "UDP SOCKET 创建并绑定成功" << std::endl;
        initialized_ = true;
    }
    UDPController::~UDPController() {
        _close();
    }

    std::vector<uint8_t> UDPController::_sendCMD(CommandCode cmd,
        const std::string& length = "0000",
        const std::string& body = "",
        int timeout_sec = 1) {
        // 设置socket超时
        struct timeval tv;
        tv.tv_sec = timeout_sec;   // 秒级超时
        tv.tv_usec = 0;            // 微秒部分设为0
        setsockopt(_sockfd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

        // 创建消息
        std::string message = CONFIG_HEADER + cmd_to_string(cmd) + length + body + CONFIG_FOOTER;
        std::vector<uint8_t> msg_bytes;

        // 将十六进制字符串转换为字节
        for (size_t i = 0; i < message.length(); i += 2) {
            std::string byte_str = message.substr(i, 2);
            uint8_t byte_val = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
            msg_bytes.push_back(byte_val);
            std::cout << "Sending byte: 0x" << std::hex << static_cast<int>(byte_val) << std::dec << std::endl;// 打印发送的字节
        }

        // 发送消息
        bool sent = _sendTo(msg_bytes, _destIp, _destPort);
        if (!sent) {
            throw std::runtime_error("Failed to send command");
        }
        // 等待回传
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        //接收回传
        std::vector<uint8_t> buffer(MAX_SIZE);
        sockaddr_in senderAddr{};
        socklen_t addrLen = sizeof(senderAddr);

        int received = recvfrom(_sockfd,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(MAX_SIZE),
            0,
            reinterpret_cast<sockaddr*>(&senderAddr),
            &addrLen);
        std::cout << "received: " << received << std::endl;
        if (received == SOCKET_ERROR) {
            return {};
        }

        buffer.resize(received);
        return buffer;

    }

    bool UDPController::_sendTo(const std::vector<uint8_t>& data, const std::string& destIp, uint16_t destPort)
    {
        sockaddr_in destAddr{};
        destAddr.sin_family = AF_INET;
        destAddr.sin_port = htons(_destPort);

        // 使用 inet_pton 替代弃用的 inet_addr
        if (inet_pton(AF_INET, destIp.c_str(), &(destAddr.sin_addr)) <= 0) {
            return false; // 无效的目标IP或转换失败
        }
        if (destAddr.sin_addr.s_addr == INADDR_NONE) {
            return false; // 无效的目标IP
        }
        int result = sendto(_sockfd,
            reinterpret_cast<const char*>(data.data()),
            static_cast<int>(data.size()),
            0,
            reinterpret_cast<sockaddr*>(&destAddr),
            sizeof(destAddr));

        return result == static_cast<int>(data.size());
    }

    /*std::vector<uint8_t> UDPController::_recvFromUDP(size_t maxSize)
    {
            std::vector<uint8_t> buffer(maxSize);
            sockaddr_in senderAddr{};
            socklen_t addrLen = sizeof(senderAddr);

            int received = recvfrom(_sockfd,
                reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(maxSize),
                0,
                reinterpret_cast<sockaddr*>(&senderAddr),
                &addrLen);

            if (received == SOCKET_ERROR) {
                return {};
            }

            buffer.resize(received);
            return buffer;
    }*/

}