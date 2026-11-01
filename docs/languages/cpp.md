# C++ 项目使用与 AI 开发规范

## 1. 适用范围与默认选择

这是本模板重点支持的语言配置：C++17、CMake 3.23+、CTest，使用 target 级配置。默认无第三方 C++ 库或测试框架，避免初始化时联网和包管理器绑定。

- 主要目标是 Linux + GCC/Clang：默认 Ninja + GCC Debug，另有显式 Clang 和 Release presets。
- macOS 可使用 Ninja 与 AppleClang，但需要自行实测；模板不声称已认证。
- C++20/23、Qt、CUDA、ROS、嵌入式、交叉编译、公司工具链和 vcpkg/Conan 不默认启用，先确认项目约束。

如果已有成熟的 Makefile、Bazel、Meson 或公司构建系统，优先沿用，不为了模板强行迁移到 CMake。

## 2. 新项目启动

在未初始化模板副本中运行：

```sh
python3 scripts/init_project.py --name sensor-core --description "本地传感器数据处理库" --language cpp --dry-run
python3 scripts/init_project.py --name sensor-core --description "本地传感器数据处理库" --language cpp
python3 scripts/doctor.py
cmake --version
ninja --version
g++ --version
clang++ --version
python3 scripts/check.py --strict-project
```

Linux 需要 Python 3.11+、CMake 3.23+、Ninja 和目标编译器。只验证 GCC 时不要求已安装 Clang；验证双编译器矩阵时两者都要准备好。团队选择工具版本及安装来源，本模板不会自动执行 sudo 或包管理器。

最后一条命令会真正编译代码。模板不安装编译器；环境缺失时先按公司认可的方式准备工具链，不把检查改成空命令来绕过。

生成文件包括：

```text
CMakeLists.txt
CMakePresets.json
.clang-format
.clang-tidy
include/starter/project.hpp
src/project.cpp
src/main.cpp
tests/cpp/project_test.cpp
scripts/cpp_check.py
scripts/cpp_format.py
.github/workflows/project.yml
```

其中 `starter` 是示例命名空间和 target 前缀，不会按项目名自动重命名全部 C++ 标识符。第一项业务任务可按团队规范明确重命名。

## 3. 构建与测试命令

| 命令 | 行为 |
| --- | --- |
| `python scripts/cpp_check.py` | Linux 默认 linux-gcc-debug；其他平台使用 dev |
| `python scripts/cpp_check.py --preset linux-gcc-debug` | Linux Ninja + 显式 g++，Debug |
| `python scripts/cpp_check.py --preset linux-gcc-release` | Linux Ninja + 显式 g++，Release |
| `python scripts/cpp_check.py --preset linux-clang-debug` | Linux Ninja + 显式 clang++，Debug |
| `python scripts/cpp_check.py --preset linux-clang-release` | Linux Ninja + 显式 clang++，Release |
| `python scripts/cpp_check.py --preset dev` | Ninja Debug，需 compiler/Ninja 可用 |
| `python scripts/cpp_check.py --preset release` | Ninja Release |

这些脚本在模板 profile 源中以 `.in` 保存，选择 cpp 初始化后才出现在 `scripts/` 中。

实测状态（逐条命令与输出见 [补做验证记录](../design/005-macos-verification.md)）：2026-09-14 在 macOS arm64 + AppleClang 21 + CMake 4.4.3 + Ninja 1.13.2 上，`dev` 与 `release` preset 均完成真实编译、链接与 CTest（各 1/1 通过），`-DSTARTER_WARNINGS_AS_ERRORS=ON` 下 6/6 干净构建，`cpp_format.py` 通过；`-DSTARTER_SANITIZERS=ON` 与 `-DSTARTER_CLANG_TIDY=ON` 在没有对应环境时按设计明确失败。Linux GCC/Clang 四组合、Sanitizers 与 clang-tidy 正路径仍**未验证**：macOS 的 `g++`、`gcc` 只是 Apple clang 别名，不能当作 GCC 证据。

脚本依次 configure、build、CTest；前一步失败就停止，避免编译失败后继续运行旧测试。CTest 使用 `--no-tests=error`，不能以“找到零个测试”宣布通过。最小 C++ 测试不用 `assert`，避免 Release 下 `NDEBUG` 使检查失效。

默认质量命令只覆盖当前 preset。要在 Linux 本地验证全部组合，可执行：

```sh
for preset in linux-gcc-debug linux-gcc-release linux-clang-debug linux-clang-release; do
  CPP_PRESET="$preset" python3 scripts/check.py --strict-project || exit 1
done
```

也可仅运行 `CPP_PRESET=linux-clang-debug python3 scripts/check.py --strict-project`。`CPP_PRESET` 是本次进程的选择，不改变全仓库配置；CLI 的 `--preset` 优先于环境变量。

四个 Linux preset 各用独立 build 目录，明确选择 g++ 或 clang++，避免编译器缓存互相污染。它们只在 Linux 主机启用；保留的 dev/release 是使用当前默认编译器的通用 Ninja 配置，不等于双编译器已验证。

生成的 C++ 项目 CI 使用 ubuntu-24.04，分别运行 GCC/Clang × Debug/Release 四项严格检查，并打印两个编译器版本。编译器依赖 runner 提供，尚未固定精确补丁版本；需完全可复现时使用经审查的固定工具链或镜像。

编译器改变时使用新的 build 目录或经确认清理旧缓存，不直接复用原 compiler 的 CMakeCache。不要提交 build 输出。

## 4. 编译告警与分析

### 默认告警

- GCC/Clang：`-Wall -Wextra -Wpedantic`。
- 仅对第一方 target 使用 PRIVATE 告警选项，不全局污染第三方依赖。
- 可用 `--warnings-as-errors` 将告警升级为错误；默认关闭，避免不同工具链新告警阻断首次配置。

### clang-format（按需启用）

安装并固定团队认可的 clang-format 版本后运行：

```sh
python3 scripts/cpp_format.py
```

该命令只检查 `include/`、`src/`、`tests/cpp/` 中的 C++ 文件，不自动重写。如果要纳入统一门禁，在 `quality_commands` 加入该脚本，同时为 CI 准备同版本工具。新增源码目录时同步更新脚本扫描范围。

### clang-tidy（按需启用）

```sh
python3 scripts/cpp_check.py --preset linux-clang-debug --tidy
```

需要可用的 clang-tidy 和兼容编译器。Ninja 构建目录中生成 `compile_commands.json`，可供编辑器和进一步分析使用，不应把本机路径写入版本控制。

增量编译可能不会重新分析未修改文件。需要完整分析时使用单独、干净的分析构建目录或经确认重建；不能把无编译动作的运行说成全量静态分析通过。默认配置不把 clang-tidy 的所有诊断升级为失败，团队需按规则配置 `WarningsAsErrors` 后才能作为严格静态分析门禁。

实测状态（2026-09-14，macOS arm64 + clang-tidy 23.1.1 + Ninja + `dev` preset）：正路径已真实验证。`-DSTARTER_CLANG_TIDY=ON` 后 `CLANG_TIDY_EXECUTABLE` 确实写入 CMakeCache，干净重建的 verbose 日志中 clang-tidy 被调用 3 次；向源码故意加入 `const char* p = NULL;` 后，分析确实报出 `warning: use nullptr [modernize-use-nullptr]`，证明不是静默跳过。**但构建仍退出 0**，因为 `WarningsAsErrors` 为空——这印证了上一段：不配置 `WarningsAsErrors` 时 tidy 诊断不会使门禁失败，不得把退出 0 当成静态分析无问题。Linux GCC/Clang 下的 tidy 正路径仍未验证。

### Sanitizers（按需启用）

```sh
python3 scripts/cpp_check.py --preset linux-clang-debug --sanitizers
```

当前明确只支持 Linux GCC/Clang 的 AddressSanitizer + UBSan 配置。其他平台需单独设计，不自动套用这些参数。线程检测 TSan 需要独立构建，不应与 ASan 混开。测试通过仍不代表没有内存或并发缺陷。

每次脚本运行按传入标志显式设置这些可选开关；不传标志会关闭对应选项，避免旧缓存让验证含义不清楚。

## 5. AI 在 C++ 项目中的具体约束

开始修改前核对：编译器和版本、C++ 标准、目标架构、操作系统、构建方式、依赖管理、异常/RTTI 政策、ABI 及性能约束。

### 对象生命周期与资源

- 优先 RAII、值语义和明确所有权；没有明确理由时不使用手工 `new/delete`。
- 使用 `unique_ptr` 表示独占所有权；仅在共享生命周期确有需求时使用 `shared_ptr`。
- 裸指针和引用可以表示非拥有关系，但必须说明有效期和可空性。
- 返回或保存 `string_view`、`span`、迭代器、引用时检查底层对象生命周期与容器失效规则。
- 避免隐藏的全局可变状态，不用未经测量的复杂缓存替代清晰实现。

### API 与兼容性

- 公共头文件保持自包含，避免泄露不必要的依赖；不要在公共头文件使用 `using namespace`。
- 按项目已确认的异常策略处理错误，不擅自启用/关闭异常和 RTTI，不滥用 `noexcept`。
- 修改 public class 布局、虚函数、导出符号、调用约定，或让 STL 类型跨越共享库边界时，检查 ABI 和工具链兼容性；尤其注意 Linux 上混用 libstdc++ 与 libc++、以及 `_GLIBCXX_USE_CXX11_ABI` 取值不一致导致的链接期或运行期失败。
- 不默认项目支持最新标准特性。升级标准需同步 CMake 标准、target compile features、工具链矩阵和设计。

### 正确性与并发

- 检查整数溢出、符号转换、越界、未初始化数据和未定义行为；测试边界值。
- 写清线程安全保证、锁范围、回调重入和线程退出顺序。
- 不以 `volatile` 代替线程同步，不在缺乏需求和证据时引入 lock-free 结构。
- 先测量再优化，性能声明应包含编译配置、输入规模、测量方法和平台。

### 构建与第三方库

- 优先 `target_link_libraries`、`target_include_directories`、`target_compile_definitions`，避免全局编译选项。
- 根据项目需求固定依赖版本和来源。未经授权不自动 FetchContent 下载、执行第三方 CMake、vcpkg 或 Conan 安装。
- 编译第三方代码也会执行构建逻辑，不把“只构建”当成没有安全影响。

## 6. 设计文档的 C++ 补充项

在通用设计章节中按需加入：

- 库与可执行文件边界、public/private 头文件和 target 依赖图。
- 所有权、生命周期、复制/移动语义、错误及异常策略。
- 线程模型、同步与关闭顺序。
- ABI、符号可见性、编译器/运行库版本和兼容性要求。
- C++ 标准、构建 preset、Debug/Release、工具链及分析工具版本。
- 单元/集成/性能/内存验证的实际命令、结果与未覆盖平台。

不要只记录“编译通过”。一个编译器的 Debug 通过不等于 Release、GCC/Clang 或所有 CPU 架构都兼容。

## 7. 推荐给 AI 的第一项任务

```text
请读取 AGENTS.md、project.json、docs/languages/cpp.md 和现有 CMake 配置。
先核对目标编译器、C++ 标准、平台、依赖以及异常/RTTI/ABI 约束。
不确定的问题先询问，不擅自改构建系统或引入第三方库。

只实现已确认的第一个小功能，优先清晰的所有权与生命周期设计。
补充正常、边界和失败场景测试；不要仅使用 Release 下会失效的 assert 作为测试判定。
运行已授权的 preset 与严格检查，说明实际 compiler、配置、结果和未验证的平台。
同步设计中的接口、资源管理及线程安全说明。
```
