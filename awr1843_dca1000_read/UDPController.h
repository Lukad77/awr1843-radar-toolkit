#pragma once
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
namespace Radar{


    constexpr const char* CONFIG_HEADER = "5aa5";
    constexpr const char* CONFIG_STATUS = "0000";
    constexpr const char* CONFIG_FOOTER = "aaee";

    // 数值常量
    constexpr uint16_t HEADER_Num = 0xa55a;
    constexpr uint16_t FOOTER_Num = 0xeeaa;
    /**
     * @brief 命令代码枚举类，使用强类型枚举确保类型安全
     */
enum class CommandCode : uint16_t {
        RESET_FPGA_CMD_CODE = 0x0100,           // 复位FPGA命令
        RESET_AR_DEV_CMD_CODE = 0x0200,         // 复位AR设备命令
        CONFIG_FPGA_GEN_CMD_CODE = 0x0300,      // 配置FPGA生成命令
        CONFIG_EEPROM_CMD_CODE = 0x0400,        // 配置EEPROM命令
        RECORD_START_CMD_CODE = 0x0500,          // 开始记录命令
        RECORD_STOP_CMD_CODE = 0x0600,           // 停止记录命令
        PLAYBACK_START_CMD_CODE = 0x0700,        // 开始回放命令
        PLAYBACK_STOP_CMD_CODE = 0x0800,         // 停止回放命令
        SYSTEM_CONNECT_CMD_CODE = 0x0900,        // 系统连接命令
        SYSTEM_ERROR_CMD_CODE = 0x0A00,         // 系统错误命令
        CONFIG_PACKET_DATA_CMD_CODE = 0x0B00,   // 配置数据包命令
        CONFIG_DATA_MODE_AR_DEV_CMD_CODE = 0x0C00, // 配置数据模式AR设备命令
        INIT_FPGA_PLAYBACK_CMD_CODE = 0x0D00,   // 初始化FPGA回放命令
        READ_FPGA_VERSION_CMD_CODE = 0x0E00      // 读取FPGA版本命令
    };

const int MAX_SIZE = 4096;//从UDP获取的最大数据包大小
// 工具函数：把枚举指令转换为字符串


/*
    @brief: 在4096端口上面给DCA1000发送命令进行配置
*/

class UDPController
{
public:
	UDPController();
	~UDPController();
    std::vector<uint8_t> _sendCMD(CommandCode cmd, const std::string& length, const std::string& body, int timeout_sec);//发送命令的方法，调用sendTo进行命令发送
private:

    
    bool _sendTo(const std::vector<uint8_t>& data, const std::string& destIp, uint16_t destPort);//底层UDP发送数据的方法
    //std::vector<uint8_t> _recvFromUDP(size_t maxSize);//从UDP端口获取回发的数据
    void _close();

    std::string cmd_to_string(CommandCode cmd) {
        static const std::unordered_map<CommandCode, std::string> cmd_map = {
            {CommandCode::RESET_FPGA_CMD_CODE, "0100"},
            {CommandCode::RESET_AR_DEV_CMD_CODE, "0200"},
            {CommandCode::CONFIG_FPGA_GEN_CMD_CODE, "0300"},
            {CommandCode::CONFIG_EEPROM_CMD_CODE, "0400"},
            {CommandCode::RECORD_START_CMD_CODE, "0500"},
            {CommandCode::RECORD_STOP_CMD_CODE, "0600"},
            {CommandCode::PLAYBACK_START_CMD_CODE, "0700"},
            {CommandCode::PLAYBACK_STOP_CMD_CODE, "0800"},
            {CommandCode::SYSTEM_CONNECT_CMD_CODE, "0900"},
            {CommandCode::SYSTEM_ERROR_CMD_CODE, "0a00"},
            {CommandCode::CONFIG_PACKET_DATA_CMD_CODE, "0b00"},
            {CommandCode::CONFIG_DATA_MODE_AR_DEV_CMD_CODE, "0c00"},
            {CommandCode::INIT_FPGA_PLAYBACK_CMD_CODE, "0d00"},
            {CommandCode::READ_FPGA_VERSION_CMD_CODE, "0e00"}
        };

        auto it = cmd_map.find(cmd);
        if (it != cmd_map.end()) {
            return it->second;
        }
        return "0000"; // 默认返回未知命令
    }

private:
    SOCKET _sockfd = INVALID_SOCKET;
    
    bool initialized_ = false;
    const std::string _destIp = "192.168.33.180";//dca1000的ip
    const std::string _srcIp = "192.168.33.30";//上位机ip
    //uint16_t localPort_ = 1024;//本地端口
    uint16_t _destPort = 4096;//dca1000的端口
#ifdef _WIN32
    static bool wsaInitialized_;
    static void initializeWSA();
#endif


};

}


