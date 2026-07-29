---
kind: external_dependency
name: 德州仪器 DCA1000 LVDS 数据采集卡
slug: ti-dca1000-capture-card
category: external_dependency
category_hints:
    - vendor_identity
scope:
    - '**'
---

### 德州仪器 DCA1000 数据采集卡

**身份与角色**：连接 AWR1843 雷达并通过以太网传输原始 ADC 数据的采集卡，通过 UDP 协议接收其 LVDS 捕获的数据流。

**集成方式**：
- **数据通道**：`UDPReceiver` 从端口 4098 接收连续字节流，按 seqNum 重组为完整帧

**关键约束**：
- 必须使用 TI 提供的 `cf.json` 配置数据格式模式（LVDS 捕获、以太网流传输）
- 数据为复数 I/Q 交织格式，每采样点 4 字节（2×int16）
- 需要 Python 回放泵（`dca1000_replay_pump.py`）配合测试