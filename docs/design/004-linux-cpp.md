# 004：Linux + GCC/Clang 优先的 C++ 配置

## 状态与依据

- 模板版本：0.3.1。
- 需求：使用者已明确主要环境为 Linux + GCC/Clang。
- 状态：已实现，48 个工具测试及 CMake 解析检查通过；Linux 实际构建待验证，无人工审批通过记录。
- 本文更新 [原语言配置设计](002-language-profiles.md) 的 C++ 默认目标与 CI，不改变 Go/Rust 等 profile 的行为。

## 设计变更

新增四个只在 Linux 主机启用的 Ninja presets：linux-gcc-debug、linux-gcc-release、linux-clang-debug、linux-clang-release。

GCC 明确使用 g++，Clang 明确使用 clang++。每种编译器和构建类型使用独立 binaryDir，避免更换编译器或配置时复用同一 CMakeCache。Release 继承同家族 Debug preset 并覆盖构建类型与目录。

`cpp_check.py` 在 Linux 默认选择 linux-gcc-debug，支持通过 CPP_PRESET 或显式 --preset 切换。命令行优先。保留 dev/release 通用 Ninja presets 和原有 fail-fast、CTest 无测试失败、可选分析开关。（本轮同时保留了 Windows/MSVC presets；已在 0.4 移除，见 [006：移除 Windows 支持](006-drop-windows-support.md)。）

默认 C++ 项目 CI 调整为 ubuntu-24.04 上的 GCC/Clang × Debug/Release 四项严格检查，记录编译器版本。本轮 Windows 已不出现在默认 C++ CI 中，但 preset 配置当时仍保留；0.4 起配置一并移除。

## 范围与限制

- 不在当前 Windows 编辑环境中安装 Linux、WSL、容器或编译器。
- 不以 Windows clang-format、JSON 解析或 mock 结果代替 Linux 编译。
- 不自动选择 GCC/Clang 的精确补丁版本；可复现生产构建需要固定工具链和依赖。
- clang-tidy、Sanitizers 和格式检查仍是显式选项，不声称已运行。
- 本轮其他语言保留各自的平台矩阵，没有因为 C++ 主要环境变化而删除 Windows 支持；全仓库范围的 Windows 支持在 0.4 统一移除，见 [006：移除 Windows 支持](006-drop-windows-support.md)。

## 验证计划与记录

新增回归测试检查：显式 compiler、四个独立目录、Linux host condition、build/test presets 与 CI 矩阵一致、Linux 默认选择、Clang Release 命令和 CTest 无测试保护。

实际执行结果（Windows、Python 3.14.4）：

| 检查 | 命令或方式 | 结果 |
| --- | --- | --- |
| 模板配置、文档和回归测试 | `py -3 scripts/check.py --tooling-only` | 通过，48 个工具测试全部通过 |
| C++ 临时副本初始化 | 初始化 cpp profile 后运行严格模式的基础配置校验 | 通过，母模板未初始化 |
| CMake presets 解析 | 临时副本执行 `cmake --list-presets=all` | 通过；Linux configure presets 因 host condition 在 Windows 隐藏，这是预期行为 |
| Linux GCC/Clang × Debug/Release 编译、CTest | 未执行 | 当前运行环境不是 Linux，不能由解析或 mock 测试代替 |
| 远端 CI | 未执行 | 已提供四组合配置，尚无远端运行证据 |

没有安装 Linux、WSL、编译器或依赖。真正的 Linux 四组合编译及 CTest 需在目标 Linux 或远端 CI 执行。

2026-09-14 在 macOS arm64 上补做的验证已记入 [005：macOS 补做验证记录](005-macos-verification.md)：`dev`/`release` preset（AppleClang 21）完成真实编译与 CTest，`STARTER_WARNINGS_AS_ERRORS=ON` 干净构建，Sanitizers 与 clang-tidy 在缺少环境时按设计明确失败，并补充了 presets 平台可见性的具体观察。这些结果不改变上表：Linux GCC/Clang 四组合与远端 CI 仍未执行。
