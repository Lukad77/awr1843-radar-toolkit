// main.cpp 
#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include "UDPController.h"
#include <vector>
#include <iomanip>
#include <chrono>
#include <sstream>
#include "AWR1843Controller.h"
#include "Logger.h"
#include "unlock_queue.h"
#include "RealTimeProcessor.h"
#include "RadarParams.h"



std::string getCurrentTimeString() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm = *std::localtime(&now_time);

    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y%m%d_%H%M%S");
    return oss.str();
}


void print_hex(std::vector<uint8_t> data) {
    std::cout << "return:";
    for (auto& byte : data) {
        std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)byte;
    }
    std::cout << std::endl;
    std::cout << std::dec;
}



//每帧的字节数，要根据自己的配置计算：numRxAnt*numChirpsPerFrame*numADCSamplels*4
const uint32_t BYTES_PER_FRAME =1*64*256*4;
/**
 * @brief 主函数，用于控制雷达设备的初始化、配置、数据采集与处理流程。
 *
 * 该函数执行以下主要操作：
 * 1. 初始化DCA1000 UDP控制器并发送复位命令；
 * 2. 创建AWR1843雷达控制器对象，并设置实时数据处理器参数；
 * 3. 发送系统连接及FPGA相关配置命令；
 * 4. 启动记录和传感器，开始接收雷达数据；
 * 5. 等待用户输入以停止处理；
 * 6. 停止记录并关闭设备。
 *
 * @return int 返回程序退出状态码（通常为0表示正常结束）。
 */
#if 0
int main() {

    Radar::RadarParams params;
    params.numADCBits = 16;
    params.numADCSamples = 256;
    params.numChirpsEachFrame = 64;
    params.numRX = 4;
    params.rxIdx = 1;
    params.isReal = false;
    params.numFrame = 2000;
    // 创建DCA1000 UDP控制器实例
    Radar::UDPController* DCA1000 = new Radar::UDPController();

    // 复位ARM设备
    std::vector<uint8_t> res = DCA1000->_sendCMD(Radar::CommandCode::RESET_AR_DEV_CMD_CODE, "0000", "", 1);
    print_hex(res);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // 复位FPGA
    res = DCA1000->_sendCMD(Radar::CommandCode::RESET_FPGA_CMD_CODE, "0000", "", 1);
    print_hex(res);

    // 创建AWR1843雷达控制器实例
    Radar::AWR1843Controller* radar1843 = new Radar::AWR1843Controller();

    // 创建实时数据处理器实例
    Radar::RealTimeDataProcessor processor(params);

    // 修改文件路径设置
    std::string timestamp = getCurrentTimeString();
    std::string file_path = "F:\\RadarData\\adc_" + timestamp + ".bin";
    processor.SetBinFilePath(file_path);

    // 设置每帧字节数
    processor.SetBytesPerFrame(BYTES_PER_FRAME);

    // 连接系统
    res = DCA1000->_sendCMD(Radar::CommandCode::SYSTEM_CONNECT_CMD_CODE, "0000", "", 1);
    print_hex(res);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 配置cf.json的参数
    res = DCA1000->_sendCMD(Radar::CommandCode::CONFIG_FPGA_GEN_CMD_CODE, "0600", "01020102031e", 1);
    print_hex(res);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 配置数据包参数
    res = DCA1000->_sendCMD(Radar::CommandCode::CONFIG_PACKET_DATA_CMD_CODE, "0600", "be0571020000", 1);
    print_hex(res);

    // 读取FPGA版本信息
    std::cout << "Read FPGA Version" << std::endl;
    res = DCA1000->_sendCMD(Radar::CommandCode::READ_FPGA_VERSION_CMD_CODE, "0000", "", 1);
    print_hex(res);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 开始录制数据
    res = DCA1000->_sendCMD(Radar::CommandCode::RECORD_START_CMD_CODE, "0000", "", 1);
    print_hex(res);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 启动雷达传感器
    std::cout << "start sensor" << std::endl;
    radar1843->startSensor();

    // 启动数据处理线程
    if (processor.Start()) {
        std::cout << "按Enter键停止处理..." << std::endl;
        std::cin.get();  // 等待用户输入
        processor.StopProcessing();  // 停止数据处理
    }

    // 停止录制数据
    res = DCA1000->_sendCMD(Radar::CommandCode::RECORD_STOP_CMD_CODE, "0000", "", 1);
    print_hex(res);

    // 关闭雷达设备连接
    radar1843->close();

    // 暂停程序以便查看输出结果
    system("pause");

    return 0;
}

#endif
#if 1
int main() {
    Radar::RadarParams params;
    params.numADCBits = 16;
    params.numADCSamples = 256;
    params.numChirpsEachFrame = 64;
    params.numRX = 4;
    params.rxIdx = 0;//从0开始，0,1,2,3分别为四个天线的索引
    params.isReal = false;
    params.numFrame = 2000;
    Radar::DataParser parser(params);
    const size_t FRAME_SIZE = params.numChirpsEachFrame * params.numADCSamples * params.numRX * 4;
    std::ifstream inputFile("D:\\radar_dataset\\1105\\坐姿轻微晃动.bin", std::ios::binary);
    // 检查文件是否成功打开
    if (!inputFile) {
        std::cerr << "无法打开文件进行读取。" << std::endl;
        return 1;
    }
    std::vector<uint8_t> frameData;
    Radar::FrameDataSingleRx singleRxData;
    frameData.resize(FRAME_SIZE);
    std::cout << "每帧数据大小：" << FRAME_SIZE << std::endl;
    inputFile.read(reinterpret_cast<char*>(frameData.data()), FRAME_SIZE);
    if (parser.parse_FrameData(frameData, singleRxData))
    {
        std::cout << "解析出来的一帧数据大小" << singleRxData.size() << "*" << singleRxData[0].size() << std::endl;
        std::ofstream outputFile("D:\\radar_dataset\\1105\\坐姿轻微晃动.csv", std::ios::out);
        if (!outputFile) {
            std::cerr << "无法创建输出文件" << std::endl;
            return 1;
        }

        // 设置浮点数格式 (6位小数)
        outputFile << std::fixed << std::setprecision(6);

        // 修正：添加行结束符和逗号分隔
        for (const auto& chirp : singleRxData) {
            for (size_t i = 0; i < chirp.size(); ++i) {
                // 关键修正1：在每个采样点后添加逗号（但最后一个采样点除外）
                outputFile << chirp[i].real() << "," << chirp[i].imag();
                if (i < chirp.size() - 1) {
                    outputFile << ","; // 仅在非最后一个采样点后添加逗号
                }
            }
            outputFile << "\n"; // 关键修正2：每个chirp结束后换行
        }
        outputFile.close();
        std::cout << "数据已成功写入: D:\\radar_dataset\\1105\\坐姿轻微晃动.csv" << std::endl;
        std::cout << "总数据点: " << singleRxData.size() * singleRxData[0].size() << " (64*256)" << std::endl;
    }

    inputFile.close();
    //解析第一帧的全部4个接收天线的数据
    Radar::FrameDataAllRx allRxData; // [chirp][rx][sample]

    if (parser.parse_FrameData_AllRX(frameData, allRxData)) {
        std::cout << "成功解析全 Rx 数据: "
            << allRxData.size() << " chirps, "
            << allRxData[0].size() << " RX, "
            << allRxData[0][0].size() << " samples\n";
    }

    system("pause");
    
    return 0;
}
#endif