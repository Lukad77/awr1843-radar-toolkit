---
kind: dependency_management
name: CMake 构建与系统库依赖管理
category: dependency_management
scope:
    - '**'
source_files:
    - CMakeLists.txt
    - awr1843_dca1000_read/net_compat.h
---

本仓库采用 CMake 作为唯一构建与依赖声明入口，未使用 vcpkg、Conan、pkg-config 或任何第三方包管理器。所有外部依赖均为操作系统提供的标准库，通过 `find_package` 和条件链接方式引入：

- **线程库**：通过 `find_package(Threads REQUIRED)` + `Threads::Threads` 目标获取，POSIX 下对应 pthread，Windows 下为空实现。
- **Winsock2（仅 Windows）**：通过 `target_link_libraries(... PRIVATE ws2_32)` 显式链接；同时在 `net_compat.h` 中以 `#pragma comment(lib, "ws2_32.lib")` 兜底，确保非 MSVC 工具链也能编译。
- **串口 I/O（仅 POSIX）**：`WzSerialportPlus.cpp` 直接 `#include <termios.h>` / `<fcntl.h>` / `<unistd.h>` 调用原生 API，无额外依赖。
- **网络 I/O（跨平台）**：`UDPController` / `UdpReceiver` 在 `net_compat.h` 中用 `#ifdef _WIN32` 分支选择 Winsock2 或 POSIX socket API，头文件层面完成兼容。

项目定义了两个可执行 target：
- `test_udp`：仅含 UDP 接收+解析链路，用于配合 Python 回放泵测试。
- `radar_full`：完整程序，包含串口控制链路与 UDP 数据链。

两个 target 均只依赖 `Threads::Threads`，并在 Windows 上额外链接 `ws2_32`。源码全部位于 `awr1843_dca1000_read/` 单目录下，以相对路径 `#include "*.h"` 引用，不存在子模块或 `add_subdirectory` 形式的内部库复用。

仓库内未发现以下依赖管理机制：lockfile（如 `vcpkg.json.lock`）、vendor 目录、私有源配置、版本锁定策略或自动化升级脚本。第三方依赖为零——所有功能均由 C++17 标准库与 OS 原生 API 实现。