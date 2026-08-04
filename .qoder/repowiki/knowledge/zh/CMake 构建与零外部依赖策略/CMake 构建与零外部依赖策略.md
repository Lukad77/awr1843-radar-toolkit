---
kind: dependency_management
name: CMake 构建与零外部依赖策略
category: dependency_management
scope:
    - '**'
source_files:
    - CMakeLists.txt
---

本项目采用 CMake 作为唯一构建系统，依赖管理遵循「最小化第三方库」原则：除标准库和平台线程库（pthread/Threads）外，不引入任何第三方 C/C++ 包管理器或 vendored 库。

**系统与工具**
- CMake 3.15+ 作为构建入口，通过 `find_package(Threads REQUIRED)` 仅链接 POSIX 线程库，Windows 上为空实现，保证跨平台。
- 所有 DSP 算子（FFT、CFAR、角度估计等）均为自实现，注释明确标注「零外部依赖，三平台」。
- 无 go.mod、package.json、vcpkg.json、Conanfile、CocoaPods 等任何语言级依赖清单文件。

**关键文件**
- `CMakeLists.txt`：集中定义全部 target（core/pipeline/spool/dsp tests 及 demo），统一设置 C++17、链接 Threads::Threads。
- `src/core/`、`src/dsp/`、`src/pipeline/`、`src/transport/` 中所有头文件与源文件均只依赖标准库与 pthread。

**架构与约定**
- 每个测试目标独立编译，通过 `add_test` 注册到 CTest，无需额外测试框架。
- 依赖通过 `target_link_libraries PRIVATE` 精确限定作用域，避免隐式传播。
- 项目未使用任何私有仓库或代理，依赖来源完全透明（仅系统库）。

**开发者规则**
- 新增功能应优先使用 C++ 标准库；若确需第三方库，需在 CMakeLists.txt 中显式 `find_package` 并说明跨平台兼容性。
- 禁止直接 `#include` 非标准库头文件而不声明链接依赖。
- 保持「零外部依赖」DSP 策略，新算子应自实现而非引入加速库。