# 002：可选语言配置与 C++ 优先支持

## 状态与范围

- 适用模板版本：0.2.0。
- 状态：已实现；37 个工具测试及可运行的本地检查通过，C++ 编译受环境阻塞、TypeScript 完整构建待验证；无人工审批通过记录。
- 关联需求：大部分项目使用 C++，需要针对不同语言提供规范、工具链、质量和文档优化。
- 本文补充并更新 [0.1 核心设计](001-template-core.md) 中的初始化和 CI 边界，不把历史验证结果用于证明新配置。

## 目标与非目标

让使用者选择一种主语言即可得到一致的起步代码、测试和 CI，同时让 AI 只读取相关语言约束。重点支持 C++17/CMake/CTest，再提供 Python 和 TypeScript 起步方案。

不支持自动迁移现有项目、一次初始化多个 profile、自动下载编译器/依赖、自动选择 Qt/CUDA/嵌入式工具链或业务框架。

## 配置与生成流程

`project.json` 增加可选 `language` 字段：generic/cpp/python/typescript。缺少该字段的 0.1 配置按 generic 读取，schema_version 保持 1。模板版本更新为 0.2.0。

`init_project.py` 增加 `--language`，`--stack` 可省略并使用配置默认描述。默认 generic 仍只填两个元数据文件。

非 generic 从 `profiles/<language>/profile.json` 读取显式文件映射及质量命令。`.in` 文件不直接作为业务代码运行；初始化只对已验证的项目 slug 做名字替换。

生成前检查配置、全部来源路径、目标路径及冲突。同名文件、路径逃逸、symlink、已有质量命令冲突时拒绝执行。不会覆盖已有项目工作流。dry-run 只生成计划，不创建文件或目录。

写入失败时尝试恢复元数据、删除本次新文件并移除新建空目录。断电、并发修改或回滚失败不提供事务保证；该限制在维护文档中说明。

## 语言行为

### C++

生成静态库、CLI 和显式返回失败状态的 CTest 测试，不依赖 Release 下会消失的 assert。默认标准 C++17，target 级告警；配置 Debug/Release 的 Ninja presets。（0.2 交付时另含 VS 2022 presets，已在 0.4 移除，见 [006：移除 Windows 支持](006-drop-windows-support.md)。）

检查脚本 fail-fast 执行 configure/build/test，CTest 无测试视为失败。clang-format、clang-tidy、告警即错误和 Linux ASan/UBSan 均按需启用。（0.2 交付时脚本另含「Visual Studio generator 下显式拒绝 clang-tidy 集成」的保护分支；该分支随 Windows preset 在 0.4 一并移除，因为剩下的 preset 全部使用 Ninja。）

语言规则强调 RAII、所有权、视图生命周期、异常/RTTI、ABI、并发和编译器兼容性，最终设计需记录这些取舍及实际验证配置。

### Python

生成标准库应用、两个 unittest 及 pyproject 元数据。没有第三方依赖。Ruff 配置是可选起点，不代表已安装或执行；分发前需另选 build backend。

### TypeScript

生成 strict TypeScript、Node ESM 示例及 Node 内置测试。检查脚本清理生成目录，再类型检查、构建、测试，避免旧产物掩盖失败。通过当前 Node 调用编译器，避免统一命令依赖 npm 的平台包装器。

不自动解析或伪造 lockfile。初次需要用户审查依赖并生成锁文件，项目 CI 使用 npm ci，没有锁文件即失败。TypeScript 固定起步版本不是最新或安全审计声明。

## CI 与验证边界

通用 `ci.yml` 用 `--tooling-only` 跑模板工具矩阵，显式跳过业务命令；该标志与 strict-project 互斥。每个 profile 新增自己的 `project.yml`，使用严格检查和适用运行时。

新增单元测试覆盖多语言生成、默认兼容、dry-run、冲突保护、路径安全、回滚、CMake preset 关系、C++ 失败即停止、Python 示例真实测试及 TypeScript 配置。

这些单元测试不等于 C++ 在所有 compiler 上完成编译，也不等于已安装 TypeScript 并完成类型检查。实际工具链验证应另行记录。

## 实际验证记录

本地环境为 Windows、Python 3.14.4，通过 `py -3` 运行。验证使用临时项目副本，发布模板仍保持未初始化；没有安装依赖、下载第三方源码、初始化 Git 或推送远端。

| 检查 | 实际执行或证据 | 结果与边界 |
| --- | --- | --- |
| 模板配置、文档和工具测试 | `py -3 scripts/check.py --tooling-only` | 通过，37 个工具测试全部通过；不代表业务门禁通过 |
| 三种语言初始化后的基础配置 | 临时副本执行 initialize，再以 strict 条件检查配置和文档 | C++、Python、TypeScript 均通过 |
| CMake presets 解析 | C++ 副本执行 `cmake --list-presets` | 通过；能列出 preset 不代表 compiler 已安装 |
| C++ 默认构建 | C++ 副本执行 `py -3 scripts/cpp_check.py` | 配置阶段失败：找不到 Visual Studio 2022；未完成编译和 CTest |
| C++ Release 构建 | 未执行 | 默认工具链不可用，没有把跳过算成通过 |
| C++ 格式 | `py -3 scripts/cpp_format.py`，以及普通名称/64 字符名称回归测试 | 修复后通过；首次检查发现多行短函数与格式规则不一致，已显式配置并复查 |
| Python 应用严格检查 | Python 副本执行 `py -3 scripts/check.py --strict-project` | 37 个工具测试及 2 个应用起步测试全部通过 |
| Node 脚本语法 | `node --check scripts/ts_check.mjs` 和 `node --check test/project.test.mjs` | 通过；不等于 TypeScript 类型检查或测试执行 |
| TypeScript 类型检查、构建、应用测试 | 未安装声明的编译器依赖，没有执行 npm install | 未执行，需用户审查依赖并生成锁文件后验证 |
| clang-tidy、Sanitizers、Linux/macOS 工具链 | 未执行 | 不声称已验证 |
| 远端 CI、实际 AI 客户端入口 | 未执行 | 配置与说明已提供，仍需独立验收 |

本地曾运行包含真实 C++ 构建尝试的验证，因缺少 VS 2022 和初版格式问题返回失败。格式修复后，复查显式跳过已确认环境阻塞的编译阶段；该次复查退出成功只适用于实际执行的检查，不改变 C++ 构建未验证的状态。

2026-09-14 上表中的部分「未执行」已在 macOS arm64 上补做，逐条证据见 [005：macOS 补做验证记录](005-macos-verification.md)：C++ 在 AppleClang 21 下完成 Debug/Release 真实编译与 CTest（不等于 Windows/MSVC 或 Linux 通过）；Python 严格检查在 Python 3.12 下重跑通过；TypeScript 仍只验证了缺依赖时的失败路径；clang-tidy、Sanitizers 正路径与远端 CI 仍未执行。本节不把旧记录当作新证据。

## 已知限制与后续

- 骨架只实现项目名称示例，不能替代业务需求和测试。
- 新语言需代码、规范、测试、CI 一起接入，不只是新增提示词。
- 质量命令属于可执行仓库代码，初始化配置可信不代表依赖可信。
- 默认尚无强制格式、静态分析或安全扫描门禁，需要团队准备工具并显式接入。
- 不覆盖未知编译器、Qt、CUDA、交叉编译、软件包发布或复杂 monorepo。
