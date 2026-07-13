
#pragma once
#include <string>
namespace Radar {
    struct RadarParams {
        int numADCBits = 16;         // ADC位数
        bool isReal = false;         // 是否为实数数据
        int numChirpsEachFrame = 0;  // 每帧Chirp数
        int numFrame = 0;            // 总帧数
        int numADCSamples = 256;       // 每个Chirp的ADC采样数
        int numRX = 1;               // 接收天线数
        int rxIdx = 1;               // 选中的接收天线索引(多天线的时候用，目前用不上）
    };
}