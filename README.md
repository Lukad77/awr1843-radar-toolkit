# Windows 雷达与呼吸带数据采集

本分支 `feat/windows-radar-belt-capture` 提供 Windows 本机网页工具：采集 AWR1843 + DCA1000 的原始雷达 BIN，可同时采集 HKH-11C 呼吸带，并可对历史会话进行离线波形回放。没有呼吸带时，选择“仅雷达”即可运行。

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
.\.venv\Scripts\python.exe -m pip install -r tools\capture_service\requirements-replay.txt
.\start-capture.ps1
```

仅采集雷达且不使用离线回放时，可跳过创建虚拟环境和安装依赖，启动脚本会使用系统 Python。已有虚拟环境时无需重建。打开 <http://127.0.0.1:8090/>；网页服务仅监听本机。

## 采集步骤

1. 在网页中选择或填写雷达 `.cfg` 文件。默认 `awr1843.windows.cfg` 为 50 Hz；`awr1843.windows.20hz.cfg` 可用于 20 Hz 采集。
2. 填写雷达 CLI 串口、本机雷达网卡 IP 和 DCA1000 IP。页面示例值来自本机验收，按实际连接修改。
3. 选择“仅雷达”或“雷达 + HKH-11C”。双设备模式需选择当前呼吸带串口及固定档位（1 或 3）；建议先确认佩戴和量程，正式采集期间不调档。
4. 点击“开始采集”。两路先连续预采集；呼吸带确认档位并稳定 3 秒后，以随后收到的下一雷达完整帧建立共同正式区间起点 T₀。点击“结束并保存”，等待页面显示“已完成”。

正式时长默认 30 秒，正式帧数上限默认 0（不限）；两者同时设置时先到者结束，都为 0 时手动结束。预采集不计入正式时长或帧数。雷达 CFG 的 `frameCfg` 帧数须为 0（连续出帧）。网页刷新不会结束后台采集。若端口被占用、设备断开或出现丢包，查看页面日志和该会话的统计文件。

## 输出与检查

每次采集在 `captures/年月日-时分秒-随机ID/` 下生成独立目录，不覆盖旧数据。主要文件：

| 文件 | 用途 |
|---|---|
| `radar.bin` | 连续保存完整雷达 ADC 帧 |
| `radar.bin.frames.csv`、`radar.bin.stats.txt` | 帧号、接收时间及丢包统计 |
| `radar.cfg`、`capture.json`、`session.json` | 本次配置、会话事件及最终状态 |
| `belt_raw.bin`、`belt_samples.csv` | 双设备模式下的呼吸带原始字节与样本 |
| `belt_chunks.csv`、`belt_events.csv`、`belt_quality.json` | 呼吸带接收时间、控制事件及质量统计 |
| `radar_formal.frames.csv`、`belt_formal.samples.csv` | 按共同正式区间导出的原始索引与样本；后者仅双设备模式生成 |

`radar.bin` 包含预采集和收尾的完整帧，正式段以 `session.json.formalWindow` 定义的 `[T₀,T₁)` 及正式索引文件为准。检查 `radar.bin.stats.txt` 的 `missingPackets` 和 `discardedFrames`，以及 `belt_quality.json` 中正式段的端点值、超量程和坏校验。曾出现过呼吸带数据传输正常但大量样本触及 0/1023 的情况，这类记录不宜直接作为有效参考。

两路均记录**同一主机的接收时间**，可按时间戳查看共同时间范围；呼吸带实际接收率不一定恰好为 50 Hz，不能按样本序号一一配对。设备间采样延迟与时钟漂移尚未标定。

## 离线回放与波形初审

完成采集后，在控制台点击“离线回放 / 波形审计”，或打开 <http://127.0.0.1:8090/replay.html>。选择历史会话并点击“生成 / 加载回放”。首次处理会生成缓存，之后相同来源和参数可直接加载；回放期间不能开始新采集。

页面展示雷达相位位移、呼吸带原始 ADC（双设备会话）和带通标准化形态比较。可播放、调速、缩放、选择固定距离单元或 DC 补偿，并调整带通频率；参数变更后需重新加载。数据范围可选“全程”或共同“正式区间”，旧会话没有正式区间时只能查看全程。预览 CSV 按当前范围导出，审计 JSON 保留全程数据和当前范围选择。原始采集文件保持只读，派生结果保存在会话的 `analysis/replay-v1/` 下。

回放用于波形形态初审；两设备的主机接收时间不等于硬件同步精度，也不自动校正设备间延迟。详见 [离线回放说明](docs/OFFLINE_REPLAY.md)。采集参数、异常处理与时间戳说明见 [Windows 采集说明](docs/WINDOWS_CAPTURE.md)。本项目采用 [MIT 许可证](LICENSE)。
