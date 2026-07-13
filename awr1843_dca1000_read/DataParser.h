#pragma once

#include <vector>
#include <cstdint>
#include <complex>
#include <mutex>
#include <iostream>
#include <shared_mutex>
#include "RadarParams.h"

namespace Radar{
	// 类型定义：1发4收一帧雷达数据 = [chirp][rx][sample]
	using FrameDataAllRx = std::vector<std::vector<std::vector<std::complex<float>>>>;
	//类型定义：1发1收的一帧雷达数据 = [chirp][sample]
	using FrameDataSingleRx = std::vector <std::vector<std::complex<float>>>;
	class DataParser
	{
	public:
		DataParser(Radar::RadarParams params);
		~DataParser();
		bool parse_FrameData(std::vector<uint8_t> &frameData,FrameDataSingleRx &output);//解交织一帧数据 
		bool parse_FrameData_AllRX(
			const std::vector<uint8_t>& frameData,
			FrameDataAllRx& output);
	private:
		
		//解析单个RX通道的所有ADCSamples
		bool parse_RxChannel(
			const int16_t* rxDataStart,          // 指向该 Rx 数据起始位置（int16）
			int numADCSamples,                   // 必须为偶数
			std::vector<std::complex<float>>& output // 输出 [numADCSamples]
		);
		mutable std::mutex dataMtx_;//数据处理的互斥锁

	private:
		Radar::RadarParams  params_;
		// 雷达参数
		const int bytesPerSample_ = 4;//每个采样点有4个字节（2个int16_t）
		size_t expectSize_;
	
	};
}


