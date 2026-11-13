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
| [009：mini-agent v3 技术方案（编码能力）](009-coding-v3.md) | 已实现（2026-11-05 M1–M4；gcc/clang preset + tidy -Werror + ASan + 75 项 pty smoke（含 v3 审批 diff 场景）+ mock 编码流 E2E + allowlist 档 E2E 全通过，验证表见 §8，偏差 D1–D7 见 §9；真机 LLM 编码冒烟已由维护者 2026-11-05 实测，记录见 §9）；待人工评审 | 文件编辑五工具、session 级 workspace 边界、diff 审批显示、git 工具（add/commit 随审批档，无 push）、默认 system prompt、CLI allowlist 档；关联 [ADR 007](../decisions/007-file-tools-workspace.md)（已实现待评审）、[ADR 008](../decisions/008-git-tool-policy.md)（**已接受**）；Q1/Q4b/Q5 待批复 |
| [010：mini-agent v4 技术方案（模型元数据）](010-model-metadata.md) | 已实现（2026-11-07；agent_test 新增三组测试 + CTest 3/3 + strict-project + cpp_format + mock E2E + PTY 实屏断言全通过，验证表见 §8，偏差 D1–D4 见 §9）；待人工评审 | SSE usage 采集（stream_options.include_usage）与跨轮汇总、字符类估算兜底、config `context_window`/`reasoning_effort`（配置权威、透传）、TUI 底部状态栏 ctx%/out/reasoning 段、`/context` `/reasoning` 命令；借鉴 [TUI 调研](../references/tui-agent-ui-survey.md) §4 状态栏专题 |
| [011：mini-agent v5 技术方案（文件内容预览 + TUI 编辑→diff→渲染一体化 + 语法高亮）](011-preview-tui-integration.md) | 已实现（2026-11-09；agent_test v5/highlight 断言 + CTest 3/3×debug/release + strict-project + cpp_format + ASan + PTY v5 断言通过，CLI TTY 回显为手工证据，基线对照确认 3 项既有 PTY 失败非本设计引入；验证表见 §8，偏差 D1–D4 见 §9）；待人工评审 | `ToolRuntime::result_preview()` 执行后钩子、`FileRuntime::preview_file` 第六文件工具（正文经 UI 通道渲染、不入模型上下文）、write/edit 成功后 applied unified diff 暂存、TUI transcript 回显 diff/渲染块（共享 `tint_diff_line` 色表）、`/preview <path>` 命令与侧栏 `p` 键、CLI TTY stderr 回显、默认 system prompt 增 preview_file 用法、手写零依赖行词法语法高亮 `hl::`（代码围栏/预览/preview_file 三处接入）；承接 [009](009-coding-v3.md) |
| [012：mini-agent v6 技术方案（多 provider 登录与统一模型切换）](012-multi-provider.md) | 已实现（2026-11-13，验证见其 §8；Q1–Q4 已批复） | config 可选 `providers` 命名档案表 + 活动指针展开、`/provider add\|rm\|list` 与 `/login <name>`、`/models` 聚合菜单、`/model <provider>/<name>` 限定名、per-profile AuthProbe 与模型元数据、env/CLI 兼容；承接 [007](007-agent-v1.md) §5.5 扩展点、[010](010-model-metadata.md) |
| [013：mini-agent v7 技术方案（provider 强制命名与配置结构统一）](013-named-providers.md) | 已实现（2026-11-14 M1–M3；四 preset + tidy + ASan + pty smoke 156 PASS + CLI smoke 全绿，验证见其 §8，偏差 D1–D6 见 §9；Q1–Q4 已批复）；待人工评审 | config schema v2：废弃顶层 LLM 五字段、`providers`+`active_provider` 单一事实来源、运行时 `current` 单向派生（env 恒赢）、自动 `default` 档案、v1 一次性加载迁移、rm-active 新语义；破坏性变更，推翻 012 §4 兼容性取舍；关联 [ADR 009](../decisions/009-named-providers-schema-v2.md)（已接受） |

0.4 将目标平台收敛为 Linux 与 macOS，删除了全部 Windows/MSVC 配置、命令示例与 CI 矩阵；取舍、迁移步骤与完整验证表见 [006](006-drop-windows-support.md)。上表 001–005 中保留的 Windows 相关文字属于当时的真实验证记录，**不代表当前仍支持该平台**；其中描述*当前配置*的段落已就地标注 0.4 变更并回链 006。[005](005-macos-verification.md) 中标为未执行的 Rust 完整门禁与 clang-tidy 正路径，已在 0.4 轮真实跑通并附反证，详见 006 第 8 节。

新增业务设计时复制 [设计模板](../../templates/design.md)，使用稳定编号和功能名称命名，并在这里登记标题、状态和关联任务。

调研报告本身的归类规则见 [参考资料索引](../references/README.md) 的「调研文档放哪里」：外部实现的横向对比放该目录，本项目采纳结论写进设计文档的「参考」章节，只留取舍结论的写 ADR。

## 生命周期

草案 → 待评审 → 已接受 → 已实现；不再适用时标记已废弃。

“已接受”需要记录实际决策依据；“已实现”需要代码核对及真实验证记录，仍须明确未验证项。文档状态不等同于人工审批通过。

开发前和交付后维护同一份文档，利用版本控制保留演进，不重复生成两份彼此矛盾的设计。

## 文档规模

- 小型修复：PR 记录原因、变更和验证；说明为何无需独立设计。
- 一般功能：至少包含目标、范围、方案、接口、参考、验证与限制。
- 跨模块、公开接口、安全或数据迁移变更：完整设计，必要时附 ADR。

系统级内容放在 [架构总览](../architecture.md)。最终核对遵循 [设计交付流程](../../workflows/finalize-design.md)。
