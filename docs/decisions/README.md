# 架构决策索引

业务架构决策（2026-11-01 随"开始第一步的开发"指示接受，实施差异见各 ADR 与 [007 第 9 节](../design/007-agent-v1.md)）：

| ADR | 状态 | 决策 |
| --- | --- | --- |
| [001：会话形态](001-session-model.md) | 已接受 | 一进程一会话，session 原子落盘 `~/.local/share/mini-agent/sessions/` |
| [002：HTTP 与 JSON 依赖](002-http-json-dependency.md) | 已接受 | libcurl 系统依赖 + vendored nlohmann/json（落地前必须核实许可证） |
| [003：TUI 实现](003-tui-approach.md) | 已接受 | 自绘最小 TUI（termios + ANSI），不引入 FTXUI |
| [004：Markdown 解析](004-md4c-parser.md) | 已接受 | Markdown 解析层 vendored md4c v0.6.0（MIT），自写 SGR writer |

使用 [ADR 模板](../../templates/adr.md) 新增决策，按 `001-decision-name.md` 命名并在上表登记标题、状态和关联设计。

仅记录值得长期保留的取舍，例如关键依赖、模块边界、数据模型、协议和安全策略。不要为每个实现细节生成 ADR。

决策被替代时保留原记录，并互相链接；AI 不得虚构决策人或审批记录。
