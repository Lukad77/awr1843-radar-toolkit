cmake_minimum_required(VERSION 3.15)
project(awr1843_dca1000_read LANGUAGES CXX)

# ---- 全局设置 ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 线程库（POSIX 需要 pthread；Windows 上为空实现）
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# 源码目录（.h/.cpp 都在这一层）
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/awr1843_dca1000_read)

# ==========================================================================
# target 1: test_udp —— 网络采集链路测试入口（三平台均可构建）
#   只依赖已跨平台的网络+解析组件，不含串口链，用于配合 Python 回放泵测试
#   “UDP 接收 → seqNum 重组 → DataParser 解析”。
# ==========================================================================
add_executable(test_udp
    ${SRC_DIR}/test_udp_main.cpp
    ${SRC_DIR}/UdpReceiver.cpp
    ${SRC_DIR}/DataParser.cpp
)
target_include_directories(test_udp PRIVATE ${SRC_DIR})
target_link_libraries(test_udp PRIVATE Threads::Threads)

# ==========================================================================
# target 2: radar_full —— 完整程序（含串口链，三平台均可构建）
#   串口链(WzSerialportPlus)已用 termios 跨平台化；网络控制通道(UDPController)
#   亦已跨平台。原 main 的 #if 0/#if 1 结构保留（默认走离线回放分支）。
# ==========================================================================
add_executable(radar_full
    ${SRC_DIR}/awr1843_dca1000_read.cpp
    ${SRC_DIR}/AWR1843Controller.cpp
    ${SRC_DIR}/DataParser.cpp
    ${SRC_DIR}/UDPController.cpp
    ${SRC_DIR}/UdpReceiver.cpp
    ${SRC_DIR}/WzSerialportPlus.cpp
)
target_include_directories(radar_full PRIVATE ${SRC_DIR})
target_link_libraries(radar_full PRIVATE Threads::Threads)

# ---- 平台相关链接 ----
if(WIN32)
    # winsock（net_compat.h 里已有 #pragma comment(lib,"ws2_32.lib")，
    # 这里再显式链一次，确保非 MSVC 工具链/纯 CMake 场景也能过）
    target_link_libraries(test_udp   PRIVATE ws2_32)
    target_link_libraries(radar_full PRIVATE ws2_32)
endif()

# ---- 构建提示 ----
message(STATUS "==============================================================")
message(STATUS " ${PROJECT_NAME} configured for ${CMAKE_SYSTEM_NAME}")
message(STATUS "   test_udp   : 网络采集链路测试（三平台）")
message(STATUS "   radar_full : 完整程序（含串口链，三平台）")
message(STATUS "==============================================================")
