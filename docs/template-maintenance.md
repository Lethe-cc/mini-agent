# 模板维护、升级与发布

## 版本

当前模板版本为 0.4.0，记录在 `project.json` 的 `template_version`。0.2 提供 C++/Python/TypeScript 配置，0.3 新增 Go/Rust；0.3.1 按使用者主要环境将 C++ 默认验证目标调整为 Linux GCC/Clang；0.4 移除全部 Windows 支持，目标平台收敛为 Linux 与 macOS（**破坏性变更**，见下文 0.4 节）。缺少 language 字段的 0.1 配置仍可读取为 generic。已有项目应在分支中比较并迁移，不重跑初始化。项目从模板创建后不会自动继承模板更新，不提供自动覆盖式同步。

升级建议：在独立分支比较版本变化，人工迁移规则和脚本，保留项目自己的上下文、参考、质量命令和设计。验证通过后再更新模板版本。后续有多项目需求时，可提取版本化工具包或共享 CI。

## 发布为模板

1. 将本目录作为独立仓库根目录，不要把父目录或私人文件一同发布。
2. 确认 `.gitignore`、许可证、第三方授权和公司数据政策。
3. 发布前运行 doctor、基础检查及实际初始化试用。初始化试用应在临时副本中进行，保留发布模板的未初始化状态。
4. 在代码平台启用模板功能。GitLab 配置随实例版本、权限与套餐不同，不假设本仓库自动完成平台设置。
5. GitHub CI 默认测试 Ubuntu、Python 3.11/3.13。这个矩阵是待运行配置，不是已经通过的认证；自 0.4 起只覆盖 Linux，macOS 不在 CI 矩阵中。
6. 保护分支并设置必要的状态检查与人工审批；这些是平台设置，不会因复制模板自动启用。

## 当前安全与功能边界

- generic 初始化只处理两个声明文件；语言配置额外创建 manifest 中的文件和项目 CI。提供 dry-run、参数和路径校验、文件冲突及重复执行保护。普通写入错误时尝试恢复原内容并删除本次新增文件；断电、并发写入、回滚本身失败不在事务保证范围内。运行前保留版本控制或备份。
- 工具只使用标准库，但质量命令和测试本身仍是任意仓库代码，必须审查后执行。
- 子进程超时不保证清理所有后代进程；长驻服务和不可信代码请使用受限容器或专用执行环境。
- GitHub Actions 使用版本 tag，团队可按供应链政策改为已核实的完整 commit SHA，并设置更新机制。
- CI 不使用 `pull_request_target`，默认只读权限且 checkout 不保留凭据。仍需审查工作流变更，不给不可信任务提供生产凭据。
- 语言配置提供最小构建/测试门禁，不代表业务覆盖完整。默认没有密钥扫描、依赖漏洞扫描或完整 Markdown linter，格式与静态分析工具也需要显式准备和接入。
- 外部参考不自动下载，也未在本次模板编写中在线核验。
- AI 规则不是权限边界；薄适配仍需在实际工具客户端中验收。

## 0.1 交付历史

0.1 版本的实际验证结果如下：

| 环境或检查 | 实际结果 |
| --- | --- |
| 本地环境 | Python 3.14.4 |
| `python3 scripts/doctor.py` | 通过；Git 可用；模板未初始化；业务质量命令尚未配置 |
| `python3 scripts/check.py` | 基础配置与文档检查通过，20 个工具单元测试全部通过 |
| GitHub Actions Ubuntu、Python 3.11/3.13 | 已提供配置，尚未运行 |
| AI 客户端入口自动发现 | 尚未实测，按启动验收文档验证 |
| 业务质量验证 | 未配置，不在模板基础检查通过的含义内 |

验证没有初始化发布模板，也没有下载第三方来源、安装依赖或操作 Git。后续运行请保留真实环境、命令和结果，不要把已提供配置当成已通过验证。

## 0.2 语言配置验证

新版本验证结果在 [语言配置设计](design/002-language-profiles.md) 中独立记录：37 个工具测试通过；三种语言生成配置通过；C++ 格式检查通过；Python 严格检查及 2 个应用起步测试通过；Node 检查脚本语法通过。

C++ 实际构建在 0.2 时期因环境缺少编译器而未完成；TypeScript 未安装编译器依赖，未执行完整类型检查、构建和应用测试。clang-tidy、Sanitizers 和远端 CI 尚未运行。不能将后续显式跳过环境阻塞项的复查成功解释为这些阶段通过。

不复用上面的 0.1 历史记录作为新代码证据。通用 CI 是 tooling-only；实际项目必须验证并要求 profile 生成的 project CI。

## 0.3 Go/Rust 验证

新增功能的实际结果与限制独立记录在 [Go/Rust 设计](design/003-go-rust-profiles.md)：0.3 的 46 个工具测试通过，两个临时项目的初始化与基础配置检查通过。本地缺少 Go/Rust 工具链，未执行其真实构建/测试，不把跳过算成通过。初始化不安装 Go/Rust；生成的项目 CI 会在显式准备步骤安装适用工具链。

## 0.3.1 Linux C++ 验证

详见 [Linux C++ 设计](design/004-linux-cpp.md)：48 个工具测试、C++ 临时项目基础配置和 CMake presets 解析均通过。目标 Linux 编译与 CTest 尚未执行。

## 0.3.1 补做验证（macOS arm64，2026-09-14）

逐条记录见 [macOS 补做验证记录](design/005-macos-verification.md)。本轮在临时副本中完成：C++ AppleClang 21 的 Debug/Release 真实编译 + CTest + `-Werror` 干净构建 + clang-format 检查；Go 1.27.1 的 gofmt/vet/test/build、`--race`、源文件 md5 不变与未格式化文件的失败对照；Python 严格检查；Rust manifest/lock 结构、缺锁与缺工具链的失败路径；TypeScript 缺依赖的失败路径；工作流 YAML 结构；pi 0.84.4 入口自动发现与 `--no-context-files` 对照组。

未因本轮而改变状态的项：Linux GCC/Clang 四组合与 Sanitizers 正路径（本机非 Linux、无容器）、clang-tidy 正路径（未安装）、Rust 真实构建（无工具链）、TypeScript 完整门禁（未安装依赖）、Python 3.11/3.13 本地矩阵、远端 CI（本目录不是 Git 仓库）。macOS 上 `/usr/bin/g++` 是 Apple clang 别名，不得当作 GCC 证据。

## 0.4 移除 Windows 支持，聚焦 Linux 与 macOS

设计取舍、逐文件变更、破坏性迁移步骤与完整验证表见 [006：移除 Windows 支持](design/006-drop-windows-support.md)。

要点：目标平台收敛为 Linux 与 macOS。删除 C++ 的 `windows-msvc` / `windows-msvc-release` presets、`if(MSVC)` 告警分支与 `cpp_check.py` 的 Windows 默认选择；通用 `ci.yml` 与 Go/Python/Rust/TypeScript 的 `project.yml` 矩阵移除 Windows runner；文档命令统一为 `sh` + `python3`。这是**破坏性变更**，既有项目的 `CPP_PRESET=windows-msvc` 会被直接拒绝（退出 2），迁移步骤见 006 第 7 节。

52 个工具测试通过（新增 4 项锁定平台收敛）。本轮在 macOS arm64 临时副本中完成了真实工具链验证：C++ `dev`/`release` 编译 + CTest + `-Werror` 标志核对 + clang-tidy 正路径（含「故意引入 `NULL` 违规被真实检出」反证）；**Rust 完整四步门禁首次跑通**（fmt → clippy `-D warnings` → test → build，含两组反证）；Go gofmt/vet/test/build 与 `--race`；Python 起步应用测试。仍未验证：Linux GCC/Clang 四组合与 Sanitizers 正路径、Rust 多工具链与 MSRV（无 `rustup`）、TypeScript 完整门禁（不联网装依赖）、Python 3.11/3.13 本地矩阵、远端 CI（仓库已配有远端但尚无提交）。macOS/clang 结果不得当作 Linux GCC 证据。

模板自身历史设计见 [模板核心设计](design/001-template-core.md)。业务接入后的最终设计按 [交付流程](../workflows/finalize-design.md) 核对。
