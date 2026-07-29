---
kind: build_system
name: CMake 跨平台构建系统
category: build_system
scope:
    - '**'
source_files:
    - CMakeLists.txt
    - awr1843_dca1000_read/cf.json
    - awr1843_dca1000_read/net_compat.h
---

本项目采用 CMake 作为唯一官方构建系统，目标为在 Linux、macOS、Windows 三平台上编译同一套 C++17 源码。根目录仅有一个 CMakeLists.txt，所有业务源码集中在 awr1843_dca1000_read/ 子目录，无 Makefile、Shell 脚本或 Dockerfile。

构建目标与产物：
- test_udp：轻量测试入口，仅依赖 UDP 接收与数据解析组件（UdpReceiver.cpp + DataParser.cpp），不含串口链，用于配合 Python 回放泵验证网络链路，三平台均可构建。
- radar_full：完整程序，包含 AWR1843 串口控制（WzSerialportPlus.cpp）、UDP 控制通道（UDPController.cpp）以及主流程（awr1843_dca1000_read.cpp）。

跨平台策略：
- 线程库通过 find_package(Threads) 获取，POSIX 下链接 pthread，Windows 上为空实现；Windows 额外显式链接 ws2_32 以支持 Winsock。
- 源码中通过 net_compat.h 的 #pragma comment(lib,"ws2_32.lib") 与条件编译屏蔽 Windows-only 头文件，使 test_udp 可在 macOS(clang) 直接编译。
- 串口层 WzSerialportPlus 使用 POSIX termios API，Windows 侧未提供等价实现，因此该 target 在 Windows 上需跳过串口相关源文件。

配置与资源：
- 运行时参数通过 JSON 配置文件 cf.json 注入，定义 DCA1000 采集模式、LVDS 模式、数据格式等字段。
- 雷达 chirp 序列配置以 .cfg 文本文件（1T1R.cfg、awr1843.cfg）形式随源码分发，由上层读取后下发至设备 CLI。

开发者约定：
- 新增可执行目标应在根 CMakeLists.txt 中以 add_executable 声明，并将新源文件加入对应 target 的源列表。
- 平台差异优先通过 #ifdef WIN32 / net_compat.h 解决，避免在业务逻辑中散落平台分支。
- 第三方库应通过 find_package 引入并链接到具体 target，而非全局设置。