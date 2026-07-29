---
kind: external_dependency
name: CMake 跨平台构建系统
slug: cmake-build-system
category: external_dependency
category_hints:
    - framework_behavior
scope:
    - '**'
---

### CMake 构建系统


**构建目标**：
- `test_udp`：最小化依赖的测试目标，仅包含 UDP 接收+解析链路，不含串口链，用于无硬件环境验证
- `radar_full`：完整程序目标，包含串口通信、雷达控制、网络控制等全部功能

**跨平台特性**：
- 强制 C++17 标准，默认 Release 构建类型
- Windows 下额外链接 `ws2_32` 库（winsock）
- 源码目录集中在 `awr1843_dca1000_read/` 子目录

**设计意图**：`test_udp` 目标使开发者在无实体硬件（如 Mac 开发机）时也能测试核心数据链路，配合 Python 回放泵完成端到端验证。