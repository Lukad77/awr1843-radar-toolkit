# AWR1843-DCA1000 雷达数据采集与处理工具

## 项目简介

本项目是一款针对德州仪器（TI）AWR1843毫米波雷达与DCA1000数据采集卡的开源工具，用于实现雷达原始数据的实时采集、解析、处理与存储。支持离线数据解析与转换，可将二进制雷达数据转换为CSV格式便于分析，并提供灵活的参数配置与日志记录功能。

## 功能特点

- **设备控制**：通过UDP协议与DCA1000通信，支持复位、配置、启动/停止采集等命令
- **雷达控制**：通过串口与AWR1843雷达交互，实现传感器启动与关闭
- **数据解析**：支持单天线与全天线数据解析，将原始ADC数据转换为复数形式
- **实时处理**：内置实时数据处理线程，支持批量数据存储
- **数据存储**：支持二进制原始数据与CSV格式数据保存
- **日志系统**：多线程安全的日志记录，支持控制台与文件输出
- **配置灵活**：通过JSON配置文件与代码参数双重控制采集参数

## 环境要求

- 操作系统：Windows（依赖Win32 API串口与网络接口）
- 编译环境：支持C++11及以上标准的编译器（如MSVC、MinGW）
- 依赖库：无第三方库依赖（仅使用C++标准库）
- 硬件要求：AWR1843雷达模块、DCA1000数据采集卡、USB转串口适配器

## 安装步骤

1. 克隆仓库到本地
   ```bash
   git clone https://github.com/yourusername/awr1843-dca1000-reader.git
   cd awr1843-dca1000-reader
   ```

2. 使用支持C++11的IDE（如Visual Studio）打开项目
3. 根据硬件连接修改配置参数（IP地址、串口名称、雷达参数等）
4. 编译项目生成可执行文件

## 使用说明

### 实时数据采集模式

1. 确保AWR1843与DCA1000正确连接并供电
2. 配置网络：将PC与DCA1000连接至同一局域网（默认DCA1000 IP为192.168.33.30）
3. 修改主程序中注释为`#if 0`的`main`函数为启用状态（将`#if 0`改为`#if 1`）
4. 根据实际硬件配置修改雷达参数：
   ```cpp
   Radar::RadarParams params;
   params.numADCBits = 16;         // ADC位数
   params.numADCSamples = 256;     // 每chirp的ADC采样数
   params.numChirpsEachFrame = 64; // 每帧的chirp数
   params.numRX = 4;               // 接收天线数量
   params.rxIdx = 0;               // 目标接收天线索引
   ```
5. 运行程序，数据将保存至指定路径（默认：`F:\\RadarData\\adc_<timestamp>.bin`）
6. 按Enter键停止采集

### 离线数据解析模式

1. 将需要解析的二进制数据文件路径修改至主程序：
   ```cpp
   std::ifstream inputFile("D:\\radar_dataset\\1105\\坐姿轻微晃动.bin", std::ios::binary);
   ```
2. 确保主程序中注释为`#if 1`的`main`函数为启用状态
3. 运行程序，解析后的数据将保存为CSV文件
4. 程序同时支持解析全天线数据并输出解析信息

## 配置文件说明

`cf.json`用于配置DCA1000工作参数：
```json
{
  "DCA1000Config": {
    "dataLoggingMode": "raw",          // 数据记录模式：原始数据
    "dataTransferMode": "LVDSCapture", // 数据传输模式：LVDS捕获
    "dataCaptureMode": "ethernetStream", // 数据捕获模式：以太网流
    "lvdsMode": 2,                     // LVDS模式
    "dataFormatMode": 3,               // 数据格式模式
    "packetDelay_us": 5                // 数据包延迟（微秒）
  }
}
```

## 代码结构说明

| 文件名称 | 功能描述 |
|----------|----------|
| `awr1843_dca1000_read.cpp` | 主程序入口，包含实时采集与离线解析两种模式 |
| `UDPController.h` | DCA1000 UDP通信控制，实现命令发送与响应处理 |
| `AWR1843Controller.h` | AWR1843雷达控制，实现传感器启动与数据处理 |
| `DataParser.cpp` | 雷达数据解析器，支持单天线与全天线数据解析 |
| `DataParser.h` | 解析器头文件，定义解析接口与数据结构 |
| `RealTimeProcessor.h` | 实时数据处理器，实现数据接收、缓存与存储 |
| `Logger.h` | 日志系统，支持多级别日志输出与线程安全 |
| `WzSerialportPlus.cpp` | 串口通信实现，用于与AWR1843雷达交互 |
| `WzSerialPortAdapter.h` | 串口适配器，提供统一的串口操作接口 |
| `cf.json` | DCA1000配置文件 |

## 数据格式说明

- **原始二进制数据**：按帧组织，每帧大小计算方式为`numRxAnt * numChirpsPerFrame * numADCSamples * 4`（4字节/采样点）
- **解析后数据**：以复数形式（I/Q分量）存储，每个采样点包含实部（I）和虚部（Q）
- **CSV格式**：每行对应一个chirp数据，每个采样点以"I,Q"形式表示，采样点间以逗号分隔

## 常见问题排查

1. **UDP连接失败**：检查网络配置是否正确，确保PC与DCA1000 IP在同一网段
2. **串口无法打开**：确认串口名称正确，检查雷达是否正确供电，关闭占用串口的其他程序
3. **数据解析错误**：检查`BYTES_PER_FRAME`定义是否与实际配置一致，确保输入文件完整
4. **文件无法写入**：检查目标路径是否存在，确保程序有写入权限
5. **数据不完整**：增加`FRAME_BATCH_SIZE`参数可减少写入频率，提高数据完整性

## 许可证

本项目采用MIT许可证，详情参见LICENSE文件。

## 致谢

本项目参考了德州仪器AWR1843与DCA1000的官方技术文档，部分通信协议与数据格式解析基于TI提供的SDK示例。