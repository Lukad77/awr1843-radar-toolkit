---
kind: error_handling
name: C++ 错误处理策略：返回值 + 异常 + 无阻塞设计
category: error_handling
scope:
    - '**'
source_files:
    - src/core/SpscRing.h
    - src/transport/FrameSpool.h
    - src/core/Interfaces.h
    - src/dsp/Fft.h
    - src/dsp/Fft.cpp
---

该 AWR1843 毫米波雷达工具库采用混合错误处理策略，核心原则是**高性能路径使用布尔返回值，编程错误使用 C++ 异常，资源/IO 错误通过状态码传播**。具体体现在以下层面：

**1. 数据路径（Data Path）——布尔返回值模式**
- `SpscRing` 的 `push()`/`try_push()`/`pop()`/`try_pop()` 全部返回 `bool`，满/空/关闭时返回 `false`，不抛异常、不丢帧
- `FrameSpool` 的 `push()` 在磁盘写入失败或已关闭时返回 `false`，调用方需显式告警
- `IStage::process()` 返回 `false` 表示丢弃帧，仅允许在 display-only 分支使用
- `IFrameSource::next()` 停止时返回 `false`，由调用循环判断

**2. 编程错误 —— 标准异常**
- `FftPlan` 构造函数对非 2 的幂尺寸抛出 `std::invalid_argument`
- 注释明确约定：DSP 阶段参数校验失败走异常路径，因为这是不可恢复的编程错误

**3. 背压与无损设计**
- `SpscRing` 的阻塞 `push()` 向上游传播背压，`try_push()` 用于 socket 接收线程（绝不能阻塞，否则内核丢包）
- 当 RAM ring 满时，数据自动 spill 到 `FrameSpool` 磁盘文件，保证 producer 永不阻塞、永不丢帧
- `close()` 后所有等待者被唤醒并优雅退出

**4. 接口契约**
- `Interfaces.h` 定义统一抽象：`IFrameSource`、`IStage`、`IResultSink`、`IInferenceEngine` 均通过 `bool` 返回值表达成功/失败
- 所有虚析构函数正确声明为 `virtual ~...() = default`，确保多态销毁安全

**5. 缺失部分**
- 未发现统一的错误类型体系（如自定义 error code enum 或 Result<T,E> 模板）
- 未见结构化日志框架集成，错误信息主要通过返回值和注释传达
- 未使用 `std::optional` 或 `std::expected` 等现代 C++ 错误传播机制