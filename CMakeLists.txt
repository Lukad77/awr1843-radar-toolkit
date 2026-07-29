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

# ==========================================================================
# target 3: radar_core_tests —— 新架构基础组件单元测试（Phase 0-2 地基）
#   纯标准库、无硬件依赖，验证：wrap-safe seq、有界阻塞 SPSC（满则阻塞不丢、
#   跨线程严格 FIFO）、连续内存 FrameBuffer、BufferPool 复用、RadarConfig 派生/校验。
# ==========================================================================
set(NEWSRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_executable(radar_core_tests
    ${NEWSRC_DIR}/tests/test_core.cpp
    ${NEWSRC_DIR}/core/RadarConfig.cpp
)
target_include_directories(radar_core_tests PRIVATE ${NEWSRC_DIR})
target_compile_features(radar_core_tests PRIVATE cxx_std_17)
target_link_libraries(radar_core_tests PRIVATE Threads::Threads)

enable_testing()
add_test(NAME radar_core_tests COMMAND radar_core_tests)

# ==========================================================================
# target 4: radar_pipeline_tests —— Phase 3 流水线骨架单测（无硬件）
#   验证：ParseStage I/Q 反交织（单/全 Rx）、Pipeline 保序与无损（小环制造回压）。
# ==========================================================================
add_executable(radar_pipeline_tests
    ${NEWSRC_DIR}/tests/test_pipeline.cpp
    ${NEWSRC_DIR}/pipeline/ParseStage.cpp
    ${NEWSRC_DIR}/pipeline/Pipeline.cpp
    ${NEWSRC_DIR}/core/RadarConfig.cpp
)
target_include_directories(radar_pipeline_tests PRIVATE ${NEWSRC_DIR})
target_compile_features(radar_pipeline_tests PRIVATE cxx_std_17)
target_link_libraries(radar_pipeline_tests PRIVATE Threads::Threads)
add_test(NAME radar_pipeline_tests COMMAND radar_pipeline_tests)

# ==========================================================================
# target 5: radar_spool_tests —— Phase 2 两级无损 FrameSpool 单测（无硬件）
#   验证：RAM→磁盘溢写的 FIFO 保序与逐字节一致、确实发生溢写、排空后回用 RAM、close 唯醒。
# ==========================================================================
add_executable(radar_spool_tests
    ${NEWSRC_DIR}/tests/test_spool.cpp
    ${NEWSRC_DIR}/transport/FrameSpool.cpp
)
target_include_directories(radar_spool_tests PRIVATE ${NEWSRC_DIR})
target_compile_features(radar_spool_tests PRIVATE cxx_std_17)
target_link_libraries(radar_spool_tests PRIVATE Threads::Threads)
add_test(NAME radar_spool_tests COMMAND radar_spool_tests)

# ==========================================================================
# Phase 4 DSP 算子源文件（自研 radix-2 FFT，零外部依赖，三平台）
# ==========================================================================
set(DSP_SRCS
    ${NEWSRC_DIR}/dsp/Fft.cpp
    ${NEWSRC_DIR}/dsp/RangeFftStage.cpp
    ${NEWSRC_DIR}/dsp/DopplerFftStage.cpp
    ${NEWSRC_DIR}/dsp/CfarStage.cpp
    ${NEWSRC_DIR}/dsp/AngleFftStage.cpp
    ${NEWSRC_DIR}/dsp/PhaseUnwrapStage.cpp
    ${NEWSRC_DIR}/dsp/ClutterRemovalStage.cpp
)

# ==========================================================================
# target 6: radar_dsp_tests —— Phase 4 DSP 算子单测（无硬件，全合成信号）
#   验证：FFT 对拍朴素 DFT、Range/Doppler 峰值定位、CA-CFAR 检测/虚警、
#   Angle FFT 导向矢量恢复、慢时间相位解缠绕、全链路过 Pipeline 回压保序。
# ==========================================================================
add_executable(radar_dsp_tests
    ${NEWSRC_DIR}/tests/test_dsp.cpp
    ${NEWSRC_DIR}/core/RadarConfig.cpp
    ${NEWSRC_DIR}/pipeline/ParseStage.cpp
    ${NEWSRC_DIR}/pipeline/Pipeline.cpp
    ${DSP_SRCS}
)
target_include_directories(radar_dsp_tests PRIVATE ${NEWSRC_DIR})
target_compile_features(radar_dsp_tests PRIVATE cxx_std_17)
target_link_libraries(radar_dsp_tests PRIVATE Threads::Threads)
add_test(NAME radar_dsp_tests COMMAND radar_dsp_tests)

# ==========================================================================
# target 7: radar_dsp_demo —— 真实数据集端到端 DSP 链 demo（无硬件回归入口）
#   Parse → RangeFFT → DopplerFFT → CA-CFAR → AngleFFT → PhaseUnwrap，
#   输出逐 stage 耗时、检测摘要与呼吸相位 CSV。
# ==========================================================================
add_executable(radar_dsp_demo
    ${NEWSRC_DIR}/tools/radar_dsp_demo.cpp
    ${NEWSRC_DIR}/core/RadarConfig.cpp
    ${NEWSRC_DIR}/pipeline/ParseStage.cpp
    ${DSP_SRCS}
)
target_include_directories(radar_dsp_demo PRIVATE ${NEWSRC_DIR})
target_compile_features(radar_dsp_demo PRIVATE cxx_std_17)
target_link_libraries(radar_dsp_demo PRIVATE Threads::Threads)

# ---- 构建提示 ----
message(STATUS "==============================================================")
message(STATUS " ${PROJECT_NAME} configured for ${CMAKE_SYSTEM_NAME}")
message(STATUS "   test_udp        : 网络采集链路测试（三平台）")
message(STATUS "   radar_full      : 完整程序（含串口链，三平台）")
message(STATUS "   radar_core_tests: 新架构基础组件单测（三平台）")
message(STATUS "   radar_pipeline_tests: 流水线骨架单测（三平台）")
message(STATUS "   radar_spool_tests   : 两级无损 FrameSpool 单测（三平台）")
message(STATUS "   radar_dsp_tests     : Phase4 DSP 算子单测（三平台）")
message(STATUS "   radar_dsp_demo      : 数据集端到端 DSP 链 demo（三平台）")
message(STATUS "==============================================================")
