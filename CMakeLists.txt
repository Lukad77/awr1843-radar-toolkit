cmake_minimum_required(VERSION 3.15)
project(awr1843_radar_toolkit LANGUAGES CXX)

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

# ==========================================================================
# target 8: radar_web_demo —— 离线回放 + WebSocket 实时推送（可选）
#   依赖 vendored IXWebSocket（third_party/ixwebsocket，USE_TLS/USE_ZLIB
#   均关闭 => 仅标准库 + 系统 socket）。目录缺失或 RADAR_BUILD_WEB=OFF
#   时静默跳过，不影响既有 target 与 ctest。
# ==========================================================================
option(RADAR_BUILD_WEB "Build radar_web_demo (requires third_party/ixwebsocket)" ON)
# 部署形态开关：OFF（默认）= 静态链接，单文件拷贝即部署（推荐 Jetson 等
# 目标机）；ON = 编译 libixwebsocket 动态库，多工具共享一份 .so / 独立
# 升级库时使用。动态模式下 .so 会被复制到可执行文件旁，且二进制带
# $ORIGIN/@loader_path rpath —— 整个目录拷走即可运行。
option(RADAR_WEB_SHARED "Link IXWebSocket as a shared library" OFF)
if(RADAR_BUILD_WEB AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ixwebsocket/CMakeLists.txt)
    set(USE_TLS OFF CACHE BOOL "" FORCE)
    set(USE_ZLIB OFF CACHE BOOL "" FORCE)
    set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS ${RADAR_WEB_SHARED} CACHE BOOL "" FORCE)
    add_subdirectory(third_party/ixwebsocket)

    add_executable(radar_web_demo
        ${NEWSRC_DIR}/tools/radar_web_demo.cpp
        ${NEWSRC_DIR}/web/WsFrameSink.cpp
        ${NEWSRC_DIR}/core/RadarConfig.cpp
        ${NEWSRC_DIR}/pipeline/ParseStage.cpp
        ${NEWSRC_DIR}/pipeline/Pipeline.cpp
        ${DSP_SRCS}
    )
    target_include_directories(radar_web_demo PRIVATE ${NEWSRC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ixwebsocket)
    target_compile_features(radar_web_demo PRIVATE cxx_std_17)
    target_link_libraries(radar_web_demo PRIVATE ixwebsocket Threads::Threads)

    if(RADAR_WEB_SHARED)
        # 相对 rpath：可执行文件优先在自身目录找 .so/.dylib，
        # 部署 = 拷贝 radar_web_demo + libixwebsocket.* 到同一目录。
        if(APPLE)
            set(RADAR_WEB_RPATH "@loader_path")
        else()
            set(RADAR_WEB_RPATH "$ORIGIN")
        endif()
        set_target_properties(radar_web_demo PROPERTIES
            BUILD_RPATH "${RADAR_WEB_RPATH}"
            INSTALL_RPATH "${RADAR_WEB_RPATH}")
        add_custom_command(TARGET radar_web_demo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:ixwebsocket>
                    $<TARGET_FILE_DIR:radar_web_demo>
            COMMENT "复制 libixwebsocket 动态库到可执行文件目录（拷目录即部署）")
    endif()

    # wire 协议单测（纯 CPU，无网络），仅在 web 功能开启时注册。
    add_executable(radar_web_tests
        ${NEWSRC_DIR}/tests/test_web.cpp
        ${NEWSRC_DIR}/core/RadarConfig.cpp
    )
    target_include_directories(radar_web_tests PRIVATE ${NEWSRC_DIR})
    target_compile_features(radar_web_tests PRIVATE cxx_std_17)
    target_link_libraries(radar_web_tests PRIVATE Threads::Threads)
    add_test(NAME radar_web_tests COMMAND radar_web_tests)

    set(RADAR_WEB_STATUS "radar_web_demo      : 回放 + WebSocket 实时显示（web/index.html）")
else()
    set(RADAR_WEB_STATUS "radar_web_demo      : 跳过（RADAR_BUILD_WEB=OFF 或缺 third_party/ixwebsocket）")
endif()

# ---- 构建提示 ----
message(STATUS "==============================================================")
message(STATUS " ${PROJECT_NAME} configured for ${CMAKE_SYSTEM_NAME}")
message(STATUS "   radar_core_tests: 新架构基础组件单测（三平台）")
message(STATUS "   radar_pipeline_tests: 流水线骨架单测（三平台）")
message(STATUS "   radar_spool_tests   : 两级无损 FrameSpool 单测（三平台）")
message(STATUS "   radar_dsp_tests     : Phase4 DSP 算子单测（三平台）")
message(STATUS "   radar_dsp_demo      : 数据集端到端 DSP 链 demo（三平台）")
message(STATUS "   ${RADAR_WEB_STATUS}")
message(STATUS "==============================================================")
