# 设计文档索引

模板自身的实现设计：

| 文档 | 状态 | 范围 |
| --- | --- | --- |
| [001：模板核心](001-template-core.md) | 0.1 历史设计，本地基础验证通过 | AI 入口、参考管理、流程、工具和质量配置 |
| [002：语言配置](002-language-profiles.md) | 0.2 已实现；37 个工具测试通过，实际工具链限制见验证记录 | C++ 优先骨架、多语言规则、安全初始化与分层 CI |
| [003：Go/Rust](003-go-rust-profiles.md) | 0.3 工具回归测试通过；实际工具链限制见记录 | Go/Rust 骨架、语言规范、离线质量命令和项目 CI |
| [004：Linux C++](004-linux-cpp.md) | 0.3.1 已实现；48 个工具测试及 CMake 解析通过，Linux 构建待验证 | GCC/Clang 明确选择、独立构建目录和四组合 CI |
| [005：macOS 补做验证](005-macos-verification.md) | 已执行；真实 C++/Go/Python 构建与 pi 入口实测通过，Linux/Rust/TypeScript/远端 CI 仍待验证 | 在 macOS arm64 上补做 002/003/004 中标为未执行的项，逐条记录命令、结果与阻塞原因 |
| [006：移除 Windows 支持](006-drop-windows-support.md) | 0.4 已实现；52 个工具测试通过，C++/Rust/Go/Python 在 macOS arm64 完成真实构建与反证；Linux 四组合、TypeScript 完整门禁、远端 CI 仍待验证 | 目标平台收敛为 Linux 与 macOS；删除 Windows/MSVC presets、`if(MSVC)` 分支、Windows CI 矩阵与 `py -3`/PowerShell 示例；含破坏性迁移步骤 |

业务功能设计：

| 文档 | 状态 | 范围 |
| --- | --- | --- |
| [007：mini-agent 第一版技术方案](007-agent-v1.md) | 已实现（内核/CLI/session/自绘 TUI；两 preset、tidy -Werror、mock E2E、pty smoke 通过）；待人工评审 | Agent 内核、CLI/TUI、多 session；关联 [ADR 001](../decisions/001-session-model.md)、[002](../decisions/002-http-json-dependency.md)、[003](../decisions/003-tui-approach.md)（均已接受） |
| [008：mini-agent v2 技术方案（工具循环 + 命令执行 + MCP）](008-mcp-tools.md) | 已实现（M1–M4：工具循环/run_bash+审批/MCP stdio/MCP Streamable HTTP；gcc×2+clang preset、tidy -Werror、ASan、agent_test/mcp_test/pty smoke/mock E2E 通过，真实 deepwiki 连通、qwen 形状偏差已记录 D3）；待人工评审 | function calling 循环、内置 run_bash 与审批策略、MCP 客户端（传输抽象 + stdio/Streamable HTTP）、session schema v2；关联 [ADR 005](../decisions/005-mcp-transport-abstraction.md)、[006](../decisions/006-tool-approval-policy.md)（均为提议） |
| [009：mini-agent v3 技术方案（编码能力）](009-coding-v3.md) | **草案**（2026-11-05 起草；同日批复 git 策略后修订，未实现；第 8 节验证计划全部未执行） | 文件编辑五工具、session 级 workspace 边界、diff 审批显示、git 工具（add/commit 随审批档，无 push）、默认 system prompt、CLI allowlist 档；关联 [ADR 007](../decisions/007-file-tools-workspace.md)（提议）、[ADR 008](../decisions/008-git-tool-policy.md)（**已接受**）；Q1/Q4b/Q5 待批复 |

0.4 将目标平台收敛为 Linux 与 macOS，删除了全部 Windows/MSVC 配置、命令示例与 CI 矩阵；取舍、迁移步骤与完整验证表见 [006](006-drop-windows-support.md)。上表 001–005 中保留的 Windows 相关文字属于当时的真实验证记录，**不代表当前仍支持该平台**；其中描述*当前配置*的段落已就地标注 0.4 变更并回链 006。[005](005-macos-verification.md) 中标为未执行的 Rust 完整门禁与 clang-tidy 正路径，已在 0.4 轮真实跑通并附反证，详见 006 第 8 节。

新增业务设计时复制 [设计模板](../../templates/design.md)，使用稳定编号和功能名称命名，并在这里登记标题、状态和关联任务。

## 生命周期

草案 → 待评审 → 已接受 → 已实现；不再适用时标记已废弃。

“已接受”需要记录实际决策依据；“已实现”需要代码核对及真实验证记录，仍须明确未验证项。文档状态不等同于人工审批通过。

开发前和交付后维护同一份文档，利用版本控制保留演进，不重复生成两份彼此矛盾的设计。

## 文档规模

- 小型修复：PR 记录原因、变更和验证；说明为何无需独立设计。
- 一般功能：至少包含目标、范围、方案、接口、参考、验证与限制。
- 跨模块、公开接口、安全或数据迁移变更：完整设计，必要时附 ADR。

系统级内容放在 [架构总览](../architecture.md)。最终核对遵循 [设计交付流程](../../workflows/finalize-design.md)。
