---
kind: configuration_system
name: 雷达配置系统（RadarConfig）
category: configuration_system
scope:
    - '**'
source_files:
    - src/core/RadarConfig.h
    - src/core/RadarConfig.cpp
    - src/tools/radar_dsp_demo.cpp
    - src/tests/test_core.cpp
---

本仓库采用单一的 `RadarConfig` 结构体作为采集与 DSP 处理的唯一配置源，集中管理 AWR1843 毫米波雷达的硬件参数、帧/剖面配置以及由此推导出的所有计算中间值。该设计消除了过去分散在 `RadarParams`、`AWR1843Controller::ConfigParams` 和松散 `frameCfg` 字段中的多份漂移表示，确保全链路一致性。

**核心机制**
- `derive()`：幂等方法，根据主字段一次性计算所有派生值（`bytesPerFrame`、`numRangeBins`、分辨率、最大范围/速度、波长、虚拟天线数等），调用方只需设置原始参数后调用一次即可。
- `validate()`：集中校验约束（如 `numAdcSamples` 必须为正偶数、`numAngleBins` 必须为 2 的幂、`rxIdx` 越界等），失败时返回错误字符串，避免非法配置污染帧解析与重装配。

**配置字段分类**
- 数据格式：`numAdcBits`、`isReal`（实/复采样）、`numRxAnt`、`numTxAnt`、`numAdcSamples`、`rxIdx`、`numAngleBins`
- 帧配置（frameCfg）：`chirpStartIdx`、`chirpEndIdx`、`numLoops`、`numFrames`、`framePeriodicityMs`、触发相关字段
- 剖面配置（profileCfg）：`startFreqGHz`、`idleTimeUs`、`rampEndTimeUs`、`freqSlopeMHzPerUs`、`digOutSampleRateKsps`
- 派生字段：`numChirpsPerFrame`、`numDopplerBins`、`numRangeBins`、`bytesPerSample`、`bytesPerFrame`、各类分辨率与物理量

**使用模式**
入口程序 `src/tools/radar_dsp_demo.cpp` 展示了标准用法：构造 `RadarConfig` → 设置主字段 → 调用 `derive()` → 调用 `validate()` → 将只读引用传递给各 Stage（ParseStage、RangeFftStage、PhaseUnwrapStage、ClutterRemovalStage、DopplerFftStage、CfarStage、AngleFftStage）。各 Stage 持有 `cfg_` 成员并在内部检测“配置漂移”（如维度不匹配）以标记无效帧。

**设计约定**
- 配置不可变：一旦 derive+validate 通过，下游 Stage 仅读取，不修改。
- 派生值零拷贝共享：通过 const 引用传递，避免复制开销。
- 单元测试覆盖 derive + validate 路径（`test_core.cpp`），保证配置一致性。
- 无外部配置文件加载器：当前 demo 直接硬编码参数；如需扩展 YAML/TOML/env 加载，应在 RadarConfig 之上封装一个解析层，保持 derive/validate 不变。