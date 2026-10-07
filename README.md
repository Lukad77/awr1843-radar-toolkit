# Windows 雷达与呼吸带数据采集

本分支 `feat/windows-radar-belt-capture` 提供 Windows 本机网页采集工具：采集 AWR1843 + DCA1000 的原始雷达 BIN，并可同时采集 HKH-11C 呼吸带。没有呼吸带时，选择“仅雷达”即可运行。

Jetson Nano / Linux 采集请使用 [jeston_dev 分支](https://github.com/Lukad77/awr1843-radar-toolkit/tree/jeston_dev)；本 README 只说明 Windows 数据采集软件的使用。

## 设备与环境

- Windows 电脑、Python 3.10+、CMake、Ninja 和 C++17 编译器。
- AWR1843 雷达的 XDS110 串口驱动；DCA1000 通过网线连接电脑，并为雷达网卡设置与采集卡同网段的 IP。
- 同时采集呼吸带时，还需 HKH-11C、CP210x 串口驱动及 pySerial。仅雷达模式无需呼吸带或 pySerial。

串口号会随设备和插拔变化，开始前请在设备管理器确认雷达 CLI 串口和呼吸带串口。本机验收时分别是 COM8 和 COM9，其他电脑不要照搬。

## 安装与启动

在 PowerShell 中执行：

```powershell
git clone --branch feat/windows-radar-belt-capture https://github.com/Lukad77/awr1843-radar-toolkit.git
cd awr1843-radar-toolkit
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r tools\capture_service\requirements.txt
.\start-capture.ps1
```

只采集雷达时，可跳过创建虚拟环境和安装 pySerial，启动脚本会使用系统 Python。打开 <http://127.0.0.1:8090/>；网页服务仅监听本机。

## 采集步骤

1. 在网页中选择或填写雷达 `.cfg` 文件。默认 `awr1843.windows.cfg` 为 50 Hz；`awr1843.windows.20hz.cfg` 可用于 20 Hz 采集。
2. 填写雷达 CLI 串口、本机雷达网卡 IP 和 DCA1000 IP。页面示例值来自本机验收，按实际连接修改。
3. 选择“仅雷达”或“雷达 + HKH-11C”。双设备模式需选择当前呼吸带串口及固定档位（1 或 3）；建议先确认佩戴和量程，正式采集期间不调档。
4. 点击“开始采集”。双设备模式会先启动呼吸带并保留 3 秒稳定段，再启动雷达。点击“结束并保存”，等待页面显示“已完成”。

“帧数上限”为 0 时由网页手动结束；设置上限时，采满后自动结束。网页刷新不会结束后台采集。若端口被占用、设备断开或出现丢包，查看页面日志和该会话的统计文件。

## 输出与检查

每次采集在 `captures/年月日-时分秒-随机ID/` 下生成独立目录，不覆盖旧数据。主要文件：

| 文件 | 用途 |
|---|---|
| `radar.bin` | 连续保存完整雷达 ADC 帧 |
| `radar.bin.frames.csv`、`radar.bin.stats.txt` | 帧号、接收时间及丢包统计 |
| `radar.cfg`、`capture.json`、`session.json` | 本次配置、会话事件及最终状态 |
| `belt_raw.bin`、`belt_samples.csv` | 双设备模式下的呼吸带原始字节与样本 |
| `belt_chunks.csv`、`belt_events.csv`、`belt_quality.json` | 呼吸带接收时间、控制事件及质量统计 |

检查 `radar.bin.stats.txt` 的 `missingPackets` 和 `discardedFrames`，以及 `belt_quality.json` 中正式段的端点值、超量程和坏校验。曾出现过呼吸带数据传输正常但大量样本触及 0/1023 的情况，这类记录不宜直接作为有效参考。

两路均记录**同一主机的接收时间**，可按时间戳查看共同时间范围；呼吸带实际接收率不一定恰好为 50 Hz，不能按样本序号一一配对。设备间采样延迟与时钟漂移尚未标定。

详细的参数、输出字段、异常处理与时间戳说明见 [Windows 采集说明](docs/WINDOWS_CAPTURE.md)。本项目采用 [MIT 许可证](LICENSE)。
