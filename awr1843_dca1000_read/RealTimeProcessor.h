#include <thread>
#include <atomic>
#include "Logger.h"
#include <iostream>
#include <complex>
#include "UdpReceiver.h"
#include "DataParser.h"
#include "RadarParams.h"
namespace Radar {
    // 基于队列的异步实时处理
    class RealTimeDataProcessor {
    private:
        UDPReceiver receiver_;
        std::atomic<bool> processing_;
        std::thread processing_thread_;
    private:
        std::string binFilePath_;  // bin文件路径
        std::mutex fileMutex_;     // 保证文件写入线程安全
        // 累积帧数据相关变量
        const int FRAME_BATCH_SIZE = 500;  // 每500帧写入一次
        std::atomic<int> frameCounter_ = 0;  // 当前累积帧数
        std::vector<uint8_t> frameBuffer_;   // 暂存帧数据的缓冲区
        std::mutex bufferMutex_;             // 缓冲区操作锁
        std::uint32_t bytesPerFrame_ = 0;// 帧字节数=numRxAnt*numChirpsPerFrame*numADCSamplels*4
    private:
        Radar::RadarParams params_;
        Radar::DataParser parser_;//数据解析器
        Radar::FrameDataSingleRx parsedSingleRxFrame_;//解析出来的1T1R的一帧数据
        std::vector<Radar::FrameDataSingleRx> totalParsedFrames_;//解析出来的1T1R的指定帧数[FRAME_BATCH_SIZE]的数据
    public:
        void SetBinFilePath(const std::string& path) {
            binFilePath_ = path;
        }
        void SetBytesPerFrame(const std::uint32_t bytesPerFrame) {
            bytesPerFrame_ = bytesPerFrame;
        }
    public:
        RealTimeDataProcessor(Radar::RadarParams params) : processing_(false), params_(params), parser_(params) {

        }

        ~RealTimeDataProcessor() {
            StopProcessing();
        }

        bool Start() {
            // 初始化UDP接收器（队列模式，缓冲区1000个包）
            if (!receiver_.Initialize("192.168.33.30", 4098,
                UDPReceiver::ReceiveMode::QUEUE_BASED, 1024)) {
                std::cerr << "初始化UDP接收器失败!" << std::endl;
                return false;
            }
            std::cout << "UDP接收器已启动..." << std::endl;
            // 启动数据接收
            if (!receiver_.StartReceiving()) {
                std::cerr << "启动接收失败!" << std::endl;
                return false;
            }

            processing_ = true;
            // 启动数据处理线程
            processing_thread_ = std::thread(&RealTimeDataProcessor::ProcessingLoop, this);

            std::cout << "实时数据处理系统启动成功" << std::endl;
            return true;
        }

        void StopProcessing() {
            processing_ = false;
            receiver_.StopReceiving();

            if (processing_thread_.joinable()) {
                processing_thread_.join();
            }

            std::cout << "实时数据处理系统已停止" << std::endl;
        }

    private:


        void ProcessingLoop() {
            std::cout << "process tid:" << std::this_thread::get_id() << "数据处理线程启动" << std::endl;

            while (processing_) {
                std::vector<uint8_t> frameData;
#if 1           
                //1.写入解析以后的数据1T1R
                // 从队列获取一帧数据（超时1秒）
                if (receiver_.GetFramesFromQueue(1, bytesPerFrame_, 1, true, frameData)) {
                    // 加锁保护缓冲区和计数器
                    std::lock_guard<std::mutex> lock(bufferMutex_);
                    if (parser_.parse_FrameData(frameData, parsedSingleRxFrame_)) {
                        
                        std::cout << "解析出来的数据格式为:" << parsedSingleRxFrame_.size() << "*" <<
                            parsedSingleRxFrame_[0].size() << std::endl;
                        totalParsedFrames_.push_back(parsedSingleRxFrame_); // 累积当前帧
                        frameCounter_++;
                    }

                    // 当累积满2000帧时结束循环
                    if (frameCounter_ >= FRAME_BATCH_SIZE) {
                        WriteParsedFramesToFile(); // 写入文件
                        frameCounter_ = 0;     // 重置计数器
                        processing_ = false;
                        break;
                    }
#endif
#if 0            //2.写入原始ADC数据
                    if (receiver_.GetFramesFromQueue(1, bytesPerFrame_, 1, true, frameData)) {
                        // 加锁保护缓冲区和计数器
                        std::lock_guard<std::mutex> lock(bufferMutex_);

                        // 将当前帧数据添加到缓冲区
                        frameBuffer_.insert(frameBuffer_.end(), frameData.begin(), frameData.end());
                        frameCounter_++;

                        // 当累积满2000帧时，写入文件并重置缓冲区
                        if (frameCounter_ >= FRAME_BATCH_SIZE) {
                            std::cout << "[已经累积2000帧，开始写入]" << std::endl;
                            writeBufferToFile();  // 批量写入
                            frameBuffer_.clear();  // 清空缓冲区
                            frameCounter_ = 0;     // 重置计数器
                            processing_ = false;
                            break;
                        }
#endif  
                        // 打印统计信息
                        /*auto stats = receiver_.GetStatistics();*/
                        /*std::cout << "已接收帧数: " << stats.totalFrames
                            << ", 当前累积帧数: " << frameCounter_ << std::endl;*/
                }
                    else {
                        // 避免CPU空转
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
            }
                //StopProcessing();
        }
            // 批量写入缓冲区数据到文件
            void writeBufferToFile() {
                std::lock_guard<std::mutex> fileLock(fileMutex_);  // 确保文件写入线程安全

                std::ofstream binFile(binFilePath_, std::ios::binary | std::ios::app);
                if (!binFile.is_open()) {
                    std::cerr << "无法打开文件: " << binFilePath_ << std::endl;
                    return;
                }

                // 写入缓冲区所有数据
                binFile.write(reinterpret_cast<const char*>(frameBuffer_.data()), frameBuffer_.size());
                if (binFile.good()) {
                    std::cout << "成功写入" << frameCounter_ << "帧数据，大小: "
                        << frameBuffer_.size() << " 字节" << std::endl;
                }
                else {
                    std::cerr << "写入文件失败，数据大小: " << frameBuffer_.size() << " 字节" << std::endl;
                }
            }

            // 将累积的解析帧数据写入二进制文件
            void WriteParsedFramesToFile() {
                std::lock_guard<std::mutex> fileLock(fileMutex_); // 确保线程安全
                std::ofstream outFile(binFilePath_, std::ios::binary); // 二进制模式打开

                if (!outFile.is_open()) {
                    std::cerr << "错误：无法打开文件 " << binFilePath_ << std::endl;
                    return;
                }

                // 遍历所有帧数据并写入
                for (const auto& frame : totalParsedFrames_) { // 每一帧
                    for (const auto& chirp : frame) { // 每一帧中的每个chirp
                        for (const auto& sample : chirp) { // 每个chirp中的每个采样点
                            // 写入complex<float>的原始内存（实部+虚部，共8字节）
                            outFile.write(reinterpret_cast<const char*>(&sample), sizeof(std::complex<float>));
                        }
                    }
                }

                std::cout << "成功写入 " << totalParsedFrames_.size() << " 帧数据到 " << binFilePath_ << std::endl;

            }
    };

}
#pragma once
