# mini-agent 参数使用大纲

> **文档性质**：使用者参数参考（大纲式索引）。所有条目按当前源码核对（v0.1.0，v1–v5 命令面）；日常快速查看以 `mini-agent --help` 为准，本文补充语义、默认值、持久化范围与出处。
>
> **来源**：[cli_main.cpp](../src/cli_main.cpp)、[config.hpp](../include/mini_agent/config.hpp)、[config.cpp](../src/config.cpp)、[tui.cpp](../src/tui.cpp)、[tools.cpp](../src/tools.cpp)、[session.cpp](../src/session.cpp)、[log.cpp](../src/log.cpp)。设计与取舍见文末[参考](#7-参考)。

## 目录

1. [总览与生效优先级](#1-总览与生效优先级)
2. [命令行（CLI）](#2-命令行cli)
3. [环境变量](#3-环境变量)
4. [配置文件 config.json](#4-配置文件-configjson)
5. [TUI 斜杠命令与按键](#5-tui-斜杠命令与按键)
6. [内置工具参数速查](#6-内置工具参数速查)
7. [参考](#7-参考)

## 1. 总览与生效优先级

### 1.1 四种入口

| 入口 | 形态 | 典型用途 |
| --- | --- | --- |
| CLI 子命令 | `mini-agent ask/run/session/mcp` | 脚本、管道、会话管理 |
| 环境变量 | `MINI_AGENT_*`（[§3](#3-环境变量)） | CI / 临时覆盖，不写文件 |
| 配置文件 | `config.json`（[§4](#4-配置文件-configjson)） | 持久配置，TUI 各命令的落盘目标 |
| TUI 斜杠命令 | `/provider`、`/approve` 等（[§5](#5-tui-斜杠命令与按键)） | 交互中查看/修改，部分持久化、部分仅会话 |

### 1.2 生效优先级

对文件与环境变量都存在的字段（LLM 三件套、日志、workspace、审批档位等）：

```text
CLI 旗标（--cwd/--tools/--mcp/--log-level…，仅本次进程）
  > 环境变量（非空才覆盖）
    > config.json 字段
      > 内置默认值
```

- TUI 内的修改直接写 config.json 或仅存于进程内（[§5](#5-tui-斜杠命令与按键) 逐项标注）；环境变量覆盖仍然优先于文件，所以被 env 盖住的字段在 TUI 里改了也可能"看不见效果"——排障时先 `env | grep MINI_AGENT`。
- 少数键**只有 env 入口**（`MINI_AGENT_CONFIG`、`MINI_AGENT_SESSION_DIR`、`MINI_AGENT_TIMEOUT_SECONDS`），没有对应的文件字段或 CLI 旗标。

### 1.3 "持久化"与"仅本次会话"

| 范围 | 含义 | 例子 |
| --- | --- | --- |
| 持久化 | 写入 config.json（0600，原子替换），下次启动仍生效 | `/provider` `/login` `/model` `/approve` `/context` `/reasoning`、`/log <路径/off/default>` |
| 仅本次会话 | 只改当前进程，退出即失，不写文件 | `/log info|debug|trace`、`/debug`、`/sidebar` `/sysinfo`、审批按键 `a`、`--mcp`、全部 CLI 旗标与 env |

### 1.4 安全红线（所有入口共同）

- `api_key` 与 MCP `bearer_token`：只经 0600 文件落盘；日志、UI、`mcp list` 恒脱敏；**没有环境变量以外的注入通道**（bearer 连 env 覆盖都没有）；不进模型上下文、不继承给 stdio 子进程。
- 无沙箱：`run_bash`、stdio MCP server 以当前用户权限运行，审批是唯一的门（[ADR 006](decisions/006-tool-approval-policy.md)）。
- 日志默认只记元数据；`debug/trace` 级抓包另见 [§3.2](#32-日志与抓包)。

## 2. 命令行（CLI）

### 2.1 子命令一览

```text
mini-agent                       # 等价于 run（v4.9）
mini-agent ask [旗标] "问题"|-   # 一次性问答，stdout 只有回复
mini-agent run [旗标]            # 会话（TTY→TUI；非 TTY→行模式）
mini-agent session list          # 列出会话 id（新→旧）
mini-agent session rm ID|前缀    # 删除会话（含锁文件）
mini-agent mcp list [--mcp …]    # 生效 MCP server 清单（脱敏；无需 LLM 配好）
mini-agent --help | --version
```

`--help`/`--version` 在 curl 初始化之前处理：零网络、不读配置。版本号来自 CMake 工程版本（当前 0.1.0）。

### 2.2 `ask` — 一次性问答

| 旗标 | 值 | 默认 | 说明 |
| --- | --- | --- | --- |
| `--tools` | `off / ask / allowlist / auto` | `off` | 工具档位，语义见 [§2.3](#23---tools-四档语义) |
| `--cwd` | 目录路径 | 无（取 config/env workspace，否则进程 cwd） | 文件/git 工具的 workspace；必须是已存在目录 |
| `--mcp` | `NAME=SPEC` | 无 | 会话级注入 MCP server，可重复（[§2.7](#27---mcp-namespec-注入)） |
| `--debug` / `--debug=all` | — | — | `--log-level debug/trace` 的别名（[§3.2](#32-日志与抓包)） |
| `--log-level` | `info / debug / trace` | `info`（可被 env 改） | 流量抓包阶梯 |
| 位置参数 | `"问题"` 或 `-` | 必填，恰好 1 个 | `-` 从 stdin 读整个问题；空 stdin 报错退出 1 |

- 旗标支持空格与 `=` 两种写法（`--tools ask` ≡ `--tools=ask`），顺序任意，但必须在位置参数之前。
- **管道契约**：stdout 只输出回复增量 + 结尾换行；审批提示、`[tool]` 状态行、警告全部走 stderr。`--tools off`（默认）时请求体、输出与 v1 逐字节一致。
- 工具执行时 stderr 打 `[tool] 名字 ok|error`；stderr 是 TTY 时另附结果预览（v5：写/改文件的 diff、`preview_file` 正文）。
- 未配置（缺 `base_url/api_key/model` 任一）→ 退出 2，提示 env 或 TUI `/provider /login /model` 两条路。

### 2.3 `--tools` 四档语义

| 档位 | 行为 | TTY 要求 | 适用 |
| --- | --- | --- | --- |
| `off`（默认） | 不注册任何工具 | 无 | 纯问答脚本 |
| `ask` | 每次工具调用在 stderr 询问 `allow? [y/N]`（首字符 y/Y 放行；写/改类工具先展示 diff 预览） | stdin 与 stdout 都须是 TTY；**否则降级为 `off` 并在 stderr 提示**，绝不在管道里静默执行 | 人工交互 |
| `allowlist` | 只有 config `tool_allowlist` 里的工具名执行；**未命中一律拒绝**（以 error Tool 消息回填模型）；名单为空 = 全部拒绝 | 无，全程不阻塞 | 脚本安全档（v3） |
| `auto` | 全部工具**不经询问直接执行**（含 `run_bash`） | 无 | 慎用；无沙箱，等价把 shell 交给模型 |

注意 `allowlist` 在 CLI 与 TUI 语义不同：CLI 未命中是**拒绝**（脚本要确定性）；TUI `/approve allowlist` 未命中是**照问**（[§5.3](#53-审批与工具)）。

### 2.4 `run` — 会话（TUI / 行模式）

| 旗标 | 值 | 默认 | 说明 |
| --- | --- | --- | --- |
| `--session` | 会话 id 或唯一前缀 | 无 | 恢复指定会话（v4.13 支持前缀；歧义时列候选并退出 2）。**只支持空格写法**，`--session=ID` 不识别 |
| `--new` | — | — | 跳过默认复用，强制新会话 |
| `--temp` | — | — | 临时会话：隐含 `--new`，全程不落盘；与 `--session` 互斥（退出 2） |
| `--cwd` | 目录路径 | 见下 | workspace；恢复会话时显式 `--cwd` 会重绑并写回会话戳 |
| `--mcp` / `--debug` / `--log-level` | 同 `ask` | — | — |

- **默认复用（v4.15）**：不带 `--session/--new/--temp` 时，扫描最近 ≤12 个会话，取第一个 workspace 戳匹配且未被锁定的复用；没有匹配才新建。损坏/不可读的会话跳过。
- 恢复时 workspace 优先级：显式 `--cwd` > 会话内的 workspace 戳 > config/env > 进程 cwd。
- stdin 与 stdout 都是 TTY → TUI（[§5](#5-tui-斜杠命令与按键)）；否则**行模式**：每行一个问题，仅识别 `/exit`（其余 `/xxx` 原样发给模型），每轮落盘（temp 除外），EOF（Ctrl-D）退出并打印保存/丢弃信息。
- 未配置且非 TTY → 退出 2；未配置但 TTY → 进 TUI 走 `/provider /login /model` 配置流。
- 会话被其他进程锁定 → 退出 1（锁随进程退出释放，崩溃残留锁按 PID 存活性判定）。

### 2.5 `session` — 会话管理

| 命令 | 行为 | 失败 |
| --- | --- | --- |
| `session list` | stdout 逐行打印会话 id（新→旧） | 存储目录不可用 → 1 |
| `session rm ID|前缀` | 删除会话文件与锁文件（v4.14.1 支持唯一前缀） | 歧义 → 列候选退出 2；不存在 → 1；缺参数 → 2 |

会话文件本身也有 `meta.schema_version`（当前 1；回合含工具消息时写 2）：旧二进制读到更高版本会明确拒绝而非静默丢数据，与 config.json 的 `schema_version` 是两回事。

### 2.6 `mcp list` — MCP 清单自检

- **不需要 LLM 三件套配好**，是校验 `mcp_servers` 配置的最快命令；配置文件校验失败会打印具体条目错误并退出 2。
- 输出脱敏：http 行只显示 URL（userinfo 部分打码）与 `bearer=set`；stdio 行显示 command 与 args。
- 可加 `--mcp` 预览注入后的清单（不连接 server，惰性连接原则）。

### 2.7 `--mcp NAME=SPEC` 注入

适用于 `ask` / `run` / `mcp list`，可重复；**会话级 upsert**：同名覆盖 config.json 条目，不写回文件。NAME 限 `[A-Za-z0-9_-]{1,32}`。

| SPEC 形式 | 示例 | 说明 |
| --- | --- | --- |
| URL | `--mcp wiki=https://mcp.deepwiki.com/mcp` | Streamable HTTP；`https` 任意主机，`http` 仅 loopback |
| 命令 + 参数 | `--mcp 'fs=npx -y @modelcontextprotocol/server-filesystem /tmp'` | stdio；按空白拆参，**参数含空格时改用 JSON 形式** |
| JSON 对象 | `--mcp 'api={"transport":"http","url":"https://h/mcp","bearer_token":"t"}'` | 与 config.json `mcp_servers` 条目同 schema，全量字段 |

三种形式都经同一套校验（[config.cpp](../src/config.cpp) `parse_mcp_cli_spec`）；违规退出 2。详细使用场景见 [mcp-usage.md](mcp-usage.md)。

### 2.8 退出码

| 码 | 含义 | 例子 |
| --- | --- | --- |
| 0 | 成功 | 正常问答、`--help` |
| 1 | 运行时错误 | 网络/HTTP/协议错误、空 stdin、会话锁占用、`session rm` 目标不存在 |
| 2 | 配置/用法错误 | 未知命令或旗标、`--tools` 非法值、位置参数个数不对、未配置且非 TTY、`--mcp` spec 非法、会话 id 歧义 |

### 2.9 Ctrl-C（SIGINT）语义

- **TTY 场景（v5）**：第一次 Ctrl-C 协作式取消当前请求（历史停在上一个完整回合边界，锁正常释放）；第二次恢复默认处置直接终止。空闲时 Ctrl-C 丢弃半行输入、保留会话。
- **全管道场景**：不安装 handler，保持默认处置（脚本契约与 v1 一致）。
- TUI 内的中断链见 [§5.4](#54-按键)。

## 3. 环境变量

全部变量**非空才生效**（设成空串等于未设置）。覆盖发生在 config.json 加载之后。

### 3.1 全表

| 变量 | 值 | 默认 | 说明 |
| --- | --- | --- | --- |
| `MINI_AGENT_BASE_URL` | URL | 无 | OpenAI 兼容端点（如 `https://api.deepseek.com/v1`）；末尾单个 `/` 自动去除 |
| `MINI_AGENT_API_KEY` | 字符串 | 无 | LLM API 密钥（请求 Authorization 头）；红线见 [§1.4](#14-安全红线所有入口共同) |
| `MINI_AGENT_MODEL` | 字符串 | 无 | 模型 id；三者齐备才算配置完整 |
| `MINI_AGENT_CONFIG` | 文件路径 | `$XDG_CONFIG_HOME/mini-agent/config.json`（回落 `~/.config/…`） | 配置文件位置 |
| `MINI_AGENT_WORKSPACE` | 目录路径 | config `workspace`，否则进程 cwd | v3 文件/git 工具根目录（`--cwd` 可再覆盖） |
| `MINI_AGENT_TOOL_APPROVAL` | `ask / allowlist / off` | config `tool_approval`（默认 `ask`） | TUI 审批档位；非法值忽略、保留文件值。对 `ask` 子命令无效（那边由 `--tools` 决定） |
| `MINI_AGENT_NO_MCP` | 任意非空 | 未设置 | 清空全部 `mcp_servers`（排障开关） |
| `MINI_AGENT_CONTEXT_WINDOW` | 正整数 | config `context_window`（0=未知） | 状态栏 token 用量百分比的分母；非法值忽略 |
| `MINI_AGENT_REASONING_EFFORT` | 字符串 | config `reasoning_effort`（空=不发） | 原样透传请求体 `reasoning_effort` 字段，不做取值校验（服务端说了算） |
| `MINI_AGENT_LOG` | 路径 / `off` | `/tmp/mini-agent-<uid>.log` | 元数据日志（0600，追加不清空）；`off` 关闭 |
| `MINI_AGENT_LOG_LEVEL` | `info / debug / trace` | `info` | 抓包阶梯（[§3.2](#32-日志与抓包)）；非法值忽略；优先于 `MINI_AGENT_DEBUG` |
| `MINI_AGENT_DEBUG` | 任意非空 / `2` | 未设置 | 旧开关：非空=debug，`2`=trace；被 `MINI_AGENT_LOG_LEVEL` 覆盖 |
| `MINI_AGENT_SESSION_DIR` | 目录路径 | `$XDG_DATA_HOME/mini-agent/sessions`（回落 `~/.local/share/…`） | 会话存储根目录 |
| `MINI_AGENT_TIMEOUT_SECONDS` | 正整数 | 120 | 单次 LLM 请求总超时秒数（连接超时固定 5s）；非法值忽略 |

`.env.example`（仓库根）是同一份清单的可复制模板；mini-agent **不会自动读取 `.env` 文件**，需自行 `source` 或导出。

### 3.2 日志与抓包

| 级别 | 元数据日志 | 抓包文件 |
| --- | --- | --- |
| `info`（默认） | 事件行（时间戳 + kv：回合、工具名/状态/耗时/字节数） | 无 |
| `debug` | 同上 | `<日志路径>.debug`（0600）：chat 请求体与 SSE 原文 |
| `trace` | 同上 | 同上 + 辅助流量（models 列表、context 探测；单条截断 4KiB） |

- 抓包路径基于日志路径推导；`MINI_AGENT_LOG=off` 只关元数据日志，抓包仍写到默认 `/tmp/mini-agent-<uid>.log.debug`。
- `api_key` 只出现在 Authorization 头，不进抓包正文。
- CLI 入口：`--debug` / `--debug=all` / `--log-level L`；TUI 入口：`/log` `/debug`（仅会话级）。文件里写 `log_level` 会被忽略（v4.8.1 起会话级字段，不落盘）。

## 4. 配置文件 config.json

### 4.1 位置与文件规则

- 路径：`MINI_AGENT_CONFIG` > `$XDG_CONFIG_HOME/mini-agent/config.json` > `~/.config/mini-agent/config.json`。
- 目录 0700、文件 0600；保存走 `.tmp` + rename 原子替换。超过 64KiB 的文件整个忽略并警告。
- `schema_version` 目前最大 1，写更大的值整份配置拒绝。
- **fail-closed 与静默回落的分界**见下表"非法值"列：标 ❌ 的字段写错会让**整份文件**校验失败（`mcp list` 可看到具体错误）；标 ⚠ 的字段只是自身回落默认。
- `ask`/`run` 遇到文件校验失败只在 stderr 打 `warn:` 并继续用 env/默认值；`mcp list` 则直接退出 2（自检语义）。

### 4.2 字段总表

| 字段 | 类型 | 默认 | 非法值 | 说明 |
| --- | --- | --- | --- | --- |
| `schema_version` | int | 1 | ❌ >1 拒绝 | 配置格式版本 |
| `base_url` | string | 空 | ⚠ 类型错→空 | LLM 端点；env `MINI_AGENT_BASE_URL` 覆盖 |
| `api_key` | string | 空 | ⚠ 类型错→空 | 红线见 [§1.4](#14-安全红线所有入口共同) |
| `model` | string | 空 | ⚠ 类型错→空 | 模型 id |
| `system_prompt` | string \| null | 内置默认 | ❌ 其他类型拒绝 | `""`=禁用系统提示；null/缺省=内置默认；仅新会话注入（作为首条 system 消息随会话保存，恢复会话不重注入） |
| `workspace` | string | 空（=进程 cwd） | ⚠ 类型错→空 | v3 工具根目录 |
| `tool_approval` | string | `"ask"` | ❌ 非 `ask/allowlist/off` 拒绝 | TUI 审批档位 |
| `tool_allowlist` | string[] | `[]` | ❌ 非字符串数组拒绝 | `allowlist` 档的放行名单（工具名，含 `server__tool`） |
| `max_tool_rounds` | int>0 | 8 | ⚠ 回落 8 | 单回合工具循环上限；到顶后以无工具请求收尾 |
| `tool_timeout_seconds` | int>0 | 30 | ⚠ 回落 30 | 单次工具执行超时（`run_bash` 可被参数覆盖） |
| `context_window` | int>0 | 0（未知） | ❌ 存在但非正整数拒绝 | 状态栏用量百分比分母；0/缺省=只显示 token 数 |
| `reasoning_effort` | string | 空 | ⚠ 类型错→空 | 原样透传，不做取值校验 |
| `log_file` | string | 空（=默认 /tmp 路径） | ⚠ 类型错→空 | 元数据日志路径；`"off"` 关闭 |
| `log_level` | — | — | 忽略 | **会话级字段**：文件里写了也不生效、下次保存丢弃（同旧字段 `debug_log`/`debug_log_aux`） |
| `mcp_servers` | object | `{}` | ❌ 条目非法拒绝；⚠ 整体非 object 跳过 | server 名 → 条目，见 [§4.3](#43-mcp_servers-条目) |

### 4.3 `mcp_servers` 条目

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `transport` | `"stdio"` \| `"http"` | 可省略 | 省略时含 `url` → http，否则 stdio；**https 也写 `"http"`**（scheme 由 url 决定），写 `"https"` 整份配置拒绝 |
| `command` | string | stdio 必填 | 可执行文件（经 PATH 查找） |
| `args` | string[] | 否 | 启动参数 |
| `env` | object(str→str) | 否 | **只透传显式声明的键**，另固定给 `PATH`/`HOME` 基线 |
| `url` | string | http 必填 | 绝对 URL；`http://` 仅 loopback（127.\*、localhost、::1） |
| `bearer_token` | string | 否 | 静态 Bearer；与 api_key 同红线；不支持 OAuth |
| `allow_insecure_http` | bool | 否（false） | 显式放行非 loopback 明文 http（不建议） |

server 名限 `[A-Za-z0-9_-]{1,32}`，工具以 `server__tool` 双下划线前缀进入注册表。完整示例与排障见 [mcp-usage.md](mcp-usage.md)。

## 5. TUI 斜杠命令与按键

TTY 下 `run` 进入 TUI。输入 `/` 触发补全（Tab 循环候选，含参数档位提示）；`/help` 列出摘要。以下"持久化"列对应 [§1.3](#13-持久化与仅本次会话)。

### 5.1 连接与模型

| 命令 | 参数 | 持久化 | 说明 |
| --- | --- | --- | --- |
| `/provider` | `<url>` | ✅ | LLM 端点；不带参数显示当前值与用法 |
| `/login` | `[key]` | ✅ | 不带参数进入掩码输入（Enter 确认，空取消）；保存后重置鉴权探测、可能自动拉取模型列表 |
| `/model` | `<name>` | ✅ | 设置模型并写入会话元数据；重置用量读数 |
| `/models` | — | — | 强制重新拉取模型列表（需 base_url+key），数字选择器：Enter 选定、Esc 收起 |
| `/reasoning` | `[值|off]` | ✅ | 原样透传 `reasoning_effort`；不带参数打开缓存档位选择器（low/medium/high/off 为常见提示）；服务端 400 时提示、不自动回退 |
| `/context` | `[tokens|off|0|probe]` | ✅（probe 除外） | 设置/清除 `context_window`；不带参数显示当前值或目录建议值；`probe` 显式发一次超大请求探测上限（需配置齐全，≤30s，绝不自动触发） |

### 5.2 会话与工作区

| 命令 | 参数 | 持久化 | 说明 |
| --- | --- | --- | --- |
| `/sessions` | — | — | 列出会话，标注当前 |
| `/session` | `<id|前缀> \| new` | 会话切换 | 需回合空闲；唯一前缀可用（歧义列候选）；保存并解锁旧会话→加载并锁定新会话（失败回滚）；temp 会话经 `new` 保持 temp，恢复旧会话则结束 temp |
| `/cwd` | `[path]` | 会话戳 | 查询/重绑 workspace（须为已存在目录）；重绑会话戳、系统提示、工具注册表与侧栏 |
| `/preview` | `<path>` | — | workspace 内文件预览（`.md` 渲染；正文走 UI 通道，不进模型上下文，v5） |
| `/exit` | — | — | 保存并退出（temp 丢弃） |

### 5.3 审批与工具

| 命令 | 参数 | 持久化 | 说明 |
| --- | --- | --- | --- |
| `/approve` | `[ask|allowlist|off]` | ✅ | 查看/切换审批档位（需 worker 空闲）。`off` 回显警示（无沙箱）且状态行常显红色 `auto-tools`；**`allowlist` 未命中名单时仍然询问**（与 CLI 的拒绝语义不同） |
| `/tools` | — | — | 重建注册表并列出 名字·来源·描述 与当前审批策略；**改完 MCP 配置免重启的生效手段** |
| `/mcp` | — | — | 列出配置的 server（脱敏） |

**审批按键**（模型请求工具时，composer 上方出现审批块；写/改类先展示 diff 预览）：

| 键 | 行为 |
| --- | --- |
| `y` / `Y` | 放行本次 |
| `n` / `N` | 拒绝（以 error Tool 消息告知模型） |
| `a` / `A` | 本会话该工具名免问（不落盘） |
| `?` | 展开/收起完整参数 |
| Esc / Ctrl-C | 拒绝并中断本轮 |
| 其他键 | 扣留不动作 |
| （无操作 120s） | 自动按拒绝处理；90s 起显示倒计时 |

### 5.4 按键

| 键 | 场景 | 行为 |
| --- | --- | --- |
| Enter | 空闲 / 流式中 | 发送 / 排队下一问 |
| Ctrl-C | 流式中 | 第一次中断请求；再来一次退出 |
| Ctrl-C | 空闲 | 清空输入行；2 秒内再按退出 |
| Esc | 流式中 | 中断请求（无二次退出升级）；菜单打开时先收菜单 |
| Ctrl-D | 任意 | 退出 |
| Ctrl-L | 任意 | 清 live 区 |
| Ctrl-F | 侧栏开启时 | 切换侧栏焦点：↑/↓ 选文件、Enter 插入相对路径、`p` 预览、Esc 离开 |
| Tab | 输入 `/` 开头 | 循环补全命令与参数档位（`/session` 补全真实 id） |
| ↑ / ↓ | 普通输入 | 历史召回（侧栏焦点时为选择） |

### 5.5 显示与诊断

| 命令 | 参数 | 持久化 | 说明 |
| --- | --- | --- | --- |
| `/config` | — | — | 显示全部生效字段与配置文件路径（api_key 只显 `***set***`） |
| `/log` | （无参数） | — | 显示日志开关、路径与当前抓包级别 |
| `/log` | `<路径> \| off \| default` | ✅ | 元数据日志目标 |
| `/log` | `info \| debug \| trace` | 仅会话 | 抓包阶梯（[§3.2](#32-日志与抓包)） |
| `/debug` | `on \| all \| off` | 仅会话 | `/log debug/trace/info` 的别名 |
| `/sidebar` | `on \| off` | 仅会话 | 文件树 + git 变更侧栏 |
| `/sysinfo` | `on \| off` | 仅会话 | user@host · cwd · 时钟信息行 |
| `/help` | — | — | 命令摘要；提示符含义：`?` 未配置、`~` 探测中、`>` 就绪、`!` key 被拒、`x` 不可达 |

## 6. 内置工具参数速查

模型可调用的内置工具（MCP 工具 schema 由各 server 提供，`/tools` 可查）。所有文件/git 工具以 workspace 为边界（含符号链接目标校验）；输出统一 32KiB 截断。

### 6.1 命令执行（来源 `builtin`）

| 工具 | 参数 | 说明 |
| --- | --- | --- |
| `run_bash` | `command`(必填), `timeout`(1–3600 秒，默认 config `tool_timeout_seconds`) | `/bin/bash -lc`，workspace 为 cwd，stdout+stderr 合并；超时杀整个进程组 |

### 6.2 文件工具（来源 `builtin`，v3/v5）

| 工具 | 参数 | 说明 |
| --- | --- | --- |
| `read_file` | `path`(必填), `from_line`, `to_line` | 带行号读取 |
| `write_file` | `path`, `content`（均必填） | 父目录必须已存在；生成 diff 审批预览 |
| `edit_file` | `path`, `old_str`, `new_str`(必填), `replace_all`(默认 false) | 默认要求恰好 1 处匹配否则拒绝（fail-closed）；`replace_all` 要求 ≥1 处并全部替换；生成 diff 预览 |
| `list_dir` | `path`(默认 workspace 根) | 目录清单 |
| `search_text` | `pattern`(必填), `is_regex`, `path_prefix` | 输出 `path:line: text` |
| `preview_file` | `path`(必填) | v5：正文只进 UI 预览通道，不进模型上下文 |

### 6.3 git 工具（来源 `builtin-git`；首次使用时惰性探测，机器上没有 git 或 workspace 不是 git 仓库时整组不可用）

| 工具 | 参数 | 说明 |
| --- | --- | --- |
| `git_status` | — | 工作区状态 |
| `git_diff` | `staged`(bool), `path` | diff（可限路径） |
| `git_log` | `limit`(1–100，默认 20) | 提交历史 |
| `git_add` | `path`(必填) | 暂存 |
| `git_commit` | `message`(必填), `body` | **仅本地提交，永不 push**（无 push 工具，[ADR 008](decisions/008-git-tool-policy.md)；`run_bash` 不受此约束，审批时注意） |

## 7. 参考

### 7.1 文档

| 资料 | 内容 |
| --- | --- |
| [mcp-usage.md](mcp-usage.md) | v2 工具层与 MCP 使用指南（配置示例、排障速查、安全边界） |
| [设计 007：agent v1](design/007-agent-v1.md) | CLI/TUI/会话/配置的基础契约 |
| [设计 008：MCP 工具](design/008-mcp-tools.md) | 工具循环、`--mcp`、审批、mcp_servers 完整规则（§5.3） |
| [设计 009：coding v3](design/009-coding-v3.md) | workspace、文件/git 工具、allowlist 脚本档 |
| [设计 010：模型元数据](design/010-model-metadata.md) | context_window、reasoning_effort、/models、日志阶梯 |
| [设计 011：预览与 TUI 集成](design/011-preview-tui-integration.md) | preview_file、diff 预览、Ctrl-C 协作取消 |
| [ADR 005](decisions/005-mcp-transport-abstraction.md) / [ADR 006](decisions/006-tool-approval-policy.md) / [ADR 007](decisions/007-file-tools-workspace.md) / [ADR 008](decisions/008-git-tool-policy.md) | 传输抽象、审批策略、workspace 边界、git 工具边界 |
| [../.env.example](../.env.example) | 环境变量可复制模板 |
| [development.md](development.md) | 构建依赖与质量门禁 |

### 7.2 源码定位（本文事实依据）

| 参数面 | 位置 |
| --- | --- |
| CLI 解析、退出码、SIGINT、行模式 | [src/cli_main.cpp](../src/cli_main.cpp) |
| env 合并、config 校验、`--mcp` spec、超时 | [src/config.cpp](../src/config.cpp)、[include/mini_agent/config.hpp](../include/mini_agent/config.hpp) |
| 斜杠命令、审批 UI、按键、120s 超时 | [src/tui.cpp](../src/tui.cpp)、[include/mini_agent/tui_bridge.hpp](../include/mini_agent/tui_bridge.hpp) |
| 内置工具 schema 与执行 | [src/tools.cpp](../src/tools.cpp)、[include/mini_agent/tools.hpp](../include/mini_agent/tools.hpp) |
| 工具注册表组装（workspace 回落 cwd、git 探测） | [src/mcp_rpc.cpp](../src/mcp_rpc.cpp) |
| 会话目录/删除 | [src/session.cpp](../src/session.cpp) |
| 日志路径解析 | [src/log.cpp](../src/log.cpp) |

### 7.3 维护约定

- 参数面变更（新增/改名/改默认值/改语义）时同步更新本文与 `.env.example`；`--help` 文本在 [src/cli_main.cpp](../src/cli_main.cpp) `print_usage`。
- 本文只记录已实现行为；计划中的参数写在对应设计文档，不提前进本文。