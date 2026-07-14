#include "UDPController.h"
#include <system_error>
#include <string>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <thread>   
namespace Radar {
    //��ʼ������ͨ�ſ�
#ifdef _WIN32
    bool UDPController::wsaInitialized_ = false;
    void UDPController::initializeWSA() {
        if (!wsaInitialized_) {
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
                throw std::system_error(WSAGetLastError(), std::system_category(), "WSAStartup failed");
            }
            std::cout << "��ʼ����ɹ�" << std::endl;
            wsaInitialized_ = true;
        }
    }
#endif
    void UDPController::_close()
    {
        if (_sockfd != INVALID_SOCKET) {
            netcompat::net_close(_sockfd);
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
        std::cout << "����UDP SOCKET �ɹ�" << std::endl;
        // �󶨱���IP�Ͷ˿ڣ��������룩
        sockaddr_in localAddr{};
        localAddr.sin_family = AF_INET;
        localAddr.sin_port = htons(4096); // ���ض˿ڣ��ɸ��������޸�
        if (inet_pton(AF_INET, _srcIp.c_str(), &(localAddr.sin_addr)) <= 0) {
            netcompat::net_close(_sockfd);
            throw std::runtime_error("Invalid local IP address");
        }
        if (bind(_sockfd, reinterpret_cast<sockaddr*>(&localAddr), sizeof(localAddr)) == SOCKET_ERROR) {
            netcompat::net_close(_sockfd);
            throw std::system_error(errno, std::system_category(), "Bind failed");
        }

        std::cout << "UDP SOCKET �������󶨳ɹ�" << std::endl;
        initialized_ = true;
    }
    UDPController::~UDPController() {
        _close();
    }

    std::vector<uint8_t> UDPController::_sendCMD(CommandCode cmd,
        const std::string& length = "0000",
        const std::string& body = "",
        int timeout_sec = 1) {
        // ����socket��ʱ
        struct timeval tv;
        tv.tv_sec = timeout_sec;   // �뼶��ʱ
        tv.tv_usec = 0;            // ΢�벿����Ϊ0
        setsockopt(_sockfd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

        // ������Ϣ
        std::string message = CONFIG_HEADER + cmd_to_string(cmd) + length + body + CONFIG_FOOTER;
        std::vector<uint8_t> msg_bytes;

        // ��ʮ�������ַ���ת��Ϊ�ֽ�
        for (size_t i = 0; i < message.length(); i += 2) {
            std::string byte_str = message.substr(i, 2);
            uint8_t byte_val = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
            msg_bytes.push_back(byte_val);
            std::cout << "Sending byte: 0x" << std::hex << static_cast<int>(byte_val) << std::dec << std::endl;// ��ӡ���͵��ֽ�
        }

        // ������Ϣ
        bool sent = _sendTo(msg_bytes, _destIp, _destPort);
        if (!sent) {
            throw std::runtime_error("Failed to send command");
        }
        // �ȴ��ش�
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        //���ջش�
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

        // ʹ�� inet_pton ������õ� inet_addr
        if (inet_pton(AF_INET, destIp.c_str(), &(destAddr.sin_addr)) <= 0) {
            return false; // ��Ч��Ŀ��IP��ת��ʧ��
        }
        if (destAddr.sin_addr.s_addr == INADDR_NONE) {
            return false; // ��Ч��Ŀ��IP
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