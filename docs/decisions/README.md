# 架构决策索引

业务架构决策（2026-11-01 随"开始第一步的开发"指示接受，实施差异见各 ADR 与 [007 第 9 节](../design/007-agent-v1.md)）：

| ADR | 状态 | 决策 |
| --- | --- | --- |
| [001：会话形态](001-session-model.md) | 已接受 | 一进程一会话，session 原子落盘 `~/.local/share/mini-agent/sessions/` |
| [002：HTTP 与 JSON 依赖](002-http-json-dependency.md) | 已接受；**2026-11-21 修订**（随 010 同批获批） | libcurl 系统依赖 + vendored nlohmann/json（落地前必须核实许可证）；修订：MCP 子系统豁免，登记 vendored cxxmcp/cpp-httplib + 系统 OpenSSL |
| [003：TUI 实现](003-tui-approach.md) | 已接受 | 自绘最小 TUI（termios + ANSI），不引入 FTXUI |
| [004：Markdown 解析](004-md4c-parser.md) | 已接受 | Markdown 解析层 vendored md4c v0.6.0（MIT），自写 SGR writer |
| [005：MCP 传输抽象与接入方式](005-mcp-transport-abstraction.md) | **已被 010 取代**（2026-11-21 维护者批复；021 M3 前自写三层仍是现行实现，git 历史保留作回退底座） | 自写最小 MCP 客户端：RPC 内核与 Transport 抽象分层，首批 stdio + Streamable HTTP 双实现（基线 2025-06-18，退回 2025-03-26），裁剪至 tools 面，不引入 SDK |
| [006：工具审批策略](006-tool-approval-policy.md) | **已接受**（2026-11-21 维护者批复方案 A；实现自 11-04 起按此运行） | 命令执行默认逐次人工确认（ask），allowlist/off 需显式配置 |
| [007：文件工具与工作区边界](007-file-tools-workspace.md) | **已接受**（2026-11-21 维护者批复"使用建议的方案"；实现自 2026-11-05 随 009 落地起按此运行，验证见 009 §8） | edit_file 精确唯一替换 + write_file/read_file 等五工具；session 级 workspace 根软边界；自写行级 LCS diff 进审批块 |
| [008：git 工具策略](008-git-tool-policy.md) | **已接受**（2026-11-05 维护者批复"commit 可自动，push 人工"） | 只读三件套 + add/commit 随审批档位（含 diff 预览与历史区可见性）；永不提供 push/remote 类工具；AGENTS.md 红线同步改"不自动推送" |
| [009：provider 强制命名与配置 schema v2](009-named-providers-schema-v2.md) | **已接受**（2026-11-14 维护者批复"全部默认，开始实现"） | 废弃顶层 LLM 五字段，`providers`+`active_provider` 单一事实来源 + 运行时 `current` 单向派生；v1 一次性迁移；破坏性变更，推翻 012 §4 兼容性取舍；关联 [design 013](../design/013-named-providers.md) |
| [010：MCP 全能力客户端路线（cxxmcp）](010-mcp-full-client-cxxmcp.md) | **已接受**（2026-11-21 维护者批复 021 Q1"批准"；许可证核实已完成，M1 vendoring 归档仍为引入前置） | 引入 cxxmcp（MIT/C++17，vendored 固定 SHA）取代自写 MCP 三层，扩展至 resources/prompts/sampling/elicitation/tasks 并跟随协议演进；取代 005、修订 002（OpenSSL/cpp-httplib 豁免）；spike 否决门已通过（021 §8，M1 放行待人工确认）；关联 [design 021](../design/021-mcp-full-client.md) |

使用 [ADR 模板](../../templates/adr.md) 新增决策，按 `001-decision-name.md` 命名并在上表登记标题、状态和关联设计。

仅记录值得长期保留的取舍，例如关键依赖、模块边界、数据模型、协议和安全策略。不要为每个实现细节生成 ADR。

决策被替代时保留原记录，并互相链接；AI 不得虚构决策人或审批记录。
