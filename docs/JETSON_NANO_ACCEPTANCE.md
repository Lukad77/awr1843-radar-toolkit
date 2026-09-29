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
./build/radar_capture --cfg awr1843.cfg \
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

**注意 `--rcvbuf` 会被内核上限截断**：程序启动时打印的 `SO_RCVBUF` 才是生效值。
若它远小于请求值（例如请求 8 MB 却打印 425984），说明 `net.core.rmem_max`
太小（默认 212992，返回值为其 2 倍）。单帧 262144 字节、按 1472 字节包突发送达，
缓冲区小于一帧就必然在上层落盘阻塞时丢包。先放开上限再长测：

```bash
sysctl net.core.rmem_max net.core.rmem_default      # 查看当前值
sudo sysctl -w net.core.rmem_max=16777216            # 临时放开到 16 MB
# 永久生效：写入 /etc/sysctl.d/99-radar.conf 后 sysctl --system
./build/radar_capture ... --rcvbuf 16777216 ...
```

验证方式：启动行打印的 `SO_RCVBUF` 应接近请求值；`frames.csv` 的线上帧号应连续，
`stats.txt` 中 `missingPackets=0`、`discardedFrames=0`。

## 6. 串口交互排查（`serial timeout:` / `radar rejected:`）

`radar_capture` 的连接顺序是：DCA1000 三条配置命令 → 串口 `sensorStop` → 逐条下发
cfg → DCA 启动记录 → 串口 `sensorStart`。因此若报 `serial timeout: sensorStop`
（而不是 `DCA command timed out`），说明 UDP 控制链已通，问题只在雷达 CLI 串口。

`serialCommand` 按 CLI 提示符 `mmwDemo:/>` 做“一问一答”帧同步：发命令前先排空残留
字节，收到 `Done`/`Ignored` 后等提示符出现、链路静默，并留 20 ms 间隔才发下一条。
错误信息会回显实际收到的内容，据此判断：

- `(reply so far: <no reply>)`：串口完全没回数据，属设备侧问题：
  1. **设备选错**：AWR1843BOOST 的 XDS110 会枚举出两个口，只有
     Application/User UART 是 CLI。用 `ls -l /dev/serial/by-id/` 看名字，
     选带 `Application/User UART`（通常 `-if00`）的那个，不要选
     `Auxiliary Data Port`（`ttyACM0`/`ttyACM1` 的顺序不固定）。
  2. **端口被占用**：mmWave Studio、`screen`、`minicom` 或 Visualizer 若仍持有该口，
     输入会被对方取走。用 `sudo fuser -v /dev/ttyACM0` 确认并关闭占用进程。
  3. **固件/启动模式**：该口只有在雷达运行 mmWave Demo（含 `lvdsStreamCfg` 采集
     固件）时才会应答；未烧写或 SOP 跳线不在功能模式时 CLI 无输出。
  4. 波特率应为 115200（已固定），确认电缆插的是 XDS110 USB 口。
- `(reply so far: Ignored: Sensor is already stopped\n)`：串口正常，命令也被处理了，
  只是 `sensorStop` 打到已停止的传感器时 CLI 不回 `Done`。已把 `Ignored:` 视为成功。
- `radar rejected dfeDataOutputMode 1: ...'eDataOutputMode' is not recognized...`：
  设备收到的命令**丢了开头字节**（`df` 消失），且回复里混入上一条命令的残留提示符
  （`Demo:/>`），说明两条命令发得太近、命令窗口串位。已用上面的帧同步修复。
  若仍复现，按宿主侧丢字节排查：
  1. 确认没有第二个进程在读同一个口：`sudo fuser -v /dev/ttyACM0`；
  2. **ModemManager 会抢读写 ttyACM（已实测命中）**：它把 XDS110 当成调制解调器，
     每次枚举后都会打开 `ttyACM0`/`ttyACM1` 轮流发二进制 AT 探测命令并读取回复，
     表现为雷达 CLI 收到乱码命令（`'<binary>' is not recognized as a CLI command`）
     以及命令回复被读走。用 `journalctl -u ModemManager -n 40 --no-pager` 可看到
     `creating modem with plugin 'generic' and '2' ports ... Failed to find primary AT port`。
     处置（XDS110 为 `0451:bef3`，本项目不需要移动宽带，直接停用最简单）：

     ```bash
     sudo systemctl stop ModemManager                 # 立即生效，先验证
     # 永久屏蔽（推荐，避免每次插拔/复位后重新抢口）
     sudo tee /etc/udev/rules.d/99-ti-xds110-mm-ignore.rules >/dev/null <<'EOF'
     SUBSYSTEM=="usb", ATTRS{idVendor}=="0451", ATTRS{idProduct}=="bef3", ENV{ID_MM_DEVICE_IGNORE}="1"
     SUBSYSTEM=="tty", ATTRS{idVendor}=="0451", ATTRS{idProduct}=="bef3", ENV{ID_MM_DEVICE_IGNORE}="1"
     EOF
     sudo udevadm control --reload && sudo udevadm trigger
     sudo systemctl disable ModemManager              # 或保持 stop 即可
     ```
  3. 换 USB 口/线，避免经 USB Hub。
- **停机命令得不到确认 / 雷达 CLI 无响应**：若 cfg 里 `guiMonitor` 开启了逐帧 UART
  结果上报，会在流式期间把 CLI 处理任务饿死（实测已定位，见 6.1）。用 DCA1000 采
  原始 ADC 时请写 `guiMonitor -1 0 0 0 0 0 0`。收尾顺序为**先停 DCA1000 记录
  （0x06）再 `sensorStop`**，且停机失败只告警、不影响已采集数据与 `stats.txt`。

- **`cannot open serial: ... No such file or directory`**：XDS110 复位/上电后 USB 会重新
  枚举，启动瞬间节点可能还不存在。程序会自动重试约 5 秒，并在报错中带上 `errno`：
  `No such file or directory` = 设备还没出现；`Permission denied` = 当前用户不在
  `dialout` 组；`Device or resource busy` = 被其它进程占用。仍失败时先
  `lsusb | grep 0451`、`ls -l /dev/serial/by-id/` 确认已枚举；若内核反复刷
  `error -110/-71`、`attempt power cycle`，属供电/线材问题，换 USB 口与线并避免经 Hub。

手工确认串口本身是否可用（不依赖本程序，注意用同一个 fd 读写，避免开关端口丢字节）：

```bash
sudo chmod a+rw /dev/ttyACM0            # 或用 usermod -aG dialout $USER 后重新登录
stty -F /dev/ttyACM0 115200 raw -echo
exec 3<>/dev/ttyACM0
printf 'sensorStop\n' >&3
timeout 2 cat <&3
exec 3<&-
```

正常应看到 `Done` 或 `Ignored: Sensor is already stopped`；完全无输出即为上面的设备侧问题。

### 6.1 已知问题（已定位修复）：流式结束后 CLI 卡死

**现象**：在 LVDS 流式输出期间（`sensorStart` 之后）下发停机命令时，mmWave demo 的
CLI 处理任务会整体卡死——命令可能被回显，但 `Done` 永远不来；此后**任何命令都不再
被响应**（含无效命令与 `flushCfg`），只能复位板子恢复。

**根因**：cfg 里 `guiMonitor -1 1 1 1 1 1 1` 会让 demo 在 `sensorStart` 后**每帧把全部
处理结果（检测点、距离谱、噪声底、热图、统计）通过 UART 上报**。本配置帧周期仅
10 ms（`frameCfg ... 10 ...`），115200 波特根本推不完，上报把同一个任务里的 CLI
处理饿死，于是表现为“能回显、不能执行”，`sensorStop` 也拿不到确认。

**修复**：用 DCA1000 采原始 ADC 时这些处理结果本来就不需要（原始数据走 LVDS
的 `lvdsStreamCfg`），把该行改为：

```
guiMonitor -1 0 0 0 0 0 0
```

实测确认：改后收尾 `sensorStop` 正常返回 `Done`，采集结束后 CLI 仍可用
（`'abc' is not recognized as a CLI command` + `mmwDemo:/>` 提示符），**无需复位板子
即可连续采集**。若在别的配置下再次遇到此现象，可按以下方式判定 CLI 是否已卡死：

```bash
exec 3<>/dev/ttyACM0
printf 'abc\n' >&3; timeout 3 cat <&3   # 卡死时：无输出；正常：'abc' is not recognized...
exec 3<&-
```

**兜底行为**：若因其他原因雷达 CLI 无响应，工具开头 `sensorStop` 连续 3 次（15 s）
失败后会直接报 `radar CLI is not responding ... power-cycle or reset the
AWR1843BOARD` 并退出，不再白等整套配置；采集过程中若停机命令未被确认，只打印
warning，完整帧、`stats.txt` 与退出码不受影响。

硬件协议依据：[TI DCA1000EVM User's Guide](https://www.ti.com/lit/ug/spruij4a/spruij4a.pdf)
及 [TI DCA1000 CLI Software Developer Guide](https://e2e.ti.com/cfs-file/__key/communityserver-discussions-components-files/1023/TI_5F00_DCA1000EVM_5F00_CLI_5F00_Software_5F00_DeveloperGuide.pdf)。

## 7. 本次实时采集调试修复清单

上机联调中定位并修复的问题汇总（详细说明见上文对应小节）：

| 现象 | 根因 | 修复 |
|------|------|------|
| 首次 `serial timeout: sensorStop` | `sensorStop` 打到已停止的传感器时 CLI 回 `Ignored: ...`，不含 `Done`，而旧代码只认 `Done` | `serialCommand` 把 `Ignored:` 也视为成功 |
| `'eDataOutputMode' is not recognized`（命令开头丢字节）、回复混入上一条提示符 | 一见 `Done` 就返回，CLI 随后打印的提示符留在缓冲区；两条命令发得过密，雷达 UART 丢首字节 | 发命令前 `drainInput` 排空；以提示符 `:/>` 作为问答结束标志；命令间留 20 ms |
| 命令被拒只表现为 3 s 超时 | 错误判定只匹配 `Error`，漏掉 CLI 的 `not recognized` | 增加 `not recognized`/`Unknown`/`Invalid` 判定，并在报错中回显实际回复 |
| 约 3.5% 丢包（帧号有缺口） | `net.core.rmem_max` 默认 212992，把 8 MB 请求截到 416 KB，小于单帧 262144 字节 | 放开 `rmem_max` 并 `--rcvbuf 16777216`，帧号恢复连续 |
| 复位后 `cannot open serial` | XDS110 重新枚举期间抢跑，节点尚不存在 | 串口 open 重试约 5 s，报错带 `errno` |
| 流式结束后 CLI 整体卡死、`sensorStop` 无响应 | `guiMonitor -1 1 1 1 1 1 1` 每帧把全部处理结果推 UART，10 ms 帧周期下推不完，饿死同一任务里的 CLI 处理 | cfg 改为 `guiMonitor -1 0 0 0 0 0 0`（原始 ADC 走 LVDS，不受影响） |
| 停机失败被报成整体失败、`stats.txt` 缺失 | 收尾停机超时抛异常，跳过统计与退出码 | 收尾先停 DCA1000 记录再 `sensorStop`，重试后仅告警，保证数据与退出码正确 |
| 串口回复时有时无、CLI 收到乱码命令 | ModemManager 把 ttyACM 当调制解调器探测（发送二进制 AT 命令并读取回复） | udev 规则屏蔽该设备（`0451:bef3`） |

验收结论：`savedFrames=1000`、`missingPackets=0`、`latePackets=0`、
`malformedPackets=0`、`discardedFrames=0`，线上帧号 0..999 连续，退出码 0。
