# mini-agent v2 工具与 MCP 使用指南

本文是 v2 工具层（工具调用循环、命令执行、MCP 客户端）的**使用者文档**：配置、命令、交互与安全边界。设计与实现依据见 [008：v2 技术方案](design/008-mcp-tools.md)（已实现、待人工评审），取舍记录见 [ADR 005](decisions/005-mcp-transport-abstraction.md)、[ADR 006](decisions/006-tool-approval-policy.md)（均为提议）。全量参数面（CLI/环境变量/config.json/TUI，含 v3–v5 新增）另见[参数使用大纲](parameters.md)。

## 1. 能力总览

| 能力 | 形态 | 默认策略 |
| --- | --- | --- |
| 工具调用循环 | `run_turn` 内层循环：模型请求 tool_calls → 审批 → 执行 → Tool 消息回填 → 再请求，直至最终文本 | 上限 8 轮（`max_tool_rounds`） |
| 命令执行 | 内置工具 `run_bash`（`/bin/bash -lc`，合并 stdout+stderr，超时杀进程组，输出 32 KiB 首尾截断） | 逐次人工确认 |
| 外部工具 | MCP 客户端，stdio 与 Streamable HTTP 两种传输；server 工具以 `server__tool` 进入同一注册表 | 逐次人工确认 |
| 会话注入 | `--mcp NAME=SPEC` 运行时增改 server（不落盘） | — |
| 清单查看 | `mini-agent mcp list`、TUI `/mcp`、`/tools` | — |

无 MCP 配置且 `--tools off` 时，请求体、落盘格式、启动路径与 v1 逐字节一致；MCP server 一律**惰性连接**，启动/`--help` 零网络。

## 2. 配置（config.json）

位置：`$XDG_CONFIG_HOME/mini-agent/config.json`（默认 `~/.config/mini-agent/config.json`，`MINI_AGENT_CONFIG` 可改）。新增字段全部可选，缺省即 v1 行为：

```json
{
  "base_url": "https://…", "api_key": "…", "model": "…",
  "tool_approval": "ask",
  "tool_allowlist": ["dev__read_wiki_structure"],
  "max_tool_rounds": 8,
  "tool_timeout_seconds": 30,
  "mcp_servers": {
    "fs":   { "transport": "stdio", "command": "npx",
              "args": ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"],
              "env": { "NODE_ENV": "production" } },
    "dev":  { "transport": "http", "url": "http://127.0.0.1:8933/mcp" },
    "wiki": { "transport": "http", "url": "https://mcp.deepwiki.com/mcp" },
    "api":  { "transport": "http", "url": "https://internal.example.com/mcp",
              "bearer_token": "tok_…", "allow_insecure_http": false }
  }
}
```

要点（完整规则在 008 §5.3）：

- `transport` 只能是 `"stdio"` 或 `"http"`（**http 与 https 都用 `http`**，scheme 由 url 决定）。写错值（如 `"https"`）会导致整份配置校验失败、全部字段不生效。
- 省略 `transport` 时：条目含 `url` → 推断为 http，否则 stdio。
- server 名限 `[A-Za-z0-9_-]{1,32}`，并作为工具名前缀（`fs/read` 形式会被 OpenAI 兼容网关静默拒绝，故用双下划线 `fs__read`；server 原始名自动净化、调用时映射回原名）。
- HTTP 明文 `http://` 仅允许 loopback；远端必须 `https://` 或显式 `"allow_insecure_http": true`（不建议：token 与数据裸奔）。
- `bearer_token` 与 api_key 同等红线：0600 落盘、日志/UI 恒脱敏（只显示 `set`）、不继承进子进程、无 env 覆盖入口；OAuth 不支持（维护者 2026-11-02 确认），server 回 401 时如实报错。
- stdio 的 `env` 只透传显式声明的键，另固定提供 `PATH`/`HOME` 基线。
- 配置错误 fail-closed：`mini-agent mcp list` 是最快的自检命令（不需 LLM 配好）。

## 3. 命令行

```
mini-agent mcp list [--mcp NAME=SPEC ...]   # 查看生效 server 清单（脱敏）
mini-agent ask [--tools off|ask|allowlist|auto] [--mcp NAME=SPEC] "问题"| -
mini-agent run [--session ID] [--new] [--temp] [--mcp NAME=SPEC]
```

`--mcp` 三种形式（可重复；同名**覆盖**配置文件条目，不写回文件，进程退出即失）：

```bash
--mcp wiki=https://mcp.deepwiki.com/mcp        # Streamable HTTP（https 任意、http 仅 loopback）
--mcp fs=npx -y @modelcontextprotocol/server-filesystem /tmp   # stdio：命令+空白拆参
--mcp 'api={"transport":"http","url":"https://h/mcp","bearer_token":"t"}'  # JSON：全量字段
```

`ask --tools` 语义：`off`（默认，脚本契约与 v1 一致，stdout 只有回复）；`ask`（TTY 内联 y/N，提示走 stderr；**非 TTY 降级为 off 并提示**——绝不在管道里静默执行命令）；`allowlist`（v3 脚本安全档：只有 config `tool_allowlist` 里的工具名执行，**名单未命中一律拒绝**并以 error Tool 消息回填模型；名单为空 = 全部拒绝，全程不阻塞、不需 TTY）；`auto`（不询问——包含 run_bash，慎用）。工具状态行走 stderr，不污染管道输出。

注意 `allowlist` 在两处语义不同：CLI `--tools allowlist` 未命中是**拒绝**（脚本要确定性），TUI `/approve allowlist` 未命中是**照问**（见 §4）。

环境变量：`MINI_AGENT_TOOL_APPROVAL=ask|allowlist|off` 覆盖审批策略；`MINI_AGENT_NO_MCP=1` 全禁 MCP（排障开关）。

## 4. TUI 交互

- 模型请求工具时 composer 上方出现审批块：
  `ALLOW dev__read_wiki_contents: {"repoName":"…"}` + 按键行
  `y` 放行本次 · `n` 拒绝（以 error Tool 消息告知模型）· `a` 本会话该工具名免问 · `?` 展开完整参数 · Esc/Ctrl-C 拒绝并中断本轮。
  120 秒无人按键自动按拒绝处理，worker 不会永久阻塞。
- 执行完成打历史行：`[tool] dev__read_wiki_contents [ok 62300ms · 45 KB]`（或 `[error …]`、`[denied]`）。
- `/tools`：列出注册表（内置 + 每个在线 server 的工具、来源），并强制握手/重列——改完配置免重启的生效手段；`/mcp`：列出配置的 server。
- **`/approve [ask|allowlist|off]`**：运行时查看/切换审批档位并持久化到 config.json（v3.1；worker 空闲时生效）。`off` 切换时回显警示行且状态行常显红色 `auto-tools`；`allowlist` 且名单为空时所有工具仍会询问（名单在 config.json `tool_allowlist` 维护）。临时覆盖也可用环境变量 `MINI_AGENT_TOOL_APPROVAL`。想"只免问这个工具"就按 `a`（会话级，不落盘）。注意 `a` 是**按工具名免问、不看参数**：对 `run_bash` 意味着本会话任意 shell 命令都不再询问（按下时 TUI 回显警示行）；不可信场景请逐次 `y` 或用 allowlist 档。
- `tool_approval=off` 时状态行常显红色 `auto-tools`。
- 深链提醒：deepwiki 类 server 的 `read_wiki_contents` 在服务端就是重操作（30–120s 常见），看状态行秒数在涨即正常；先用 `read_wiki_structure`/`ask_wiki_question` 通常更快。工具执行中的 HTTP 调用暂不可 Ctrl-C 中断（进度/取消属 v2 非目标，008 §2）。

## 5. 会话与兼容

- 回合内含任何工具消息时 session 文件自动写 `schema_version: 2`；v1 二进制读到会明确拒绝并提示升级（不会静默丢工具数据）。
- 失败回合仍不留脏历史：Provider 错误/中断时消息不落盘；但**已执行工具的副作用不会回滚**，错误行下会注明本回合已执行过工具。
- 回合上限：达到 `max_tool_rounds` 后以无工具请求收尾总结（临时 system 注记不持久化）。

## 6. 安全边界

- 无沙箱：`run_bash` 与 stdio server 以你的用户权限运行；审批是唯一的门。给不可信 server 配 allowlist/auto 前先读它的工具描述与输出面。
- MCP server 的 name/description/inputSchema 原样进模型上下文 = 注入面；`annotations` 不消费、server `instructions` 不注入 system prompt（评审问题 008 R1 的当前保守取舍）。
- 工具输出与 server 文本进 TUI 前过滤控制字符/ANSI 序列（`sanitize_display`），防转义注入终端。
- 日志只有元数据：事件 `tools`/`tool_exec`/`tool_approve`/`mcp_*` 记录工具名、状态、耗时、字节数；参数、输出、命令内容、server stderr 原文、bearer 一律不落日志。

## 7. 排障速查

| 症状 | 原因与去处 |
| --- | --- |
| 启动慢/卡 banner | 不该发生（008 偏差 D5 已修）；若复现，`MINI_AGENT_NO_MCP=1` 对照定位 |
| `config file is rejected` / 字段像没加载 | 校验 fail-closed：`mini-agent mcp list` 会打印具体条目错误（常见：transport 写成 https） |
| `[error] provider streamed tool calls in an unrecognized shape [指纹] slots=x/y/z` | 网关分片形状未覆盖（008 R2/D3）；带指纹反馈适配 |
| 工具列表正常但模型从不调用 | 检查模型端点是否支持 function calling；确认 `--tools`/`tool_approval` 不是 off 路径 |
| server 标 down、工具消失 | 看错误文本；`/tools` 重握手；`tools/mcp_probe.py` 可按同规则手动重放握手（用法 `python3 tools/mcp_probe.py --help`） |
| 审批等待久 | 120s 自动拒绝；deepwiki 重查询本身慢（§4） |

## 8. 未覆盖（当前明确排除）

resources/prompts/sampling/elicitation/roots、HTTP 侧 OAuth 与服务器发起请求（GET 常开流）、progress/取消通知、并行 tool_calls、自定义 IPC 传输（抽象已就位，实现可后补）。列表见 008 §2/§9。
