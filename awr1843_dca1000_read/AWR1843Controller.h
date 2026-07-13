#pragma once
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <chrono>
#include <stdexcept>
#include <functional>  // 用于回调函数

// 引入用户提供的串口库
#include "WzSerialportPlus.h"
#include "WzSerialPortAdapter.h"

namespace Radar{
    // 魔术字定义
    const uint8_t MAGIC_WORD[] = { 0x02, 0x01, 0x04, 0x03, 0x06, 0x05, 0x08, 0x07 };
    const size_t MAGIC_WORD_SIZE = 8;

    // TLV消息类型
    enum TlvType {
        MSG_DETECTED_POINTS = 1,
        MSG_RANGE_PROFILE = 2,
        MSG_NOISE_PROFILE = 3,
        MSG_AZIMUT_STATIC_HEAT_MAP = 4,
        MSG_POINT_CLOUD_2D = 6,
        MSG_RANGE_DOPPLER_HEAT_MAP = 5
    };

    // 配置参数结构体
    struct ConfigParams {
        int numDopplerBins;
        int numRangeBins;
        float rangeResolutionMeters;
        float rangeIdxToMeters;
        float dopplerResolutionMps;
        float maxRange;
        float maxVelocity;
        float sleepTime;
        int num_rx_ant;
        int num_tx_ant;
    };

    // 解析后的帧数据结构
    struct ProcessedFrame {
        int parser_result;
        size_t headerStartIndex;
        std::string platform;
        uint32_t frameNumber;
        uint32_t timeCpuCycles;
        size_t totalPacketNumBytes;
        int numDetObj;
        int numTlv;
        int subFrameNumber;

        std::vector<float> detectedX;
        std::vector<float> detectedY;
        std::vector<float> detectedZ;
        std::vector<float> detectedV;
        std::vector<float> detectedRange;
        std::vector<float> detectedAzimuth;
        std::vector<float> detectedElevAngle;
        std::vector<float> detectedSNR;
        std::vector<float> detectedNoise;

        std::vector<int16_t> rangeProfile;
        std::vector<int16_t> noiseFloor;
        std::vector<int16_t> azimuthHeatMap;
        std::vector<int16_t> rangeDoppler;

        std::vector<uint32_t> stats;
        std::vector<float> temperatureStats;

        ConfigParams configParams;
    };

    class AWR1843Controller {
    private:
        bool connected;
        bool initialFrameSent;
        bool verbose;
        int mode;
        std::string dataLoc;  // 数据串口名称（如"COM2"）
        int dataBaud;         // 数据串口波特率
        float sdkVersion;

        // 串口对象（使用用户提供的WzSerialportPlus库）
        WzSerialPortAdapter cliPort;  // 配置串口（CLI）
        WzSerialportPlus dataPort; // 数据串口

        // 配置参数
        int numRxAnt;
        int numTxAnt;
        int numVirtualAnt;
        std::vector<std::string> configCommands;
        ConfigParams configParams;

        // 帧配置参数
        int chirpStartIdx;
        int chirpEndIdx;
        int numLoops;
        int numFrames;
        float framePeriodicity;
        int triggerSelect;
        float triggerDelay;

        // 数据缓冲区（线程安全）
        std::vector<uint8_t> byteBuffer;
        size_t byteBufferLength;
        const size_t maxBufferSize = 1 << 15;  // 32768字节缓冲区上限
        std::mutex bufferMutex;  // 保护缓冲区的互斥锁

        // 多线程控制
        bool stopFlag;
        std::thread cliReadThread;  // CLI串口读取线程（用于接收雷达响应）

        // 辅助函数：字符串分割
        std::vector<std::string> split(const std::string& s, char delimiter) {
            std::vector<std::string> tokens;
            std::string token;
            std::istringstream tokenStream(s);
            while (std::getline(tokenStream, token, delimiter)) {
                tokens.push_back(token);
            }
            return tokens;
        }

        // 辅助函数：从4字节计算32位无符号整数（小端）
        uint32_t bytesToUint32(const uint8_t* bytes) {
            return (bytes[0] << 0) | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
        }

        // 辅助函数：从4字节计算32位浮点数（小端）
        float bytesToFloat(const uint8_t* bytes) {
            uint32_t val = bytesToUint32(bytes);
            return *reinterpret_cast<float*>(&val);
        }

        // 内部数据处理函数（替代带userData的回调，通过this访问成员）
        void handleReceivedData(char* data, int length) {
            if (length <= 0) return;

            // 线程安全地将数据写入缓冲区
            std::lock_guard<std::mutex> lock(bufferMutex);
            size_t newLength = byteBufferLength + length;

            // 若缓冲区溢出，保留最新的数据（覆盖旧数据）
            if (newLength > maxBufferSize) {
                size_t shift = newLength - maxBufferSize;
                std::memmove(byteBuffer.data(),
                    byteBuffer.data() + shift,
                    byteBufferLength - shift);
                byteBufferLength -= shift;
                newLength = maxBufferSize;
            }

            // 复制新数据到缓冲区
            std::memcpy(byteBuffer.data() + byteBufferLength,
                data, length);
            byteBufferLength = newLength;
        }

        // CLI串口读取线程（用于接收雷达配置响应）
        void cliReadLoop() {
            std::cout << "cliReadLoop" << std::endl;
           
        }
        // 计算变量中有几个1
        int countSetBits(int x) {
            int count = 0;
            // 循环清除最右边的1，直到x为0
            while (x) {
                x &= x - 1;  // 清除最右边的1
                count++;
            }
            return count;
        }
    public:
        AWR1843Controller(float sdkVersion = 3.0,
            const std::string& cliLoc = "COM6", int cliBaud = 115200,
            const std::string& dataLoc = "COM8", int dataBaud = 921600,
            int numRx = 4, int numTx = 1,
            bool verbose = false, bool connect = true, int mode = 0,
            const std::string& configFile = "./1T1R.cfg")
            : sdkVersion(sdkVersion), numRxAnt(numRx), numTxAnt(numTx),
            verbose(verbose), mode(mode), dataLoc(dataLoc), dataBaud(dataBaud),
            connected(false), initialFrameSent(false),
            byteBufferLength(0), stopFlag(false) 
        {
            numVirtualAnt = numRxAnt * numTxAnt;
            byteBuffer.resize(maxBufferSize, 0);

            if (connect) {
                if (!openSerialPorts(cliLoc, cliBaud, dataLoc, dataBaud)) {
                    throw std::runtime_error("串口打开失败");
                }
                connected = true;
            }

            if (mode == 0) {
                initialize(configFile);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                std::cout << "初始化完成" << std::endl;
            }
        }

        ~AWR1843Controller() {
            close();
        }

        // 打开CLI和数据串口
        bool openSerialPorts(const std::string& cliLoc, int cliBaud,
            const std::string& dataLoc, int dataBaud) {
            // 打开CLI串口（配置端口）
            // 假设WzSerialportPlus的open原型：bool open(const char* name, int baud, int stopBits, int dataBits, char parity);
            bool cliOpen = cliPort.open(cliLoc.c_str(), cliBaud, 1, 8, 'N');
            if (!cliOpen) {
                if (verbose) {
                    std::cerr << "CLI串口 " << cliLoc << " 打开失败" << std::endl;
                }
                return false;
            }

            // 打开数据串口（接收雷达数据）
            bool dataOpen = dataPort.open(dataLoc.c_str(), dataBaud, 1, 8, 'N');
            if (!dataOpen) {
                if (verbose) {
                    std::cerr << "数据串口 " << dataLoc << " 打开失败" << std::endl;
                }
                cliPort.close();
                return false;
            }

            // 注册数据串口接收回调：用lambda捕获this，适配std::function<void(char*, int)>
            // 假设WzSerialportPlus的setReceiveCalback原型：void setReceiveCalback(std::function<void(char*, int)>);
            dataPort.setReceiveCalback(
                [this](char* data, int length) {
                    this->handleReceivedData(data, length);  // 调用内部处理函数
                }
            );

            // 启动CLI串口读取线程（接收配置响应）
            stopFlag = false;
            cliReadThread = std::thread(&AWR1843Controller::cliReadLoop, this);

            return true;
        }

        // 初始化雷达配置
        void initialize(const std::string& configFile) {
            // 读取配置文件
            std::ifstream file(configFile);
            if (!file.is_open()) {
                throw std::runtime_error("配置文件打开失败: " + configFile);
            }

            std::string line;
            while (std::getline(file, line)) {
                // 移除换行符和注释
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line[0] == '%') continue;
                std::cout << line << std::endl;
                configCommands.push_back(line);
            }
            file.close();

            if (connected) {
                configureRadar();
            }

            // 解析配置参数
            parseConfigParams();
        }

        // 配置雷达（发送配置命令）
        void configureRadar() {
            for (const auto& cmd : configCommands) {
                // 跳过空行和注释
                if (cmd.empty() || cmd[0] == '%') continue;
                // 遇到sensorStart停止（后续手动启动）
                if (cmd == "sensorStart") break;

                // 发送命令（添加换行符作为结束符）
                std::string cmdWithNewline = cmd + "\n";
                // 解决const char*转char*的问题：用const_cast转换（发送不修改数据，安全）
                // 假设WzSerialportPlus的send原型：int send(char* data, int len);
                int sendLen = cliPort.send(
                    const_cast<char*>(cmdWithNewline.c_str()),  // 强制转换移除const
                    cmdWithNewline.size()
                );
                if (verbose) {
                    std::cout << "[发送命令] " << cmd << " (长度: " << sendLen << ")" << std::endl;
                }

                // 等待雷达响应
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            initialFrameSent = false;
        }

        // 解析配置参数（计算雷达关键参数）
        void parseConfigParams() {
            float startFreq = 0;
            float idleTime = 0;
            float rampEndTime = 0;
            float freqSlopeConst = 0;
            int numAdcSamples = 0;
            int digOutSampleRate = 0;

            for (const auto& cmd : configCommands) {
                std::vector<std::string> parts = split(cmd, ' ');
                if (parts.empty()) continue;

                if (parts[0] == "profileCfg") {
                    if (parts.size() >= 12) {
                        startFreq = std::stof(parts[2]);
                        idleTime = std::stof(parts[3]);
                        rampEndTime = std::stof(parts[5]);
                        freqSlopeConst = std::stof(parts[8]);
                        numAdcSamples = std::stoi(parts[10]);
                        digOutSampleRate = std::stoi(parts[11]);
                    }
                }
                else if (parts[0] == "frameCfg") {
                    if (parts.size() >= 8) {
                        chirpStartIdx = std::stoi(parts[1]);
                        chirpEndIdx = std::stoi(parts[2]);
                        numLoops = std::stoi(parts[3]);
                        numFrames = std::stoi(parts[4]);
                        framePeriodicity = std::stof(parts[5]);
                        triggerSelect = std::stoi(parts[6]);
                        triggerDelay = std::stof(parts[7]);
                    }
                }
                else if (parts[0] == "channelCfg") {
                    if (parts.size() >= 3) {
                        int rxMask = std::stoi(parts[1]);
                        int txMask = std::stoi(parts[2]);

                        numRxAnt = countSetBits(rxMask);
                        numTxAnt = countSetBits(txMask);
                        numVirtualAnt = numRxAnt * numTxAnt;
                    }
                }
            }

            // 计算2的幂（FFT处理需要）
            int numAdcSamplesRoundTo2 = 1;
            while (numAdcSamples > numAdcSamplesRoundTo2) {
                numAdcSamplesRoundTo2 *= 2;
            }

            // 计算雷达关键参数
            int numChirpsPerFrame = (chirpEndIdx - chirpStartIdx + 1) * numLoops;

            configParams.numDopplerBins = numChirpsPerFrame / numTxAnt;
            configParams.numRangeBins = numAdcSamplesRoundTo2;
            configParams.rangeResolutionMeters = (3e8 * digOutSampleRate * 1e3) /
                (2 * freqSlopeConst * 1e12 * numAdcSamples);
            configParams.rangeIdxToMeters = (3e8 * digOutSampleRate * 1e3) /
                (2 * freqSlopeConst * 1e12 * configParams.numRangeBins);
            configParams.dopplerResolutionMps = 3e8 /
                (2 * startFreq * 1e9 * (idleTime + rampEndTime) * 1e-6 *
                    configParams.numDopplerBins * numTxAnt);
            configParams.maxRange = (300 * 0.9 * digOutSampleRate) /
                (2 * freqSlopeConst * 1e3);
            configParams.maxVelocity = 3e8 /
                (4 * startFreq * 1e9 * (idleTime + rampEndTime) * 1e-6 * numTxAnt);
            configParams.sleepTime = 0.001 * framePeriodicity;
            configParams.num_rx_ant = numRxAnt;
            configParams.num_tx_ant = numTxAnt;
        }

        // 设置帧配置
        void setFrameCfg(int numFrames) {
            if (!connected) return;

            this->numFrames = numFrames;
            std::string cmd = "frameCfg " + std::to_string(chirpStartIdx) + " " +
                std::to_string(chirpEndIdx) + " " +
                std::to_string(numLoops) + " " +
                std::to_string(numFrames) + " " +
                std::to_string(framePeriodicity) + " " +
                std::to_string(triggerSelect) + " " +
                std::to_string(triggerDelay);

            std::string cmdWithNewline = cmd + "\n";
            // 同样使用const_cast处理类型转换
            cliPort.send(
                const_cast<char*>(cmdWithNewline.c_str()),
                cmdWithNewline.size()
            );

            if (verbose) {
                std::cout << "[设置帧配置] " << cmd << std::endl;
            }
        }

        // 启动传感器
        void startSensor() {
            if (!connected) return;

            std::string cmd = initialFrameSent ? "sensorStart 0" : "sensorStart";
            std::string cmdWithNewline = cmd + "\n";
            cliPort.send(
                const_cast<char*>(cmdWithNewline.c_str()),
                cmdWithNewline.size()
            );
            std::cout << "[启动传感器] " << cmd << std::endl;
           /* if (verbose) {
                std::cout << "[启动传感器] " << cmd << std::endl;
            }*/
            initialFrameSent = true;
        }

        // 停止传感器
        void stopSensor() {
            if (!connected) return;

            std::string cmd = "sensorStop\n";
            cliPort.send(
                const_cast<char*>(cmd.c_str()),
                cmd.size()
            );

            if (verbose) {
                std::cout << "[停止传感器] sensorStop" << std::endl;
            }
        }

        // 读取当前缓冲区数据（线程安全）
        std::vector<uint8_t> readBuffer() {
            std::lock_guard<std::mutex> lock(bufferMutex);
            return std::vector<uint8_t>(byteBuffer.begin(), byteBuffer.begin() + byteBufferLength);
        }

        // 清空缓冲区（线程安全）
        void clearBuffer() {
            std::lock_guard<std::mutex> lock(bufferMutex);
            byteBufferLength = 0;
            std::memset(byteBuffer.data(), 0, maxBufferSize);
        }

        // 解析单帧数据
        bool parseData18xx(const std::vector<uint8_t>& readBuffer, ProcessedFrame& frame) {
            const size_t OBJ_STRUCT_SIZE_BYTES = 12;
            const int MMWDEMO_UART_MSG_DETECTED_POINTS = 1;
            const int MMWDEMO_OUTPUT_MSG_RANGE_DOPPLER_HEAT_MAP = 5;
            const size_t tlvHeaderLengthInBytes = 8;
            const size_t pointLengthInBytes = 16;

            bool magicOK = false;
            bool dataOK = false;
            std::vector<uint8_t> tempBuffer = readBuffer;
            size_t bufferLen = tempBuffer.size();

            // 查找魔术字
            if (bufferLen > MAGIC_WORD_SIZE) {
                std::vector<size_t> possibleLocs;
                for (size_t i = 0; i < bufferLen - MAGIC_WORD_SIZE; ++i) {
                    if (std::memcmp(&tempBuffer[i], MAGIC_WORD, MAGIC_WORD_SIZE) == 0) {
                        possibleLocs.push_back(i);
                    }
                }

                if (!possibleLocs.empty()) {
                    size_t startIdx = possibleLocs[0];
                    // 截取从魔术字开始的数据
                    tempBuffer = std::vector<uint8_t>(tempBuffer.begin() + startIdx, tempBuffer.end());
                    bufferLen = tempBuffer.size();
                    magicOK = true;
                }
            }

            // 解析帧数据
            if (magicOK && bufferLen >= 32) {  // 至少包含头部
                size_t idX = 0;

                // 跳过魔术字
                idX += MAGIC_WORD_SIZE;

                // 读取头部信息
                uint32_t version = bytesToUint32(&tempBuffer[idX]);
                idX += 4;

                uint32_t totalPacketLen = bytesToUint32(&tempBuffer[idX]);
                idX += 4;
                frame.totalPacketNumBytes = totalPacketLen;

                uint32_t platformRaw = bytesToUint32(&tempBuffer[idX]);
                idX += 4;
                frame.platform = std::to_string(platformRaw);

                frame.frameNumber = bytesToUint32(&tempBuffer[idX]);
                idX += 4;

                frame.timeCpuCycles = bytesToUint32(&tempBuffer[idX]);
                idX += 4;

                frame.numDetObj = bytesToUint32(&tempBuffer[idX]);
                idX += 4;

                frame.numTlv = bytesToUint32(&tempBuffer[idX]);
                idX += 4;

                frame.subFrameNumber = bytesToUint32(&tempBuffer[idX]);
                idX += 4;

                // 检查数据包完整性
                if (totalPacketLen > bufferLen) {
                    return false;  // 数据不完整，等待后续数据
                }

                // 解析TLV数据
                for (int tlvIdx = 0; tlvIdx < frame.numTlv; ++tlvIdx) {
                    if (idX + tlvHeaderLengthInBytes > totalPacketLen) break;

                    uint32_t tlvType = bytesToUint32(&tempBuffer[idX]);
                    idX += 4;

                    uint32_t tlvLength = bytesToUint32(&tempBuffer[idX]);
                    idX += 4;

                    // 检测到的点数据
                    if (tlvType == MMWDEMO_UART_MSG_DETECTED_POINTS) {
                        frame.detectedX.resize(frame.numDetObj);
                        frame.detectedY.resize(frame.numDetObj);
                        frame.detectedZ.resize(frame.numDetObj);
                        frame.detectedV.resize(frame.numDetObj);

                        for (int objNum = 0; objNum < frame.numDetObj; ++objNum) {
                            if (idX + 16 > totalPacketLen) break;

                            frame.detectedX[objNum] = bytesToFloat(&tempBuffer[idX]);
                            idX += 4;

                            frame.detectedY[objNum] = bytesToFloat(&tempBuffer[idX]);
                            idX += 4;

                            frame.detectedZ[objNum] = bytesToFloat(&tempBuffer[idX]);
                            idX += 4;

                            frame.detectedV[objNum] = bytesToFloat(&tempBuffer[idX]);
                            idX += 4;
                        }
                        dataOK = true;
                    }
                    // 距离-多普勒热图
                    else if (tlvType == MMWDEMO_OUTPUT_MSG_RANGE_DOPPLER_HEAT_MAP) {
                        size_t numBytes = 2 * configParams.numRangeBins * configParams.numDopplerBins;
                        if (idX + numBytes > totalPacketLen) continue;

                        frame.rangeDoppler.resize(numBytes / 2);
                        std::memcpy(frame.rangeDoppler.data(), &tempBuffer[idX], numBytes);
                        idX += numBytes;

                        // 调整多普勒维度顺序
                        size_t half = configParams.numDopplerBins / 2;
                        std::vector<int16_t> rearranged;
                        rearranged.insert(rearranged.end(),
                            frame.rangeDoppler.begin() + half * configParams.numRangeBins,
                            frame.rangeDoppler.end());
                        rearranged.insert(rearranged.end(),
                            frame.rangeDoppler.begin(),
                            frame.rangeDoppler.begin() + half * configParams.numRangeBins);
                        frame.rangeDoppler = rearranged;
                    }
                }
            }

            frame.parser_result = dataOK ? 0 : 1;
            frame.configParams = configParams;
            return dataOK;
        }

        // 批量处理缓冲区数据
        std::vector<ProcessedFrame> postProcessDataBuf(bool verbose = false) {
            if (verbose) {
                std::cout << "开始处理缓冲区数据..." << std::endl;
            }

            std::vector<ProcessedFrame> processedFrames;
            std::vector<uint8_t> data = readBuffer();  // 线程安全地读取缓冲区
            size_t dataLength = data.size();
            size_t totalBytesParsed = 0;

            while (totalBytesParsed < dataLength) {
                ProcessedFrame frame;
                // 查找魔术字
                bool found = false;
                for (size_t i = totalBytesParsed; i <= dataLength - MAGIC_WORD_SIZE; ++i) {
                    if (std::memcmp(&data[i], MAGIC_WORD, MAGIC_WORD_SIZE) == 0) {
                        // 尝试解析从i开始的帧
                        std::vector<uint8_t> frameData(data.begin() + i, data.end());
                        if (parseData18xx(frameData, frame)) {
                            frame.headerStartIndex = i;
                            processedFrames.push_back(frame);
                            // 移动解析指针到当前帧末尾
                            totalBytesParsed = i + frame.totalPacketNumBytes;
                            found = true;
                        }
                        break;
                    }
                }

                if (!found) break;  // 未找到完整帧，退出循环
            }

            if (verbose) {
                std::cout << "处理完成，解析到 " << processedFrames.size() << " 帧数据" << std::endl;
            }

            return processedFrames;
        }

        // 关闭所有资源
        void close() {
            stopFlag = true;
            if (cliReadThread.joinable()) {
                cliReadThread.join();
            }

            stopSensor();
            cliPort.close();
            dataPort.close();
            connected = false;

            if (verbose) {
                std::cout << "已关闭所有串口和线程" << std::endl;
            }
        }
    };
}