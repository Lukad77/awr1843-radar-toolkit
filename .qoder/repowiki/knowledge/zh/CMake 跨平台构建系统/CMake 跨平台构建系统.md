---
kind: build_system
name: CMake 跨平台构建系统
category: build_system
scope:
    - '**'
source_files:
    - CMakeLists.txt
---

本项目采用 CMake 作为唯一构建系统，目标为跨平台（Linux/Windows/macOS）的 C++17 工具链。根目录仅包含一个 `CMakeLists.txt`，未使用 Makefile、Dockerfile、CI 脚本或版本发布脚本。

**构建配置与标准**
- 最低 CMake 版本 3.15，强制启用 C++17（`CMAKE_CXX_STANDARD 17`），关闭编译器扩展以保证可移植性。
- 默认构建类型为 Release；线程库通过 `find_package(Threads)` 发现，POSIX 下链接 pthread，Windows 为空实现。

**目标产物组织**
CMakeLists.txt 按功能阶段定义多个独立可执行目标，每个目标对应一组源文件并注册为 CTest 测试：
- `radar_core_tests`：基础组件单测（RadarConfig、SPSC 队列、FrameBuffer、BufferPool）
- `radar_pipeline_tests`：流水线骨架单测（ParseStage、Pipeline 保序与无损）
- `radar_spool_tests`：两级无损 FrameSpool 单测（RAM→磁盘溢写 FIFO 一致性）
- `radar_dsp_tests`：Phase4 DSP 算子单测（FFT、Range/Doppler/CFAR/Angle FFT、相位解缠绕）
- `radar_dsp_demo`：真实数据集端到端 DSP 链 demo（无硬件回归入口）

所有目标均通过 `target_include_directories` 指向 `src/`，并通过 `target_compile_features` 显式声明 cxx_std_17，避免隐式依赖。

**跨平台策略**
- 构建系统本身不区分平台，依赖 CMake 的 `CMAKE_SYSTEM_NAME` 在配置时输出当前平台信息。
- 线程库通过 Threads::Threads 抽象层屏蔽平台差异。
- 注释中多次强调“三平台”、“零外部依赖”，表明设计目标是纯标准库 + 自研 DSP 实现，无需 TI SDK 即可编译运行。

**测试集成**
- 通过 `enable_testing()` 和 `add_test()` 将每个可执行目标注册为 CTest 用例，可直接通过 `ctest` 运行全部测试。

**缺失部分**
- 未发现 CI/CD 配置文件（如 .github/workflows、Jenkinsfile、azure-pipelines.yml 等）
- 未发现 Dockerfile、容器化脚本或交叉编译脚本
- 未发现版本管理脚本（build.sh、release.sh 等）
- 未发现包管理器配置（vcpkg.json、Conanfile、package.xml 等）