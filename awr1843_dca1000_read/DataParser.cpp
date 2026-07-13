#include "DataParser.h"



Radar::DataParser::DataParser(Radar::RadarParams params):params_(params)
{
    
    expectSize_ = params_.numChirpsEachFrame *params_.numRX* params_.numADCSamples * bytesPerSample_;
   
}

Radar::DataParser::~DataParser()
{
}
/**
 * @brief 线程安全的单帧数据解析
 * @param frameData 原始字节流数据
 * @return 解析是否成功
 **/
bool Radar::DataParser::parse_FrameData(std::vector<uint8_t>& frameData,FrameDataSingleRx &output)
{
    std::unique_lock<std::mutex> lock(dataMtx_);
    

    if (frameData.size() != expectSize_) {
        std::cout << "数组大小与期望不符,期望大小为："<<expectSize_<<"实际为："<< frameData.size() << std::endl;
        return false;
    }
    // 预分配内存以减少运行时分配
   //**这里必须用resize，因为reserve仅预留内存，不会开辟空间，因此会触发数组越界**
    output.resize(params_.numChirpsEachFrame,
        std::vector<std::complex<float>>(params_.numADCSamples));
    //原本按 “1 字节（uint8_t）” 访问的内存，重新解释为按 “2 字节（int16_t）” 访问，避免了逐个字节处理的开销
    const int16_t* dataPtr = reinterpret_cast<const int16_t*>(frameData.data());

    const size_t int16PerRx = params_.numADCSamples * 2;
    const size_t int16PerChirp = int16PerRx * params_.numRX;


    try {
        for (int chirpIdx = 0; chirpIdx < params_.numChirpsEachFrame; ++chirpIdx) {
            size_t chirpStart = chirpIdx * int16PerChirp;
            size_t rxStart = chirpStart + params_.rxIdx * int16PerRx;
            // 复用核心函数进行每个天线数据的解析
            if (!parse_RxChannel(dataPtr + rxStart, params_.numADCSamples, output[chirpIdx])) {
                return false;
            }
        }
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Parse error: " << e.what() << std::endl;
       
        return false;
    }
}

bool Radar::DataParser::parse_FrameData_AllRX(const std::vector<uint8_t>& frameData, FrameDataAllRx& output)
{
    std::unique_lock<std::mutex> lock(dataMtx_);

    if (frameData.size() != expectSize_) {
        std::cout << "数组大小与期望不符,期望大小为：" << expectSize_ << "实际为：" << frameData.size() << std::endl;
        return false;
    }

    const int16_t* dataPtr = reinterpret_cast<const int16_t*>(frameData.data());
    const size_t int16PerRx = params_.numADCSamples * 2;
    const size_t int16PerChirp = int16PerRx * params_.numRX;

    // output预分配为 [chirp][rx][sample]
    output.resize(params_.numChirpsEachFrame,
        std::vector<std::vector<std::complex<float>>>(
            params_.numRX,                          // rx 维度
            std::vector<std::complex<float>>(params_.numADCSamples) // sample 维度
        ));

    try {
        for (int chirpIdx = 0; chirpIdx < params_.numChirpsEachFrame; ++chirpIdx) {
            size_t chirpStart = chirpIdx * int16PerChirp;

            for (int rxIdx = 0; rxIdx < params_.numRX; ++rxIdx) {
                size_t rxStart = chirpStart + rxIdx * int16PerRx;

                // 直接写入 output[chirp][rx] 的连续内存
                if (!parse_RxChannel(dataPtr + rxStart, params_.numADCSamples, output[chirpIdx][rxIdx])) {
                    return false;
                }
            }
        }
        return true;
    }
    catch (...) {
        return false;
    }
}



bool Radar::DataParser::parse_RxChannel(const int16_t* rxDataStart, int numADCSamples, std::vector<std::complex<float>>& output)
{
    if (numADCSamples % 2 != 0) return false; // 应在上层校验

    output.resize(params_.numADCSamples);//预分配内存
    for (int s = 0; s < numADCSamples; s += 2) {
        size_t base = s * 2; // 每 2 个 sample 占 4 int16

        int16_t I0 = rxDataStart[base + 0];
        int16_t I1 = rxDataStart[base + 1];
        int16_t Q0 = rxDataStart[base + 2];
        int16_t Q1 = rxDataStart[base + 3];

        output[s] = std::complex<float>(static_cast<float>(I0), static_cast<float>(Q0));
        output[s + 1] = std::complex<float>(static_cast<float>(I1), static_cast<float>(Q1));
    }
    return true;


    
}
;




