# AWR1843-DCA1000 雷达数据采集与处理工具

## 项目简介

本项目是一款针对德州仪器（TI）AWR1843毫米波雷达与DCA1000数据采集卡的开源工具，用于实现雷达原始数据的实时采集、解析、处理与存储。支持离线数据解析与转换，可将二进制雷达数据转换为CSV格式便于分析，并提供灵活的参数配置与日志记录功能。

项目近期完成了核心数据处理流水线的**架构重构**，从原有的ad-hoc实现迁移到全新的**无丢失、顺序保持**的实时处理架构，并在此基础上交付了 Phase 4 **DSP 算子层**（Range/Doppler/Angle FFT、CA-CFAR、MTI 杂波抑制、相位解缠），全部以 `IStage` 插件形式接入、零外部依赖，并用真实数据集完成了端到端验证。

## 功能特点

### 设备控制与数据采集

- **设备控制**：通过UDP协议与DCA1000通信，支持复位、配置、启动/停止采集等命令
- **雷达控制**：通过串口与AWR1843雷达交互，实现传感器启动与关闭
- **数据解析**：支持单天线与全天线数据解析，将原始ADC数据转换为复数形式
- **数据存储**：支持二进制原始数据与CSV格式数据保存
- **配置灵活**：通过JSON配置文件与代码参数双重控制采集参数

### 新架构特性（Phase 0-3 地基）

- **无丢失、顺序保持的实时处理流水线**：从根本上修正了原架构中会静默丢帧/乱序的队列缺陷
- **单一事实源配置管理**：`RadarConfig` 收敛三处漂移的配置为统一来源，通过 `derive()` 计算派生值、`validate()` 校验一致性
- **有界阻塞队列**：`SpscRing` 替代不安全的 `UnlockQueue`，满则回压不丢，提供显式无丢失契约
- **内存池复用**：`BufferPool` 消除每帧分配开销，预分配对象减少运行时分配压力
- **两级无损缓冲**：`FrameSpool`（RAM环 + 磁盘溢写）确保生产者永不阻塞且数据不丢失
- **保序管道执行**：`Pipeline` 单工作线程FIFO消费，输出顺序==提交顺序，无需重排序
- **连续内存解析**：`ParseStage` 替代传统 `DataParser`，写入单一连续分配，无内部互斥锁
- **接口驱动可扩展**：`IFrameSource/IStage/IResultSink/IInferenceEngine` 接口支持依赖倒置，新增处理阶段仅需实现 `IStage`
- **自动化测试**：4个测试套件、11151条断言、100%通过，全部无硬件依赖可在CI运行

### DSP 算子层（Phase 4，src/dsp/）

全部以 `IStage` 插件形式串接到 `Pipeline`，零外部依赖（自研 radix-2 FFT），单帧 DSP 全链 < 2 ms（M 系列单核，30fps 预算余量充足）：

- **Range FFT**：DC 去除 + Hann 窗 + 零填充到 2 的幂，沿连续内存采样轴原位变换
- **Doppler FFT**：跨 chirp 慢时间维 FFT + fftshift（零多普勒居中）+ 4Rx 非相干积累生成 RD 功率图
- **MTI 杂波抑制**（ClutterRemovalStage）：跨帧 EMA 杂波图减除，静止杂波检出降低 ~25%，运动目标无损保留
- **CA-CFAR**：沿距离维 1D 单元平均恒虚警检测，边缘截断窗保持设计 Pfa，局部峰校验去重
- **Angle FFT**：逐检测虚拟阵列快拍零填充 FFT，λ/2 ULA 角度换算（1Tx 支持，TDM-MIMO 留接缝）
- **相位解缠**（PhaseUnwrapStage，⚠️ 实验性）：慢时间相位跟踪与解缠绕，含峰值跟随/相位桥接/Kasa 圆拟合 DC 补偿与 trackAmp 质量位；**真实数据的相位提取正确性尚存疑问，仅供研究，见"已知问题"章节**

## 系统架构

### 整体系统架构图

下图展示硬件设备与上位机之间的三条独立链路：串口控制链、UDP 命令控制链（4096）与 UDP 数据链路（4098）。

```mermaid
graph TB
    subgraph HW["硬件层"]
        RADAR["AWR1843<br/>毫米波雷达"]
        DCA["DCA1000<br/>数据采集卡"]
    end
    subgraph HOST["上位机（Windows / Linux / macOS）"]
        subgraph CTRL["设备控制"]
            SERIAL["串口通信<br/>WzSerialportPlus<br/>AWR1843Controller"]
            UDPCTRL["UDP命令通道<br/>UDPController :4096"]
        end
        subgraph DATA["数据链路"]
            UDPRECV["UDP数据接收<br/>UdpReceiver :4098<br/>seqNum帧重组"]
            PIPE["实时处理流水线<br/>Pipeline + ParseStage"]
        end
        STORE["数据存储<br/>bin / CSV"]
        LOG["日志系统<br/>Logger"]
    end

    SERIAL -->|"CLI配置命令 921600bps"| RADAR
    UDPCTRL -->|"采集/回放配置命令"| DCA
    RADAR -->|"LVDS原始数据"| DCA
    DCA -->|"UDP以太网流"| UDPRECV
    UDPRECV --> PIPE
    PIPE --> STORE
    PIPE -.记录.-> LOG
    CTRL -.记录.-> LOG
```

### 分层架构图

新架构 `src/` 树采用 core / transport / pipeline / dsp 四层划分，依赖单向向下，上层仅依赖下层接口：

```mermaid
graph TB
    subgraph DSP["dsp/ 算子层（Phase 4）"]
        RF["RangeFftStage"]
        DF["DopplerFftStage"]
        CR["ClutterRemovalStage<br/>MTI"]
        CF["CfarStage<br/>CA-CFAR"]
        AF["AngleFftStage"]
        PU["PhaseUnwrapStage<br/>⚠️实验性"]
        FFT["FftPlan<br/>自研radix-2"]
    end
    subgraph PIPELINE["pipeline/ 管道层"]
        PL["Pipeline<br/>保序无损执行器"]
        PS["ParseStage<br/>I/Q解交织"]
    end
    subgraph TRANSPORT["transport/ 传输层"]
        FS["FrameSpool<br/>两级无损缓冲"]
    end
    subgraph CORE["core/ 核心层"]
        RC["RadarConfig<br/>单一事实源"]
        SR["SpscRing&lt;T&gt;<br/>有界阻塞队列"]
        BP["BufferPool&lt;T&gt;<br/>内存池"]
        FB["FrameBuffer<br/>连续内存"]
        FC["FrameContext<br/>工作单元"]
        IF["Interfaces<br/>接口定义"]
        SN["SeqNum<br/>回绕安全序号"]
    end

    RF & DF & CR & CF & AF & PU --> IF
    RF & DF --> FFT
    RF & DF --> BP
    PL --> SR
    PL --> IF
    PS --> RC
    PS --> BP
    PS --> FB
    PS --> IF
    FS -.存储.-> FC
    IF --> FC
    FC --> FB
    BP --> FB
```

## 新架构组件说明

### 核心层（src/core/）

| 组件 | 文件 | 职责 |
|------|------|------|
| **RadarConfig** | `src/core/RadarConfig.{h,cpp}` | 单一事实源；集中管理数据格式参数与frameCfg/profileCfg配置，`derive()` 计算派生值（bytesPerFrame、numRangeBins等），`validate()` 验证配置一致性 |
| **SpscRing&lt;T&gt;** | `src/core/SpscRing.h` | 有界阻塞队列，替代不安全的UnlockQueue；`push()` 满则阻塞（背压传播），`try_push()` 永不阻塞，`pop()` 空则阻塞，`close()` 优雅关闭 |
| **BufferPool&lt;T&gt;** | `src/core/BufferPool.h` | 线程安全对象池，`acquire()` 返回带自定义析构器的shared_ptr，自动归还对象，消除每帧malloc开销 |
| **FrameBuffer** | `src/core/FrameBuffer.h` | 连续、缓存友好的存储，布局为行优先 `[chirp][rx][sample]`，替代三层嵌套vector，适合FFT/CFAR/NN |
| **FrameContext** | `src/core/FrameContext.h` | 流水线工作单元，携带单调uint64 frameSeq（保序/丢帧判据）+ raw/parsed载荷 + 时间戳 |
| **Interfaces** | `src/core/Interfaces.h` | 依赖倒置接缝：`IFrameSource`（产生有序无损帧）、`IStage`（管道步骤）、`IResultSink`（扇出消费者）、`IInferenceEngine`（NN后端抽象） |
| **SeqNum** | `src/core/SeqNum.h` | uint32序列号回绕安全比较（RFC1982/TCP式），防止长跑回绕误判 |

### 传输层（src/transport/）

| 组件 | 文件 | 职责 |
|------|------|------|
| **FrameSpool** | `src/transport/FrameSpool.{h,cpp}` | 两级无损FIFO：Tier 1为有界RAM环形缓冲区，Tier 2溢出到磁盘文件；`push()` 非阻塞且无损，`pop()` 阻塞且保持FIFO顺序；跨RAM→磁盘边界保持严格FIFO |

### 管道层（src/pipeline/）

| 组件 | 文件 | 职责 |
|------|------|------|
| **Pipeline** | `src/pipeline/Pipeline.{h,cpp}` | 顺序保持、无丢失的管道执行器；单工作线程按FIFO顺序弹出帧，依次运行各 `IStage`，扇出到各 `IResultSink`；`submit()` 满则阻塞（背压），`stop()` 优雅排空 |
| **ParseStage** | `src/pipeline/ParseStage.{h,cpp}` | 替代DataParser的管道阶段；将原始DCA1000 int16 I/Q字节解交织为连续FrameBuffer；保持与旧DataParser相同的字节布局，输出匹配旧的bin→CSV黄金标准；无内部mutex，可选BufferPool复用 |

### 算子层（src/dsp/，Phase 4）

算子链推荐顺序（见 `src/tools/radar_dsp_demo.cpp`）：
`Parse → RangeFFT → PhaseUnwrap → ClutterRemoval → DopplerFFT → CA-CFAR → AngleFFT`
（PhaseUnwrap 必须在 ClutterRemoval 之前：相位需要未滤波的 rangeCube，EMA 减除后的残差相位不再满足 φ=4πd/λ）

| 组件 | 文件 | 职责 |
|------|------|------|
| **FftPlan** | `src/dsp/Fft.{h,cpp}` | 自研迭代 radix-2 FFT（零外部依赖）；构造期预计算旋转因子/位反转表，`forward()` 零分配；预留替换 pffft/FFTW 的接缝 |
| **RangeFftStage** | `src/dsp/RangeFftStage.{h,cpp}` | parsed → rangeCube `[chirp][rx][rangeBin]`；DC去除 + Hann窗 + 零填充，沿连续内存行原位FFT，输出池化 |
| **DopplerFftStage** | `src/dsp/DopplerFftStage.{h,cpp}` | rangeCube → dopplerCube（fftshift零多普勒居中）+ rdMap（线性功率，4Rx非相干积累，供CFAR；显示时再转dB） |
| **ClutterRemovalStage** | `src/dsp/ClutterRemovalStage.{h,cpp}` | MTI：按(rx,bin)维护chirp均值的跨帧EMA杂波图并原位减除；静止杂波收敛消失，多普勒旋转目标无损通过 |
| **CfarStage** | `src/dsp/CfarStage.{h,cpp}` | 1D CA-CFAR（逐多普勒行沿距离维）；α=T(pfa^(-1/T)-1) 按实际训练单元数计算，边缘截断保持Pfa；产出 Detection（rangeBin/dopplerBin/rangeM/velocityMps/snrDb） |
| **AngleFftStage** | `src/dsp/AngleFftStage.{h,cpp}` | 逐检测从 dopplerCube 提取虚拟阵列快拍，零填充FFT，sinθ=2k/N（λ/2 ULA）；仅 1Tx，TDM-MIMO 需多普勒相位补偿（留接缝） |
| **PhaseUnwrapStage** | `src/dsp/PhaseUnwrapStage.{h,cpp}` | ⚠️ **实验性**：慢时间相位跟踪/解缠（峰值跟随+迟滞、换bin相位桥接、Kasa圆拟合DC补偿、trackBin/trackAmp质量位）；真实数据相位提取正确性尚在复核 |
| **PhaseCsvSink** | `src/dsp/PhaseCsvSink.h` | IResultSink：逐帧输出 `frameSeq,相位,位移,trackBin,trackAmp` CSV |
| **Detection/CfarParams** | `src/dsp/Detection.h` | 检测记录与 CA-CFAR 参数（guard/training/pfa/maxDetections） |

### 实时处理流水线架构图

下图展示从 UDP 数据包到磁盘落盘的完整数据流，以及 `RadarConfig`、`SpscRing`、`BufferPool`、`FrameSpool`、`Pipeline`、`ParseStage` 等核心组件的交互关系：

```mermaid
flowchart LR
    NET["DCA1000<br/>UDP:4098"]
    RECV["UdpReceiver<br/>帧重组(seqNum)"]
    SPOOL["FrameSpool<br/>RAM环 + 磁盘溢写<br/>两级无损FIFO"]
    RING["SpscRing<br/>有界阻塞队列<br/>满则回压"]
    WORKER["Pipeline worker<br/>单线程FIFO保序"]
    PARSE["ParseStage<br/>I/Q解交织"]
    FB["FrameBuffer<br/>连续内存<br/>[chirp][rx][sample]"]
    SINK["IResultSink 扇出<br/>文件/CSV/WebSocket/指标"]
    POOL["BufferPool<br/>内存池复用"]
    CFG["RadarConfig<br/>单一事实源<br/>derive/validate"]

    NET --> RECV
    RECV -->|"push()非阻塞无损"| SPOOL
    SPOOL -->|"pop()阻塞FIFO"| RING
    RING -->|"submit()背压"| WORKER
    WORKER --> PARSE
    PARSE --> FB
    FB --> SINK
    POOL -.复用帧缓冲.-> FB
    CFG -.bytesPerFrame等参数.-> RECV
    CFG -.解析维度.-> PARSE
    PARSE -.尺寸不匹配→valid=false.-> SINK
```

**关键数据流说明**：
- `FrameSpool` 作为 record-then-process 兜底，RAM 满则溢写磁盘，生产者（socket 排空线程）永不阻塞也永不丢包
- `SpscRing` 在数据路径上满则阻塞，逐级回压，移除一切 drop-oldest 语义
- `Pipeline` 单 worker FIFO 消费，保证输出顺序 == 提交顺序，无需 Resequencer
- `RadarConfig` 作为单一事实源，供帧重组与解析共用一致参数

## 环境要求

- **操作系统**：Windows / Linux / macOS（跨平台支持，已通过跨平台改造）
- **编译环境**：C++17及以上标准的编译器（如MSVC、GCC、Clang）
- **构建系统**：CMake 3.15+
- **依赖库**：无第三方库依赖（仅使用C++标准库 + 系统线程库）
- **平台特定**：
  - Windows：自动链接 ws2_32（Winsock2）
  - Linux/macOS：需要 pthread（通过 CMake `find_package(Threads)` 自动处理）
- **硬件要求**：AWR1843雷达模块、DCA1000数据采集卡、USB转串口适配器（仅实时采集模式需要；离线解析和单元测试无需硬件）

## 安装步骤

1. 克隆仓库到本地
   ```bash
   git clone https://github.com/Lukad77/awr1843-radar-toolkit.git
   cd awr1843-radar-toolkit
   ```

2. 使用 CMake 构建项目（三平台通用）
   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j
   ```

3. 运行单元测试验证新架构组件（无需硬件）
   ```bash
   ctest --test-dir build --output-on-failure
   ```

4. 根据硬件连接修改配置参数（IP地址、串口名称、雷达参数等）

### 构建目标说明

CMake 定义了七个独立的可执行目标：

| 目标 | 用途 | 硬件依赖 |
|------|------|----------|
| `test_udp` | 网络采集链路测试（UDP接收→seqNum重组→DataParser解析） | 无（配合Python回放泵） |
| `radar_full` | 完整程序（含串口链与DCA1000命令通道）；离线分支支持命令行指定 bin 文件逐帧解析 | 实时模式需要；离线解析无 |
| `radar_core_tests` | 新架构基础组件单测（SeqNum/SpscRing/FrameBuffer/BufferPool/RadarConfig） | 无 |
| `radar_pipeline_tests` | 流水线骨架单测（ParseStage单/全Rx + Pipeline保序无损） | 无 |
| `radar_spool_tests` | 两级无损FrameSpool单测（RAM→磁盘溢写FIFO保序） | 无 |
| `radar_dsp_tests` | DSP算子单测（FFT对拍朴素DFT、Range/Doppler峰值定位、CFAR检测/虚警、Angle导向矢量、MTI、相位解缠、全链过Pipeline回压保序） | 无 |
| `radar_dsp_demo` | 真实数据集端到端 DSP 链 demo（逐 stage 耗时/检测摘要/相位 CSV） | 无 |
| `radar_web_demo` | 离线回放 + WebSocket 实时推送（浏览器实时显示，可选 target） | 无 |
| `radar_web_tests` | Web wire 协议单测（编码布局回读对拍，仅 RADAR_BUILD_WEB=ON 时注册） | 无 |

## 使用说明

### 实时数据采集模式

1. 确保AWR1843与DCA1000正确连接并供电
2. 配置网络：将PC与DCA1000连接至同一局域网（默认DCA1000 IP为192.168.33.30）
3. 修改主程序中注释为`#if 0`的`main`函数为启用状态（将`#if 0`改为`#if 1`，同时将原`#if 1`改为`#if 0`）
4. 重新编译：`cmake --build build -j`
5. 根据实际硬件配置修改雷达参数：
   ```cpp
   Radar::RadarParams params;
   params.numADCBits = 16;         // ADC位数
   params.numADCSamples = 256;     // 每chirp的ADC采样数
   params.numChirpsEachFrame = 64; // 每帧的chirp数
   params.numRX = 4;               // 接收天线数量
   params.rxIdx = 0;               // 目标接收天线索引
   ```
6. 运行程序，数据将保存至指定路径（默认：`F:\\RadarData\\adc_<timestamp>.bin`）
7. 按Enter键停止采集

### 离线数据解析模式（默认模式）

`radar_full` 离线分支已改为命令行参数驱动（不再硬编码路径）：

```bash
./build/radar_full <adc_raw.bin> [maxFrames] [out.csv]
#   maxFrames: 最多解析帧数（0 = 全部）
#   out.csv  : 可选，导出第 1 帧单 Rx 数据为 CSV
```

逐帧解析并统计成功/失败帧数与平均样本幅值（数据健全性指标）。已用 500MB 真实数据集（2000 帧 × 262144 B，4Rx×64chirps×256samples）验证：2000/2000 帧全部解析成功。

### 网络链路测试模式（无硬件）

配合仓库根目录的 Python 回放泵 `dca1000_replay_pump.py`（按 DCA1000 包格式：4B seqNum 从 1 起 + 6B byteCnt + 1456B payload），将本地 `.bin` 文件重放至 UDP 端口：

```bash
# 终端 1（注意 bytes_per_frame 必须与数据集匹配，如 262144）
./build/test_udp 0.0.0.0 4098 262144

# 终端 2
 python3 dca1000_replay_pump.py --bin <adc_raw.bin> --host 127.0.0.1 --port 4098 \
         --frame-bytes 262144 --fps 30 --max-frames 300
```

默认监听 `0.0.0.0:4098`，开启 seqNum 排序重组，Ctrl-C 优雅退出并输出统计信息。

> ⚠️ 已知限制：遗留链路 `GetFramesFromQueue(frameNum=1)` 存在**机制性隔帧丢失**（帧尾包含下一帧开头字节被丢弃，重同步只能对齐到再下一帧），实测吞吐恒为发送帧率的 ~50%（与速率无关，0 解析失败/0 接收错误）。新架构接入真实 UDP 源（Dca1000UdpSource）时将修复此问题。

### DSP 端到端 demo（无硬件）

```bash
./build/radar_dsp_demo <adc_raw.bin> [maxFrames] [phase.csv]
```

读取录制的 ADC bin，逐帧跑完整算子链，输出：逐 stage 耗时、检测摘要（距离/速度/角度/SNR）、相位与位移 CSV（含 trackBin/trackAmp 质量列）。真实数据集实测：2000 帧 invalid=0，单帧 DSP 全链 < 2 ms。

配套诊断脚本：`diagnose_phase.py`（直接读 bin 做相位机理诊断；`--plot` 生成相位曲线对比图，需 numpy/matplotlib）。

### Web 实时显示（无硬件）

离线回放 + 浏览器实时图表（原始 ADC 波形 / 距离谱 / 相位位移 / 呼吸波形与呼吸率）：

```bash
# 终端 1：回放并启动 WebSocket 服务（默认端口 8765；--loop 循环回放）
./build/radar_web_demo <adc_raw.bin> [port] [--loop]

# 浏览器直接打开 web/index.html（file:// 即可；端口非默认时加 ?port=<port>）
```

- 数据服务端：`WsFrameSink`（IResultSink 扇出，编码 + try_push 入队，专用发送线程
  广播，客户端过慢时丢帧计数，绝不阻塞 DSP worker）；协议见 `src/web/WireProtocol.h`
  （每帧一条二进制消息 ~2 KB，接入时下发一次 meta JSON）。
- 前端零构建工具链：`web/index.html` + `web/app.js` + vendored uPlot；呼吸带通
  （0.1–0.5 Hz biquad）与呼吸率（30 s 滑窗 Goertzel 谱峰）在前端计算，
  trackAmp 幅度门控的不可靠样本在呼吸波形上以断线呈现。
- 依赖：vendored IXWebSocket（`third_party/ixwebsocket`，USE_TLS/USE_ZLIB 关闭，
  仅标准库 + 系统 socket）。`-DRADAR_BUILD_WEB=OFF` 或删除该目录可完全跳过
  此 target，不影响其余构建与测试。
- 链接形态：默认静态（单文件拷贝即部署，推荐 Jetson 等目标机）；
  `-DRADAR_WEB_SHARED=ON` 改为动态库 `libixwebsocket.so/.dylib`（构建后自动
  复制到可执行文件旁，二进制带 `$ORIGIN`/`@loader_path` rpath，拷目录即部署），
  适合多个工具共享一份库或独立升级库的场景。
- 若浏览器策略禁止 file:// 页面连接 ws://localhost，可用任意静态服务兜底：
  `python3 -m http.server 8000 -d web` 后访问 `http://localhost:8000`。
- `--loop` 回卷处相位有一次 ≤π 的桥接跳变（PhaseUnwrapStage 状态跨回卷），属预期显示行为。

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

新架构中，`RadarConfig` 作为单一事实源管理所有雷达采集和处理参数，可通过 `derive()` 一次计算所有派生值（bytesPerFrame、numRangeBins、分辨率等），通过 `validate()` 校验配置一致性，避免多份配置漂移。

## 代码结构说明

### 传统实时处理层（awr1843_dca1000_read/）

| 文件名称 | 功能描述 |
|----------|----------|
| `awr1843_dca1000_read.cpp` | 主程序入口，包含实时采集（`#if 0`）与离线解析（`#if 1`）两种模式 |
| `UDPController.h/.cpp` | DCA1000 UDP通信控制，实现命令发送与响应处理 |
| `AWR1843Controller.h/.cpp` | AWR1843雷达控制，实现传感器启动与数据处理 |
| `DataParser.h/.cpp` | 雷达数据解析器，支持单天线与全天线数据解析 |
| `RealTimeProcessor.h` | 实时数据处理器，实现数据接收、缓存与存储 |
| `UdpReceiver.h/.cpp` | UDP数据接收与帧重组，支持阻塞与队列两种模式 |
| `Logger.h` | 日志系统，支持多级别日志输出与线程安全 |
| `WzSerialportPlus.h/.cpp` | 跨平台串口通信实现（Win32 DCB/POSIX termios双实现） |
| `net_compat.h` | 网络兼容层，统一Windows Winsock2与POSIX BSD socket差异 |
| `test_udp_main.cpp` | 网络链路测试入口，无硬件依赖 |
| `cf.json` | DCA1000配置文件 |

### 新架构核心组件层（src/）

| 文件路径 | 功能描述 |
|----------|----------|
| `src/core/RadarConfig.h/.cpp` | 单一事实源配置管理，`derive()`计算派生值，`validate()`校验一致性 |
| `src/core/SpscRing.h` | 有界阻塞队列，替代不安全的UnlockQueue，提供无损背压机制 |
| `src/core/BufferPool.h` | 线程安全对象池，消除每帧分配开销，自动归还复用 |
| `src/core/FrameBuffer.h` | 连续内存存储，布局为`[chirp][rx][sample]`，缓存友好 |
| `src/core/FrameContext.h` | 流水线工作单元，携带frameSeq + raw/parsed载荷 + 时间戳 |
| `src/core/Interfaces.h` | 接口定义：IFrameSource/IStage/IResultSink/IInferenceEngine |
| `src/core/SeqNum.h` | uint32序列号回绕安全比较原语 |
| `src/transport/FrameSpool.h/.cpp` | 两级无损FIFO（RAM环+磁盘溢写），确保零丢包 |
| `src/pipeline/Pipeline.h/.cpp` | 保序无损管道执行器，单工作线程FIFO消费 |
| `src/pipeline/ParseStage.h/.cpp` | I/Q解交织管道阶段，替代DataParser，连续内存无mutex |
| `src/dsp/Fft.h/.cpp` | 自研 radix-2 FFT（FftPlan/fftshift/Hann窗），零外部依赖 |
| `src/dsp/RangeFftStage.h/.cpp` | 距离FFT算子（DC去除+加窗+零填充） |
| `src/dsp/DopplerFftStage.h/.cpp` | 多普勒FFT算子 + RD功率图生成 |
| `src/dsp/ClutterRemovalStage.h/.cpp` | MTI静态杂波抑制（跨帧EMA杂波图） |
| `src/dsp/CfarStage.h/.cpp` | 1D CA-CFAR恒虚警检测算子 |
| `src/dsp/AngleFftStage.h/.cpp` | 角度FFT算子（虚拟阵列快拍，1Tx） |
| `src/dsp/PhaseUnwrapStage.h/.cpp` | ⚠️ 实验性：慢时间相位跟踪/解缠算子 |
| `src/dsp/PhaseCsvSink.h` | 相位/位移/质量位 CSV 输出 Sink |
| `src/dsp/Detection.h` | 检测记录与 CFAR 参数定义 |
| `src/tools/radar_dsp_demo.cpp` | 真实数据集端到端 DSP 链 demo |
| `src/tests/test_core.cpp` | 基础组件单测（SeqNum/SpscRing/FrameBuffer/BufferPool/RadarConfig） |
| `src/tests/test_pipeline.cpp` | 流水线单测（ParseStage + Pipeline保序无损） |
| `src/tests/test_spool.cpp` | FrameSpool单测（两级缓冲FIFO保序与溢写验证） |
| `src/tests/test_dsp.cpp` | DSP算子单测（全合成信号对拍，含平台复现/修复对拍、全链集成） |

### 辅助脚本（仓库根目录）

| 文件 | 功能 |
|------|------|
| `dca1000_replay_pump.py` | DCA1000 UDP 回放泵：把离线 bin 按线上包格式重放，配合 test_udp 无硬件验证接收链路 |
| `diagnose_phase.py` | 相位机理诊断（峰值 bin 轨迹/幅度塌陷分析）与修复前后对比绘图（`--plot`） |

## 数据格式说明

- **原始二进制数据**：按帧组织，每帧大小计算方式为`numRxAnt * numChirpsPerFrame * numADCSamples * 4`（4字节/采样点，复数int16 I/Q）
- **解析后数据**：以复数形式（I/Q分量）存储，每个采样点包含实部（I）和虚部（Q）
- **CSV格式**：每行对应一个chirp数据，每个采样点以"I,Q"形式表示，采样点间以逗号分隔
- **FrameBuffer布局**：行优先 `[chirp][rx][sample]`，索引计算 `idx = (chirp * numRx + rx) * numSamples + sample`

## 已知问题与状态

以下为当前已确认、尚未解决的问题（均有实测依据）：

| 问题 | 状态 | 说明 |
|------|------|------|
| **PhaseUnwrapStage 相位提取正确性存疑** | ⚠️ 待解决 | 已实现峰值跟随/相位桥接/Kasa DC 补偿并通过合成信号单测，但**真实数据提取出的相位仍被认为存在问题**，结果仅供研究，不应用于生命体征结论。已知硬约束：跨帧采样间隙大（例 20fps 时 ≈ 40ms）导致容忍径向速度仅 ~24mm/s，快速体动必然欠采样（信息论层面丢失，需提高帧率或改采样策略）；trackAmp 质量位可标记不可靠段但不能修复它们 |
| **遗留 UDP 链路隔帧丢失** | 已定位未修复 | `GetFramesFromQueue(frameNum=1)` 帧尾字节丢弃 + 重同步机制导致吞吐恒为 ~50%；修复路径是新架构 `Dca1000UdpSource`（未实现） |
| **bin 0 近场泄漏残留** | 已定位未修复 | MTI 只能消零多普勒分量，天线耦合泄漏受相噪调制落在 ±1 doppler bin（实测 44.9dB）；需要 CFAR 增加 minRangeBin 距离门控 |
| **TDM-MIMO（多Tx）不支持** | 设计留接缝 | AngleFftStage 仅 1Tx 直通；多 Tx 需多普勒相位补偿后虚拟阵列才相干 |
| **IFrameSource 无实现** | 未开始 | 新架构尚未接入真实 UDP 源/文件回放源，DSP 链目前由 demo 手动驱动 |
| **遗留源码 GBK 编码** | 未处理 | `awr1843_dca1000_read/` 部分文件编译时有编码警告，不影响功能 |

## 常见问题排查

### 传统组件问题

1. **UDP连接失败**：检查网络配置是否正确，确保PC与DCA1000 IP在同一网段
2. **串口无法打开**：确认串口名称正确，检查雷达是否正确供电，关闭占用串口的其他程序
3. **数据解析错误**：检查`BYTES_PER_FRAME`定义是否与实际配置一致，确保输入文件完整
4. **文件无法写入**：检查目标路径是否存在，确保程序有写入权限
5. **数据不完整**：增加`FRAME_BATCH_SIZE`参数可减少写入频率，提高数据完整性

### 新架构组件问题

6. **RadarConfig配置校验失败**：检查 `RadarConfig::validate()` 返回的错误信息，确认 `bytesPerFrame` 计算是否正确，确保 `numAdcSamples` 为偶数且 `rxIdx` 在有效范围内
7. **SpscRing队列阻塞**：监控 `size()` 和 `capacity()`，确认背压机制正常工作；若消费端处理过慢导致持续阻塞，考虑增大队列容量或优化下游处理速度
8. **BufferPool对象泄漏**：监控 `free_count()`，确认对象回收正常；检查 `shared_ptr` 生命周期管理，避免悬垂引用导致对象无法归还
9. **Pipeline处理延迟**：监控 `backlog()` 确认处理速度跟上生产速度；检查各 `IStage::process()` 返回值，识别被丢弃的帧
10. **FrameSpool磁盘溢写**：监控 `diskPeak()` 确认是否发生溢出；若频繁溢写，考虑增大 `ramCapFrames` 或优化消费速度；检查磁盘写入权限和剩余空间
11. **ParseStage尺寸不匹配**：检查 `expectedBytes()` 与实际帧大小是否一致；确认 `RadarConfig` 的 `numRxAnt`、`numChirpsPerFrame`、`numAdcSamples` 配置与实际雷达配置匹配

### 构建问题

12. **CMake找不到线程库**：Linux/macOS 确保安装了 pthread 开发包；Windows 上线程库为空实现，通常不会出现此问题
13. **Windows下链接错误**：确认 CMakeLists.txt 中包含 `ws2_32` 链接（已在 CMake 中自动处理）
14. **测试目标构建失败**：检查 `src/` 目录下相应组件是否正确编译，确认 C++17 标准已启用

## 后续扩展开发指南

新架构通过接口驱动设计（`IFrameSource`/`IStage`/`IResultSink`/`IInferenceEngine`）支持低耦合扩展。新增处理能力只需实现相应接口并注册到 `Pipeline`，无需修改执行器或核心组件。

### 扩展开发架构图

下图展示四个可插拔扩展点：`IFrameSource`（数据源）、`IStage`（处理阶段）、`IInferenceEngine`（推理后端）与 `IResultSink`（结果输出）。标 ✓ 为已实现，其余为后续扩展示例：

```mermaid
graph TB
    subgraph SOURCES["数据源 IFrameSource"]
        S1["Dca1000UdpSource"]
        S2["FileReplaySource"]
        S3["SimulatorSource"]
    end
    PIPE["Pipeline 执行引擎<br/>保序·无损·背压"]
    subgraph STAGES["处理阶段 IStage"]
        ST1["ParseStage ✓"]
        ST2["RangeFftStage ✓"]
        ST2b["DopplerFftStage ✓"]
        ST2c["ClutterRemovalStage ✓"]
        ST3["CfarStage ✓"]
        ST3b["AngleFftStage ✓"]
        ST3c["PhaseUnwrapStage ⚠️实验性"]
        ST4["InferenceStage"]
    end
    subgraph ENGINE["推理后端 IInferenceEngine"]
        E1["OnnxInferenceEngine"]
        E2["TensorRTEngine"]
    end
    subgraph SINKS["结果输出 IResultSink"]
        K1["FileSink"]
        K2["CsvSink"]
        K3["WebSocketSink"]
        K4["MetricsSink"]
    end

    S1 -->|"submit(FrameContext)"| PIPE
    S2 --> PIPE
    S3 --> PIPE
    PIPE --> ST1
    ST1 --> ST2
    ST2 --> ST2b
    ST2b --> ST2c
    ST2c --> ST3
    ST3 --> ST3b
    ST3b --> ST3c
    ST3c --> ST4
    ST4 -.调用.-> E1
    ST4 -.调用.-> E2
    ST4 -->|"扇出"| K1
    ST4 --> K2
    ST4 --> K3
    ST4 --> K4
```

### 新增处理阶段（IStage）

实现 `IStage` 接口，在 `Pipeline` 中按顺序注册即可串联到处理链路：

```cpp
#include "core/Interfaces.h"

class RangeFFTStage : public radar::IStage {
public:
    const char* name() const override { return "RangeFFT"; }

    bool process(radar::FrameContext& ctx) override {
        // ctx.parsed 已由 ParseStage 填充为 FrameBuffer [chirp][rx][sample]
        // 在此执行距离FFT，结果可写回 ctx.parsed 或附加到自定义字段
        // 返回 true 保留帧，返回 false 标记丢弃（仅限 best-effort 分支）
        auto& fb = *ctx.parsed;
        // ... FFT 处理逻辑 ...
        return true;
    }
};

// 注册到 Pipeline
pipeline.addStage(std::make_shared<ParseStage>(cfg));
pipeline.addStage(std::make_shared<RangeFFTStage>());  // 新增阶段
```

**关键约定**：
- `process()` 在 Pipeline 单工作线程中调用，天然保序，无需内部加锁
- `ctx.parsed` 为 `shared_ptr<FrameBuffer>`，可直接读写连续内存
- 返回 `false` 表示显式丢弃（仅限显示旁路等 best-effort 分支，数据路径必须返回 `true`）

### 新增结果输出（IResultSink）

实现 `IResultSink` 接口，注册后 Pipeline 会将处理完的帧扇出到所有 Sink：

```cpp
class CsvSink : public radar::IResultSink {
public:
    void consume(const radar::FrameContext& ctx) override {
        // 将 ctx.parsed 写入 CSV 文件
    }
    void flush() override {
        // 刷新文件缓冲
    }
};

pipeline.addSink(std::make_shared<CsvSink>());
```

**典型 Sink 场景**：文件落盘、CSV 导出、WebSocket 实时推送、指标统计（延迟/丢帧/队列水位）。

### 新增数据源（IFrameSource）

实现 `IFrameSource` 接口，使实时 UDP 接收、文件回放、仿真器等不同来源统一接入 Pipeline：

```cpp
class FileReplaySource : public radar::IFrameSource {
public:
    bool open() override { /* 打开 .bin 文件 */ }
    void close() override { /* 关闭文件 */ }
    bool next(radar::FrameContext& out) override {
        // 读取下一帧原始字节，填充 out.raw 和 out.frameSeq
        // 返回 false 表示数据耗尽
    }
};

// 使用：source.next(ctx) -> pipeline.submit(ctx)
```

### 新增推理后端（IInferenceEngine）

实现 `IInferenceEngine` 接口，抽象 NN 后端（ONNX Runtime / TensorRT 等），供 `InferenceStage` 调用：

```cpp
class OnnxInferenceEngine : public radar::IInferenceEngine {
public:
    bool load(const std::string& modelPath) override { /* 加载模型 */ }
    bool infer(const float* input, std::size_t inN,
               float* output, std::size_t outN) override { /* 执行推理 */ }
};
```

### 扩展开发检查清单

| 检查项 | 说明 |
|--------|------|
| 配置来源 | 所有维度参数从 `RadarConfig` 获取，不硬编码；调用 `derive()` 确保派生值正确 |
| 内存管理 | 使用 `BufferPool` 复用 `FrameBuffer`，避免每帧 malloc；`shared_ptr` 管理生命周期 |
| 线程安全 | Pipeline 单工作线程保证保序，`IStage::process()` 内无需加锁；Sink 的 `consume()` 可能被快速调用，注意写入竞态 |
| 数据完整性 | 帧尺寸不匹配时设置 `ctx.valid=false` 并计数告警，绝不静默转发残帧 |
| 背压传播 | `Pipeline::submit()` 满则阻塞回压；如需非阻塞，检查 `backlog()` 后决策 |
| 日志记录 | 关键节点使用 `Logger` 记录；高频路径避免重格式化 |
| 测试验证 | 新增组件编写单元测试，加入 CMake 测试目标，确保 `ctest` 全通过 |

### 后续阶段路线图

| 阶段 | 目标 | 关键组件 |
|------|------|------|
| Phase 2 后续 | 接入真实 UDP 源与文件回放（修复遗留链路隔帧丢失） | `Dca1000UdpSource`、`FileReplaySource` |
| Phase 0/1 收尾 | 配置外置、遗留源 UTF-8 转换、拆分 `AWR1843Controller` | `AppConfig`(JSON)、`Logger` 启用 |
| Phase 3 后续 | WebSocket 实时显示旁路（RD 图/检测点/波形） | `WebSocketSink` + 前端 |
| Phase 4 收尾 | 相位提取正确性复核、CFAR 距离门控、检测聚类/跟踪 | `PhaseUnwrapStage` 复核、`minRangeBin`、DBSCAN/Kalman |
| Phase 5 | NN 推理 | `InferenceStage`（ONNX Runtime） |
| Phase 6 | 可观测性与优化 | `MetricsSink`（延迟/丢帧/队列水位/Spool 深度）、线程绑核、overload 告警 |

> 更详细的扩展开发指南（含传统链路接入点、数据契约、锁约定）请参阅 [数据处理流程扩展开发指南](.qoder/repowiki/zh/content/数据处理流程扩展开发指南.md) 和 [架构演进文档](docs/ARCHITECTURE_EVOLUTION.md)。

## 许可证

本项目采用MIT许可证，详情参见LICENSE文件。

## 致谢

本项目参考了德州仪器AWR1843与DCA1000的官方技术文档，部分通信协议与数据格式解析基于TI提供的SDK示例。
