#pragma once
#include "net_compat.h"   // 跨平台 socket 兼容层（统一 SOCKET/INVALID_SOCKET/closesocket 等）
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
namespace Radar{


    constexpr const char* CONFIG_HEADER = "5aa5";
    constexpr const char* CONFIG_STATUS = "0000";
    constexpr const char* CONFIG_FOOTER = "aaee";

    // ��ֵ����
    constexpr uint16_t HEADER_Num = 0xa55a;
    constexpr uint16_t FOOTER_Num = 0xeeaa;
    /**
     * @brief �������ö���࣬ʹ��ǿ����ö��ȷ�����Ͱ�ȫ
     */
enum class CommandCode : uint16_t {
        RESET_FPGA_CMD_CODE = 0x0100,           // ��λFPGA����
        RESET_AR_DEV_CMD_CODE = 0x0200,         // ��λAR�豸����
        CONFIG_FPGA_GEN_CMD_CODE = 0x0300,      // ����FPGA��������
        CONFIG_EEPROM_CMD_CODE = 0x0400,        // ����EEPROM����
        RECORD_START_CMD_CODE = 0x0500,          // ��ʼ��¼����
        RECORD_STOP_CMD_CODE = 0x0600,           // ֹͣ��¼����
        PLAYBACK_START_CMD_CODE = 0x0700,        // ��ʼ�ط�����
        PLAYBACK_STOP_CMD_CODE = 0x0800,         // ֹͣ�ط�����
        SYSTEM_CONNECT_CMD_CODE = 0x0900,        // ϵͳ��������
        SYSTEM_ERROR_CMD_CODE = 0x0A00,         // ϵͳ��������
        CONFIG_PACKET_DATA_CMD_CODE = 0x0B00,   // �������ݰ�����
        CONFIG_DATA_MODE_AR_DEV_CMD_CODE = 0x0C00, // ��������ģʽAR�豸����
        INIT_FPGA_PLAYBACK_CMD_CODE = 0x0D00,   // ��ʼ��FPGA�ط�����
        READ_FPGA_VERSION_CMD_CODE = 0x0E00      // ��ȡFPGA�汾����
    };

const int MAX_SIZE = 4096;//��UDP��ȡ��������ݰ���С
// ���ߺ�������ö��ָ��ת��Ϊ�ַ���


/*
    @brief: ��4096�˿������DCA1000���������������
*/

class UDPController
{
public:
	UDPController();
	~UDPController();
    std::vector<uint8_t> _sendCMD(CommandCode cmd, const std::string& length, const std::string& body, int timeout_sec);//��������ķ���������sendTo���������
private:

    
    bool _sendTo(const std::vector<uint8_t>& data, const std::string& destIp, uint16_t destPort);//�ײ�UDP�������ݵķ���
    //std::vector<uint8_t> _recvFromUDP(size_t maxSize);//��UDP�˿ڻ�ȡ�ط�������
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
        return "0000"; // Ĭ�Ϸ���δ֪����
    }

private:
    SOCKET _sockfd = INVALID_SOCKET;
    
    bool initialized_ = false;
    const std::string _destIp = "192.168.33.180";//dca1000��ip
    const std::string _srcIp = "192.168.33.30";//��λ��ip
    //uint16_t localPort_ = 1024;//���ض˿�
    uint16_t _destPort = 4096;//dca1000�Ķ˿�
#ifdef _WIN32
    static bool wsaInitialized_;
    static void initializeWSA();
#endif


};

}


