# Windows 网页控制与可选呼吸带同步采集

分支：`feat/windows-radar-belt-capture`。

## 启动

需要 Windows、Python 3.10+、CMake 和 C++17 编译器。仅雷达模式无需 pip 包；HKH-11C 使用项目虚拟环境中的 pySerial 3.5。

```powershell
cd awr1843-radar-toolkit
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6
# 首次使用呼吸带时安装，项目已有 .venv 则不必重建
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r tools\capture_service\requirements.txt
.\start-capture.ps1
```

打开 <http://127.0.0.1:8090/>。服务仅绑定本机，POST 使用同源校验和会话令牌，不对局域网开放控制。
当前电脑已安装 XDS110 驱动；雷达连接时实测 CLI 为 COM8（115200 8N1），COM7 为辅助串口，重插后应确认端口；原始 ADC 通过 DCA1000 网口采集，不通过 COM7。
网页打开后选择 CFG 或填写本机完整路径；文件选择会在 `captures/configs/` 保存副本并回填路径。
中文 CFG 路径受支持。每次开始都会复制选定文件，采集期间修改原文件不会影响本次会话。

默认使用 `awr1843.windows.cfg`：单 TX、4 RX、256 点、64 chirps、20 ms（50 Hz），每帧 262144 字节。原 20 Hz 配置另存为 `awr1843.windows.20hz.cfg`。
原 `awr1843.cfg` 保留，未被修改。原示例 10 ms（100 Hz）在本机真机测试中出现丢包，尚不能承诺该速率完整采集。
网页默认双 LVDS 通道、50 μs 包间隔；更改采样率后须重新验证链路吞吐和丢包，不能无限增大发包间隔。

本机网卡 `192.168.33.30`、DCA1000 `192.168.33.180`；命令/数据端口默认 4096/4098。
Windows 防火墙需允许 `build/radar_capture.exe` 接收该设备的 UDP 数据。本机已有程序放行规则；本次未成功新增防火墙规则。

## 操作与结果

1. 模式选择“仅雷达”，无需连接或安装呼吸带。
2. 确认 CFG、CLI 串口、网卡/DCA1000 地址，点击“开始采集”。
3. 准备与预采集期间保持“准备与预采集”；稳定后的下一雷达完整帧建立共同 T₀，才进入“采集中”。正式时长默认 30 秒；也可点击“结束并保存”。
4. 每次生成 `captures/年月日-时分秒-随机ID/`，不会覆盖前次数据。

| 文件 | 内容 |
|---|---|
| `radar.bin` | 连续存放完整 ADC 帧，不加入自定义头或时间戳 |
| `radar.bin.frames.csv` | `file_frame,wire_frame,host_rx_monotonic_ns` |
| `radar.bin.stats.txt` | 保存帧数、帧大小、丢失/迟到/非法包及丢弃帧 |
| `radar.cfg` | 本次雷达配置快照 |
| `capture.json` | 本次传输参数，可用于复现 |
| `session.json` | 模式、时间基准、启动/停止事件、最终状态与统计 |
| `capture.log` | 硬件配置、启停发送/应答、首包/首帧及共同区间事件 |
| `radar_formal.frames.csv` | 正式雷达帧在完整 BIN 中的原始索引，加上相对共同 T₀ 的时间 |
| `belt_formal.samples.csv` | 按同一个 [T₀,T₁) 导出的呼吸带样本；仅双设备会话生成 |

网页正式时长默认 30 秒，正式帧数上限默认 0（不限）。两者同时设置时先到者结束；都为 0 时手动结束。CFG 的 frameCfg 帧数必须为 0（连续出帧），有限帧 CFG 会在启动硬件前拒绝，避免预采集消耗正式帧预算。网页计数为正式帧数，BIN 大小包含全部预采集和尾部数据。
停止时先停 DCA1000，再停雷达，接收线程保持工作直至收尾排空。仅完整帧落盘；结束时在途不完整尾帧不写入 BIN。
缺包、磁盘写入失败、连续 15 秒没有完整帧、设备控制失败会报告失败并保留已有数据，不用补零伪装完整数据。
错误会话可保留部分有效帧，须结合 `wire_frame` 和统计判断数据连续性。
网页刷新不停止后台采集；在服务终端按 Ctrl-C 会请求结束当前会话。强制杀死服务时子进程检测 stdin 断开后停止硬件，但最终 session.json 可能来不及更新。

原生终端也可使用（输出路径须不存在）：

```powershell
.\build\radar_capture.exe --cfg awr1843.windows.cfg --serial COM8 --bind-ip 192.168.33.30 --dca-ip 192.168.33.180 --lvds-lanes 2 --packet-delay-us 50 --output captures\manual.bin
```

终端 Ctrl-C 或 stdin 写入 `stop\n` 都触发正常停止；网页通过 stdin 控制，不用强杀进程作为常规结束手段。

## HKH-11C 呼吸带双设备模式

网页选择“雷达 + HKH-11C”，再选择当前 CP210x 串口和固定档位 1 或 3（默认 3；本机佩戴调整后档位 1 实测无削顶）。端口按 VID 10C4/PID EA60 枚举，不写死 COM9；多设备时需手动选择，采集开始后还会读取并保存厂商设备号原始字节。
“刷新呼吸带串口”只枚举设备，不开测；选择“仅雷达”不会创建呼吸带适配器，也无需安装 pySerial。

HKH-11C 协议由交接文档核对，代码已包含实现；其他电脑运行时无需交接资料目录。协议实现在 `hkh11c_protocol.py`，设备适配器在 `hkh11c.py`，注册接口仍在 `belt.py`。
通过现有 Python 会话适配接口复用 pySerial 的 Windows 串口收发；雷达 C++ 控制、重组和落盘继续复用原实现。串口配置 115200/8N1，无流控，打开前将 DTR/RTS 设为 False。

### 共同正式采集区间（syncVersion=2）

开始流程：CFG 校验 → 雷达打开文件/UDP/CLI、配置 DCA1000 并下发 CFG → 雷达 ARMED 等待协调器 → 打开呼吸带并读取设备号 → 请求两路连续预采集 → 呼吸带开测、调档确认并稳定 3 秒 → 检查雷达至少 3 个连续线上完整帧、呼吸带最新量程有效样本距当前不超过 250 ms → 发出正式采集请求 → 请求之后的下一雷达完整帧确定共同 T₀。

READY 只表示雷达 sensorStart 调用已返回，不能据此开始正式计时。T₀ 来自雷达接收线程的真实完整帧时间，不使用 Python 读日志的时间。启动过程即使超过正式时长，也不会消耗正式时长预算。3 秒稳定段是本实验选择，不是厂家保证。

正式区间为半开区间 `[T₀,T₁)`。按时长结束时 T₁=T₀+时长；按帧数上限结束时 T₁=最后正式帧的接收时间+1 ns（包含该帧的边界约定，不代表 1 ns 同步精度）；手动结束时 T₁=原生程序处理 stop 请求时的 QPC 时间。T₁ 在发出任何设备停机命令之前固定。

然后按既有顺序停 DCA1000、停 sensor、排空雷达；呼吸带继续接收以覆盖 T₁，随后 A1 停止并确认。所有预采集/尾部原始数据保留。`session.json.formalWindow` 及正式导出文件是共同区间的权威定义，原始 belt_samples.csv 的 phase 是异步实时阶段标签，不用它重新界定 T₀/T₁。最终质量统计采用区间筛选后的样本，并写入 `beltResult.formal_window_stats`。

在准备/预采集期间点结束，会收尾已经启动的设备并保存取消原因；没有 T₀ 就不会产生伪造的正式区间。雷达没有数据、呼吸带数据过期、断连、配置失败时，不会自动降级为单设备成功；仅雷达模式不访问呼吸带，但也以预采集后的完整雷达帧建立 T₀。

`radar.bin` 仍是完整帧连续存储，包含预采集和尾部，不能再用 `maxFrames * frameBytes` 检查整个 BIN。完整 BIN 大小应等于 `savedFrames * frameBytes`，正式数量以 `formalFrames` 及 `radar_formal.frames.csv` 核验；读取正式第 i 帧时使用其原始 file_frame 定位。

新增诊断包括 sensorStart/sensorStop 实际 WriteFile 前后、首次接受应答、DCA 命令调用前后、首 UDP 包、首完整帧、连续帧就绪、正式请求和 T₀/T₁。呼吸带记录 command_tx/command_tx_end、应答及 first_sample。日志用于区分命令发送、设备响应和数据到达，不等同于硬件采样时间。

协调器通过仅对子进程设置的 `RADAR_SESSION_CONTROL=1` 使用 stdin `start`、`formal <not_before_ns> <duration_ns>`、`stop` 协议；没有该环境变量时原生 CLI 保持直接采集语义。正式窗口导出复用两路原始索引，不平移源时间戳、不做固定 1 秒补偿。

呼吸带读超时为 50 ms、写超时 1 秒、单条应答超时 2 秒；正式采集中连续 3 秒没有完整校验帧会停止会话。串口占用/断开、缺少调档或停止应答、落盘失败均保留错误原因及已有数据。帧超量程/端点命中会保留原值与标记，不截断为 10 位、不自动归一化或补样。

在原雷达会话文件外增加：

| 文件 | 内容 |
|---|---|
| `belt_raw.bin` | 所有收到的原始字节，含应答、坏帧、过渡段 |
| `belt_chunks.csv` | chunk_id、原始偏移、字节数、读取完成时的主机单调时间 |
| `belt_samples.csv` | received_index、原始偏移、首末 chunk、host_rx_monotonic_ns、空 device_timestamp、value_raw、range_ok、rail_hit、gain_commanded、phase |
| `belt_events.csv` | 所有命令发送、应答、档位确认、阶段切换及异常 |
| `belt_quality.json` | 样本数、分阶段量程/端点统计、坏帧计数、停止确认、错误与有效接收率 |
| `session.json` 新字段 | beltConfig（端口、USB 枚举信息、档位、稳定时间）、beltResult、formalPolicy、formalWindow、formal_start/end |

`gain_commanded` 是最近确认的命令参数，不是硬件读回；设备号不猜测整数端序。`received_index` 只表示接收计数，不是设备序号。
网页显示样本数、当前原始值、阶段和档位，以及正式段端点/超量程与全程坏校验。质量标记不直接宣判实验合格，佩戴、削顶和可接受阈值仍需按实验方案确认。

### 时间对齐与扩展

- 呼吸带读取线程在解析和写文件前调用 `context.clock_ns()`（Windows perf_counter_ns），雷达 C++ 使用同一 QPC 时间基准换算为纳秒。跨读取样本使用完成帧所需最后一个 chunk 的时间，原始字节映射保留。
- `origin_monotonic_ns` 与 `origin_utc_ns` 为会话锚点，近似 UTC 为 `origin_utc_ns + sample_ns - origin_monotonic_ns`。主机时间锚点本身存在调用间隔误差。
- 协议没有设备采样时钟或序号，device_timestamp 为空；不按固定 20 ms 人工生成真实时间，也不能用零校验错误宣称绝对无丢样。
- 硬件传输、USB 批处理、主机调度和传感器响应会影响对齐。共同 QPC 不等于硬件同步精度，跨设备延迟/漂移仍需独立校准。
- 新型号继续实现 `prepare/start/status/stop` 并注册；HKH-11C 支持额外的 `mark_phase` 钩子。stop 必须幂等，串口和线程需明确超时，错误返回需被会话保留。

不要强制结束进程代替正常停止：雷达子进程可检测父进程 stdin 断开，但呼吸带在 Python 服务内，强杀服务无法保证发送 A1 或写完质量文件。

## 验证与协议修正

```powershell
ctest --test-dir build --output-on-failure
.\.venv\Scripts\python.exe -m unittest discover -s tools\capture_service -p 'test_*.py' -v
```

新增协议回环测试验证 25 μs 编码为 3125 个 FPGA tick；原实现漏掉 `*1000/8`，已在 Windows/Linux 共用控制层修复。
依据本机 TI mmWave Studio 的 `ReferenceCode/DCA1000/SourceCode/RF_API/rf_api.cpp`（ConfigureRFDCCard_Record）及 [TI 协议字段说明](https://e2e.ti.com/support/sensors-group/sensors/f/sensors-forum/702269/dca1000evm-expected-packet-loss-rate)。
双 LVDS 通道依据 [AWR1843 数据手册](https://www.ti.com/lit/gpn/AWR1843)。

无硬件集成测试通过实际 HTTP 和 Windows 接收进程验证启停、重复会话不覆盖、中文 CFG、BIN 逐字节内容、QPC 时间范围、缺包失败、可选呼吸带生命周期/断连，以及跨站请求拒绝。测试中的被动回放入口通过测试替换注入，不暴露为生产网页选项。
Windows 和 WSL Linux 均通过 9 项 CTest；Python 协议/设备/共同区间/回放回归通过 27 项，其中交接数据回放使用本机 `resp-belt-handoff`（其他电脑可设置环境变量 `HKH11C_HANDOFF`）。

### 先前流程的本机真机验收结果（2026-10-07）

- 50 Hz 双设备采集：雷达 1500 帧（375 MiB），线上帧号 0–1499 连续，丢包与丢帧均为 0；呼吸带档位 1 的正式段 1540 样本，幅值 249–720，端点和超量程均为 0，坏校验为 0。全程实际接收率约 50.76 Hz。样本、原始字节和两路时间覆盖校验通过。
- 20 Hz 双设备档位 3 曾完成雷达零丢包采集，但该次呼吸带正式段有大量端点削顶。调整佩戴及档位后才得到上述有效量程记录；不要把端点削顶的记录当作参考数据。
- 仓库不含受试数据和采集 BIN；会话保存在本机 `captures/`。共同主机时钟便于按接收时间查看两路数据，尚未标定设备间实际采样延迟或漂移。

## 离线波形初审

采集完成后，使用控制台的“离线回放 / 波形审计”入口选择历史会话；支持播放、缩放、固定距离单元与 DC 对照、质量标记和导出。见 [离线回放说明](OFFLINE_REPLAY.md)。

新版共同区间流程已通过本机 HTTP + 原生 UDP 接收进程 + 模拟呼吸带的自动化验收，未在本次代码修改中重新启动真实设备。仍需下一轮真机核对发送/应答/首帧时序、T₀/T₁ 和停止后的时间覆盖；本改动不承诺消除波形中约 1 秒的形态时差。
