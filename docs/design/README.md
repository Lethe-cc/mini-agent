# 设计文档索引

**测试设计**独立成目录：[testing/](testing/README.md) —— 三层测试体系、运行命令、mock 服务、CI 映射；用例分组清单见 [testing/case-inventory.md](testing/case-inventory.md)。其他文档只引用该目录，不重复展开。

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
| [008：mini-agent v2 技术方案（工具循环 + 命令执行 + MCP）](008-mcp-tools.md) | 已实现（M1–M4：工具循环/run_bash+审批/MCP stdio/MCP Streamable HTTP；gcc×2+clang preset、tidy -Werror、ASan、agent_test/mcp_test/pty smoke/mock E2E 通过，真实 deepwiki 连通、qwen 形状偏差已记录 D3）；待人工评审 | function calling 循环、内置 run_bash 与审批策略、MCP 客户端（传输抽象 + stdio/Streamable HTTP）、session schema v2；关联 [ADR 005](../decisions/005-mcp-transport-abstraction.md)（提议）、[006](../decisions/006-tool-approval-policy.md)（已接受，2026-11-21） |
| [009：mini-agent v3 技术方案（编码能力）](009-coding-v3.md) | 已实现（2026-11-05 M1–M4；gcc/clang preset + tidy -Werror + ASan + 75 项 pty smoke（含 v3 审批 diff 场景）+ mock 编码流 E2E + allowlist 档 E2E 全通过，验证表见 §8，偏差 D1–D7 见 §9；真机 LLM 编码冒烟已由维护者 2026-11-05 实测，记录见 §9）；待人工评审 | 文件编辑五工具、session 级 workspace 边界、diff 审批显示、git 工具（add/commit 随审批档，无 push）、默认 system prompt、CLI allowlist 档；关联 [ADR 007](../decisions/007-file-tools-workspace.md)（**已接受**，2026-11-21）、[ADR 008](../decisions/008-git-tool-policy.md)（**已接受**）；Q1/Q4b/Q5 待批复 |
| [010：mini-agent v4 技术方案（模型元数据）](010-model-metadata.md) | 已实现（2026-11-07；agent_test 新增三组测试 + CTest 3/3 + strict-project + cpp_format + mock E2E + PTY 实屏断言全通过，验证表见 §8，偏差 D1–D4 见 §9）；待人工评审 | SSE usage 采集（stream_options.include_usage）与跨轮汇总、字符类估算兜底、config `context_window`/`reasoning_effort`（配置权威、透传）、TUI 底部状态栏 ctx%/out/reasoning 段、`/context` `/reasoning` 命令；借鉴 [TUI 调研](../references/tui-agent-ui-survey.md) §4 状态栏专题 |
| [011：mini-agent v5 技术方案（文件内容预览 + TUI 编辑→diff→渲染一体化 + 语法高亮）](011-preview-tui-integration.md) | 已实现（2026-11-09；agent_test v5/highlight 断言 + CTest 3/3×debug/release + strict-project + cpp_format + ASan + PTY v5 断言通过，CLI TTY 回显为手工证据，基线对照确认 3 项既有 PTY 失败非本设计引入；验证表见 §8，偏差 D1–D4 见 §9）；待人工评审 | `ToolRuntime::result_preview()` 执行后钩子、`FileRuntime::preview_file` 第六文件工具（正文经 UI 通道渲染、不入模型上下文）、write/edit 成功后 applied unified diff 暂存、TUI transcript 回显 diff/渲染块（共享 `tint_diff_line` 色表）、`/preview <path>` 命令与侧栏 `p` 键、CLI TTY stderr 回显、默认 system prompt 增 preview_file 用法、手写零依赖行词法语法高亮 `hl::`（代码围栏/预览/preview_file 三处接入）；承接 [009](009-coding-v3.md) |
| [012：mini-agent v6 技术方案（多 provider 登录与统一模型切换）](012-multi-provider.md) | 已实现（2026-11-13，验证见其 §8；Q1–Q4 已批复） | config 可选 `providers` 命名档案表 + 活动指针展开、`/provider add\|rm\|list` 与 `/login <name>`、`/models` 聚合菜单、`/model <provider>/<name>` 限定名、per-profile AuthProbe 与模型元数据、env/CLI 兼容；承接 [007](007-agent-v1.md) §5.5 扩展点、[010](010-model-metadata.md) |
| [013：mini-agent v7 技术方案（provider 强制命名与配置结构统一）](013-named-providers.md) | 已实现（2026-11-14 M1–M3；四 preset + tidy + ASan + pty smoke 156 PASS + CLI smoke 全绿，验证见其 §8，偏差 D1–D6 见 §9；Q1–Q4 已批复）；待人工评审 | config schema v2：废弃顶层 LLM 五字段、`providers`+`active_provider` 单一事实来源、运行时 `current` 单向派生（env 恒赢）、自动 `default` 档案、v1 一次性加载迁移、rm-active 新语义；破坏性变更，推翻 012 §4 兼容性取舍；关联 [ADR 009](../decisions/009-named-providers-schema-v2.md)（已接受） |
| [014：Provider 流前重试与退避](014-provider-retry.md) | 已实现（2026-11-14；agent_test 新增助手/桥接用例 + CLI smoke flaky 场景 9 断言 + TUI smoke 159 PASS + GCC/Clang Debug CTest 3/3，验证见其 §6）；待人工评审 | `complete_chat` 流前重试（网络错误/408/429/5xx，指数退避 + Retry-After，流已开始门禁）、`MINI_AGENT_RETRIES` env 旋钮、`provider_retry` 元数据日志（TUI 经 TurnBridge 队列）；承接 007/008 失败轮回滚与事件/渲染分离 |
| [015：上下文溢出提示与手动 /compact](015-context-compact.md) | 已实现（2026-11-14；agent_test 溢出模式/压缩构造用例 + TUI smoke 167 PASS（新增 8 断言）+ CLI smoke 全绿 + GCC/Clang Debug CTest 3/3，验证见其 §6）；待人工评审 | 非 2xx 错误体提取（有界/截断/净化）、`context_overflow_message` 窄模式集 + TUI/CLI 溢出提示、TUI `/compact`（独立 worker、有界转录、摘要替换历史、失败/中断不动历史）；007 §5.5 压缩扩展点首次落地，自动压缩仍为非目标 |
| [016：会话导出与清理](016-session-lifecycle.md) | 已实现（2026-11-14；agent_test markdown 导出用例 + CLI smoke lifecycle 段 9 断言 + TUI smoke 171 PASS + GCC/Clang Debug CTest 3/3，验证见其 §5）；待人工评审 | `session export`（md 纯函数转录/json 盘上格式，--out 0600）、`session prune --older-than DAYS [--dry-run]`（mtime 阈值必填、活锁跳过）；无 schema 变化 |
| [017：上下文管理增强](017-context-management.md) | 已实现（2026-11-15；agent_test test_context_017 28 断言 + TUI smoke 180 PASS 双环境 + CLI smoke ALL PASS + 四 preset CTest 3/3，验证见其 §6，偏差 D1–D3 见 §7）；待人工评审 | 锚点+尾部计量单一读数、`/context` breakdown、max(15%,16k) 压力提示（只提示不自动）、/compact 升级（备份→切点保留尾部→机械剪枝→结构化迭代摘要+NUDGE）、溢出排除集+Exceeded 合成；调研输入 [ctx-compaction-survey](../references/context-compaction-survey.md)；不推翻 014/015 |
| [018：预览 pane](018-preview-pane.md) | 已实现（2026-11-15；test_pane_helpers 8 断言 + TUI smoke 193 PASS 双环境 + CLI ALL PASS + 四 preset CTest 3/3，验证见其 §7，偏差 D1–D5 见 §9）；已提交 `b419ba8`；待人工评审 | live 区内可关可滚预览视图（/preview 与侧栏 p 共用）+ 单级目录浏览器；转录仅 open/closed 标记、空闲门控、复用 extra_rows 与行记账；不引入 alternate screen（ADR 003 保留） |
| [019：代码模块拆分](019-module-split.md) | 已实现（P1 `04b36f1`、P2 `a879bcd`、P4 `c1a57f3` 收官，P1–P4 全部完成，2026-11-21）；待人工评审 | 行为保持分阶段拆分：P1 纯函数 + P2 CLI/session_serde/tools + P3 screen 层 + P4 TUI（preview/sidebar/workers/turn/slash/input/frame/keys，tui.cpp →1268）；TuiState 完整状态对象未建（以 TurnState 首片 + Ctx 引用替代，见其 §8）；单文件≤~1000 行目标基本达成，不新增依赖 |
| [020：状态流转集中描述](020-state-machines.md) | **草案 / 待人工评审**（2026-11-20；纯文档） | 连接/认证、回合生命周期、审批流、后台 worker、预览 pane、session 六组状态机（PlantUML），集中 007/008/010/013/017/018/ADR001/006 的散落描述 |
| [021：MCP 全能力客户端（cxxmcp 迁移）](021-mcp-full-client.md) | **M0/M1 已实现，全门禁验收，默认后端已翻转 cxxmcp**（2026-11-22，待人工评审；M2/M3 未启动） | 维护者方向"resources/prompts/sampling/elicitation/tasks/追新版本"推翻 008/ADR005 的 tools-only 裁剪；引入 cxxmcp 取代自写 MCP 三层（M0 spike 否决门 → M1 双轨底座替换 → M2 能力分期 → M3 收尾）；M0 全绿：四 preset 编译、mock/deepwiki/线程重放、ASan/TSan 干净、conformance 2025-11-25 同口径 247/0 精确复现 + 最新 wire 463/0（6 draft-auth 场景挂起为上游析构缺陷，M2 前须确认修复，见其 §9）；M1：vendored client-only SHA `3c25a8af`（LICENSE 归档）、`McpSession` 接缝双轨、四 preset × debug/release × 双 env 全绿、回放 7 一致+3 已接受偏差、pty/冷启动/tidy/ASan/UBSan/strict-project 通过，默认翻转 cxxmcp（`MINI_AGENT_MCP_BACKEND=legacy` 回滚保留至 M3）；关联 [ADR 010](../decisions/010-mcp-full-client-cxxmcp.md)（已接受，取代 005、修订 002，前提 3 已关闭） |
| [022：MCP M2 能力扩展](022-mcp-capabilities-m2.md) | **方向已获批（2026-11-22 Q1–Q5 批复）；M2a 已实现待人工评审（2026-11-23 全门禁验收）**；整体逐节人审未做 | 021 §5.1 M2 的细化设计：M2a resources/prompts（合成工具 + /prompt 注入）→ M2b sampling/elicitation（per-server 默认 OFF、ServerRequestBridge 审批队列、非交互拒绝，对应 [ADR 011](../decisions/011-mcp-server-request-approval.md) **提议/待人审**，获批前 M2b 不实现）→ M2c tasks（状态机增补 020）；版本白名单上移 2025-11-25（硬前置：上游析构缺陷复核）；配置加字段不 bump schema；Q5 采用受控部分并行（开发并行、验收合并串行，其 §5.6/§7） |

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
