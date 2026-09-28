# Jetson Nano 采集验收（`jeston_dev`）

此分支新增 Linux 专用 `radar_capture`。它接收 DCA1000 原始 UDP 包，按 48 位
byte count 重组，只把完整 ADC 帧写入 `adc.bin`。同时生成
`adc.bin.frames.csv`（文件帧号与线上帧号对应）及 `adc.bin.stats.txt`。
发生包缺失、帧缺损、文件写入失败或没有完整帧时，程序返回非零状态。

## 1. Nano 本机编译

```bash
git fetch origin
git switch jeston_dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRADAR_BUILD_WEB=OFF
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

验收：能生成 `build/radar_capture`，所有 CTest 项通过。若 Nano 的 CMake
低于 3.10，先安装较新版本。建议先关闭 Web target，以便只检查采集链。

## 2. 无硬件 UDP 回放验收

在第一个终端运行：

```bash
./build/radar_capture --no-control --frame-bytes 262144 \
  --output replay_capture.bin --max-frames 2
```

第二个终端生成两帧，发向本机 UDP 4098：

```bash
python3 -c "open('replay_input.bin','wb').write(bytes(range(256))*2048)"
python3 dca1000_replay_pump.py --bin replay_input.bin \
  --host 127.0.0.1 --port 4098 --frame-bytes 262144 --fps 10 --max-frames 2
cmp replay_input.bin replay_capture.bin
cat replay_capture.bin.stats.txt
```

验收：`cmp` 无输出且退出码为 0；`savedFrames=2`、`missingPackets=0`、
`discardedFrames=0`。这是“UDP 收包→重组→落盘”验收，不涉及真实硬件。

## 3. DCA1000 和 AWR1843 控制验收

准备一份**已经在该雷达上验证可运行**的 mmWave CLI `.cfg`。当前采集入口
仅支持单 TX、单 chirp、16 位复数 ADC、`lvdsStreamCfg` 不带头的原始 ADC
模式。程序根据 `.cfg` 的 `channelCfg`、`profileCfg`、`frameCfg` 自动计算
`frameBytes`，不依赖 demo 中的硬编码数据集参数。配置中的 `sensorStart`
会延后到 DCA1000 进入记录状态后发送。

确认 Nano 网口已配置成与采集卡同网段，并查明 CLI 串口设备：

```bash
ip -br addr
ls -l /dev/serial/by-id/
```

下面的地址与端口是 DCA1000 常见默认值；如果采集卡 EEPROM 曾被修改，
应填写实际值。请把接口、串口和 cfg 路径替换成 Nano 的实际值：

```bash
mkdir -p captures
./build/radar_capture --cfg /path/to/working.cfg \
  --serial /dev/ttyACM0 --bind-ip 192.168.33.30 \
  --dca-ip 192.168.33.180 --output captures/adc.bin --max-frames 1000
```

验收：启动过程不出现 DCA 命令超时/拒绝或雷达 CLI `Error`；DCA 数据灯
有活动；程序接收足够数据后自行停止雷达与记录。串口若无权限，请检查
设备所属用户组后赋予当前用户访问权。

## 4. 原始数据与完整性验收

```bash
cat captures/adc.bin.stats.txt
head captures/adc.bin.frames.csv
wc -c captures/adc.bin
```

验收：`savedFrames=1000`，`missingPackets=0`、`discardedFrames=0`、
`malformedPackets=0`；输出字节数等于 `savedFrames × frameBytes`；
`frames.csv` 的线上帧号连续。程序退出码为 0。若有丢包，它仍保留
已完整采集的帧，`frames.csv` 标明对应的线上帧号，退出码为 2；不要把
这种文件当作连续无缺口录制数据使用。

可用已有离线处理入口读取录制文件，但需注意其参数仍针对仓库演示数据集
硬编码；仅在真实 cfg 与其参数一致时运行：

```bash
./build/radar_dsp_demo captures/adc.bin 100
```

## 5. 持续采集验收

去掉 `--max-frames`，至少连续运行目标使用时长，以 Ctrl-C 停止。每次检查
`stats.txt`、文件字节数、磁盘剩余空间及帧号连续性。当前接收和写盘在
同一线程，受磁盘延迟及 Nano 系统负载影响，无法仅凭单元测试保证零丢包；
以实机长测结果作为性能验收依据。必要时增大 `--rcvbuf`，并调整
`--packet-delay-us`（5–500，默认 25）降低发包速率。

硬件协议依据：[TI DCA1000EVM User's Guide](https://www.ti.com/lit/ug/spruij4a/spruij4a.pdf)
及 [TI DCA1000 CLI Software Developer Guide](https://e2e.ti.com/cfs-file/__key/communityserver-discussions-components-files/1023/TI_5F00_DCA1000EVM_5F00_CLI_5F00_Software_5F00_DeveloperGuide.pdf)。
