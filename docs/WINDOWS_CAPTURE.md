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
3. 配置下发后进入“采集中”；点击“结束并保存”，等待“已完成”。
4. 每次生成 `captures/年月日-时分秒-随机ID/`，不会覆盖前次数据。

| 文件 | 内容 |
|---|---|
| `radar.bin` | 连续存放完整 ADC 帧，不加入自定义头或时间戳 |
| `radar.bin.frames.csv` | `file_frame,wire_frame,host_rx_monotonic_ns` |
| `radar.bin.stats.txt` | 保存帧数、帧大小、丢失/迟到/非法包及丢弃帧 |
| `radar.cfg` | 本次雷达配置快照 |
| `capture.json` | 本次传输参数，可用于复现 |
| `session.json` | 模式、时间基准、启动/停止事件、最终状态与统计 |
| `capture.log` | 硬件配置和采集日志 |

帧数上限 0 表示网页手动结束；CFG 若指定有限帧数，则采到相应上限后自动结束。
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

开始流程：CFG 校验 → 创建会话 → 呼吸带设备号确认 → A0 开测并等待首个样本 → A4 调档并确认 → 保留 3 秒稳定段 → 雷达配置/启动 → 收到 radar READY 后标记 formal。3 秒是交接实验的稳定时间选择，不是厂家稳定性保证。正式段不提供调档操作。
两个设备先后启动，各自原始数据全部保留。`formal_start` 标记服务收到雷达启动确认的时刻，并非硬件共同触发，也不证明雷达已收到首帧；无雷达数据会按原有超时路径失败。

停止流程：雷达先停 DCA1000、再停 sensor → 雷达排空并结束 → 呼吸带 A1 停止并确认 → 有界接收尾部、关闭串口和文件。手动结束时呼吸带切入 tail；自动达到帧数上限时 formal 可包含雷达收尾区间，分析须结合雷达帧时间戳取共同时间范围。`formal_end` 是收尾时的主机事件，不是最后一次硬件采样时刻。
稳定段内点“结束”会取消启动并停止呼吸带，不启动雷达；取消会话以失败/取消原因保留，避免误当成功实验。

呼吸带读超时为 50 ms、写超时 1 秒、单条应答超时 2 秒；正式采集中连续 3 秒没有完整校验帧会停止会话。串口占用/断开、缺少调档或停止应答、落盘失败均保留错误原因及已有数据。帧超量程/端点命中会保留原值与标记，不截断为 10 位、不自动归一化或补样。

在原雷达会话文件外增加：

| 文件 | 内容 |
|---|---|
| `belt_raw.bin` | 所有收到的原始字节，含应答、坏帧、过渡段 |
| `belt_chunks.csv` | chunk_id、原始偏移、字节数、读取完成时的主机单调时间 |
| `belt_samples.csv` | received_index、原始偏移、首末 chunk、host_rx_monotonic_ns、空 device_timestamp、value_raw、range_ok、rail_hit、gain_commanded、phase |
| `belt_events.csv` | 所有命令发送、应答、档位确认、阶段切换及异常 |
| `belt_quality.json` | 样本数、分阶段量程/端点统计、坏帧计数、停止确认、错误与有效接收率 |
| `session.json` 新字段 | beltConfig（端口、USB 枚举信息、档位、稳定时间）、beltResult、formal_start/end |

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
Windows 和 WSL Linux 均通过 9 项 CTest；Python 协议/设备/会话回归通过 15 项，其中交接数据回放使用本机 `resp-belt-handoff`（其他电脑可设置环境变量 `HKH11C_HANDOFF`）。

### 本机验收结果（2026-10-07）

- 50 Hz 双设备采集：雷达 1500 帧（375 MiB），线上帧号 0–1499 连续，丢包与丢帧均为 0；呼吸带档位 1 的正式段 1540 样本，幅值 249–720，端点和超量程均为 0，坏校验为 0。全程实际接收率约 50.76 Hz。样本、原始字节和两路时间覆盖校验通过。
- 20 Hz 双设备档位 3 曾完成雷达零丢包采集，但该次呼吸带正式段有大量端点削顶。调整佩戴及档位后才得到上述有效量程记录；不要把端点削顶的记录当作参考数据。
- 仓库不含受试数据和采集 BIN；会话保存在本机 `captures/`。共同主机时钟便于按接收时间查看两路数据，尚未标定设备间实际采样延迟或漂移。
