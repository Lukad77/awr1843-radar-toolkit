---
kind: error_handling
name: C++ 雷达工具的错误处理策略：异常与布尔返回值混合模式
category: error_handling
scope:
    - '**'
source_files:
    - awr1843_dca1000_read/UDPController.cpp
    - awr1843_dca1000_read/UdpReceiver.cpp
    - awr1843_dca1000_read/DataParser.cpp
    - awr1843_dca1000_read/Logger.h
    - awr1843_dca1000_read/RealTimeProcessor.h
---

本仓库在 C++ 实现的 AWR1843+DCA1000 毫米波雷达数据采集与处理工具中，采用了**异常与布尔返回值混合**的错误处理策略，未定义统一的错误码枚举或自定义异常类型。具体表现为：

## 1. 网络层（UDPController）——抛出 std::exception
- `UDPController` 构造函数及 `_sendCMD` 在 WSAStartup、socket/bind/send/recv 失败时抛出 `std::system_error` 和 `std::runtime_error`。
- 这些异常由调用方（如 `RealTimeProcessor::Start`）通过检查初始化返回值间接感知，但上层并未显式 try/catch，依赖进程崩溃或日志输出暴露问题。

## 2. UDP 接收层（UdpReceiver）——返回 bool + 统计计数
- `Initialize`、`StartReceiving`、`GetFramesFromQueue`、`ReadFrames` 等 API 全部以 `bool` 返回值表示成功/失败，失败时通过 `std::cerr` 打印带前缀 `[UDPReceiver]` 的错误信息。
- 非阻塞接收线程内部对 `recvfrom` 的 EAGAIN/EWOULDBLOCK 使用 `netcompat::net_would_block()` 判断并忽略，其他错误则累加 `stats_.errorCount` 并通过 `GetStatistics()` 暴露给上层。
- 无异常抛出，无中断机制，超时通过 `data_ready_cv_.wait_for` 配合 timeout 参数实现。

## 3. 数据解析层（DataParser）——返回 bool + 局部 try/catch
- `parse_FrameData` / `parse_FrameData_AllRX` 以 `bool` 返回值报告帧大小校验失败等情况。
- 核心循环内用 `try { ... } catch (const std::exception& e)` 捕获越界等运行时异常，记录到 `std::cerr` 后返回 `false`；另一处 `catch (...)` 作为兜底。
- 解析函数本身不向上抛出异常，将错误转化为布尔状态供调用方决策。

## 4. 串口控制层（AWR1843Controller）——返回 bool
- `openSerialPorts`、`parseData18xx` 等方法均以 `bool` 返回值表达打开串口、解析 TLV 帧是否成功，失败路径直接 `return false`，无额外错误上下文传递。

## 5. 日志系统（Logger）——独立子系统
- `Logger.h` 定义了 `LogLevel` 枚举（Debug/Info/Warn/Error），采用单例 + 观察者模式 + 异步队列实现多线程安全日志。
- `FileSink` 构造失败时抛出 `std::runtime_error`，其余错误通过 `LOG_ERROR` 级别记录而非抛出。
- 日志格式支持 `{}` 占位符替换，输出包含时间戳、线程 ID、级别与消息。

## 6. 实时处理管线（RealTimeProcessor）——组合上述策略
- `Start()` 依次调用 `receiver_.Initialize` 与 `receiver_.StartReceiving`，任一失败即 `return false` 并由调用者打印错误。
- 数据处理循环中对 `parser_.parse_FrameData` 的返回值做分支处理，失败时仅跳过当前帧继续运行。

## 设计特点与风险
- **优点**：简单直观，无需引入第三方错误库；关键路径（UDP 接收）避免异常开销，适合高频数据流场景。
- **缺点**：错误语义分散于 `bool`、`std::cerr`、`stats.errorCount` 三处，缺乏统一错误上下文；`UDPController` 抛出的异常未被任何 `try/catch` 包裹，可能导致程序直接终止；日志与错误输出混用 `std::cout`/`std::cerr`，难以区分正常信息与故障信息。
- **缺失**：无统一错误码枚举、无自定义异常基类、无 panic/recover 机制、无中间件式错误传播（如 `std::expected` 风格）。