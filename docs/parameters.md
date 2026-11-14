# mini-agent 参数使用大纲

> **文档性质**：使用者参数参考（大纲式索引）。所有条目按当前源码核对（v0.1.0，v1–v6 命令面）；日常快速查看以 `mini-agent --help` 为准，本文补充语义、默认值、持久化范围与出处。
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
- 少数键**只有 env 入口**（`MINI_AGENT_CONFIG`、`MINI_AGENT_SESSION_DIR`、`MINI_AGENT_TIMEOUT_SECONDS`、`MINI_AGENT_RETRIES`），没有对应的文件字段或 CLI 旗标。
- **v7 命名档案（[§4.4](#44-多-provider-档案providers--active_provider)）**：LLM 配置只存于命名档案表（`providers`），`active_provider` 指定生效档案；运行时生效值 = 活动档案 + env 叠加（单向派生，无顶层字段）。`MINI_AGENT_MODEL=provider/model` 限定名与 CLI `--provider NAME` / `--model NAME`（[§2.2](#22-ask--一次性问答)）选档案，CLI `--model` 按上述链条最后生效且只改本次进程；三个既有 LLM env 的非空覆盖**恒赢**——env 覆盖生效期间 TUI 内切档案/改 key 只写盘、本进程生效值不变（`/config` 显示 `env override in effect`，选择时另有 `[warn]`；unset 后重启恢复）。

### 1.3 "持久化"与"仅本次会话"

| 范围 | 含义 | 例子 |
| --- | --- | --- |
| 持久化 | 写入 config.json（0600，原子替换），下次启动仍生效 | `/provider`（含 `add/rm/list`） `/login` `/model` `/approve` `/context` `/reasoning`、`/log <路径/off/default>` |
| 仅本次会话 | 只改当前进程，退出即失，不写文件 | `/log info\|debug\|trace`、`/debug`、`/sidebar` `/sysinfo`、审批按键 `a`、`--mcp`、全部 CLI 旗标与 env |

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
| `--provider` | 档案名 | 无（config `active_provider`） | v6：选择命名档案（[§4.4](#44-多-provider-档案providers--active_provider)）；未知名字退出 2 并列可用档案；仅本次进程，不写文件 |
| `--model` | 模型 id 或 `provider/model` | 无 | v6：模型覆盖，后于 `--provider` 生效；限定名前缀命中已配置档案时先选该档案；未知前缀按字面 id（模型名本身可含 `/`）；v7 起为会话级叠加，只改本次进程生效值、从不写档案 |
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
| `--provider` / `--model` | 同 `ask` | — | v6 档案/模型覆盖，仅本次进程（[§2.2](#22-ask--一次性问答)） |
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
| `session rm ID\|前缀` | 删除会话文件与锁文件（v4.14.1 支持唯一前缀） | 歧义 → 列候选退出 2；不存在 → 1；缺参数 → 2 |

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
| `MINI_AGENT_MODEL` | 字符串 | 无 | 模型 id；三者齐备才算配置完整。v6：`provider/model` 限定名且前缀命中已配置档案时生效值从该档案派生（URL/key 取档案值，仍可被另两个 env 覆盖；不改盘上 `active_provider`），模型取 `/` 后部分 |
| `MINI_AGENT_CONFIG` | 文件路径 | `$XDG_CONFIG_HOME/mini-agent/config.json`（回落 `~/.config/…`） | 配置文件位置 |
| `MINI_AGENT_WORKSPACE` | 目录路径 | config `workspace`，否则进程 cwd | v3 文件/git 工具根目录（`--cwd` 可再覆盖） |
| `MINI_AGENT_TOOL_APPROVAL` | `ask / allowlist / off` | config `tool_approval`（默认 `ask`） | TUI 审批档位；非法值忽略、保留文件值。对 `ask` 子命令无效（那边由 `--tools` 决定） |
| `MINI_AGENT_NO_MCP` | 任意非空 | 未设置 | 清空全部 `mcp_servers`（排障开关） |
| `MINI_AGENT_CONTEXT_WINDOW` | 正整数 | 活动档案 `context_window`（0=未知） | 状态栏 token 用量百分比的分母；非法值忽略 |
| `MINI_AGENT_REASONING_EFFORT` | 字符串 | 活动档案 `reasoning_effort`（空=不发） | 原样透传请求体 `reasoning_effort` 字段，不做取值校验（服务端说了算） |
| `MINI_AGENT_LOG` | 路径 / `off` | `/tmp/mini-agent-<uid>.log` | 元数据日志（0600，追加不清空）；`off` 关闭 |
| `MINI_AGENT_LOG_LEVEL` | `info / debug / trace` | `info` | 抓包阶梯（[§3.2](#32-日志与抓包)）；非法值忽略；优先于 `MINI_AGENT_DEBUG` |
| `MINI_AGENT_DEBUG` | 任意非空 / `2` | 未设置 | 旧开关：非空=debug，`2`=trace；被 `MINI_AGENT_LOG_LEVEL` 覆盖 |
| `MINI_AGENT_SESSION_DIR` | 目录路径 | `$XDG_DATA_HOME/mini-agent/sessions`（回落 `~/.local/share/…`） | 会话存储根目录 |
| `MINI_AGENT_TIMEOUT_SECONDS` | 正整数 | 120 | 单次 LLM 请求总超时秒数（连接超时固定 5s）；非法值忽略 |
| `MINI_AGENT_RETRIES` | `0–5` | 2 | 流前重试次数（[设计 014](design/014-provider-retry.md)）：仅对尚未交付任何 SSE 内容的瞬时失败（网络错误/HTTP 408/429/500/502/503/504）生效，指数退避 1s/2s/…，尊重 `Retry-After`（>30s 放弃）；`0` 关闭；越界/非法值忽略 |

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
- `schema_version` 目前最大 2，写更大的值整份配置拒绝。v1 文件（含无版本号的旧文件）加载时**一次性迁移**为 v2（见 [§4.4](#44-多-provider-档案providers--active_provider)），下次保存落 v2；旧二进制读 v2 文件会被版本门禁拒绝。
- **fail-closed 与静默回落的分界**见下表"非法值"列：标 ❌ 的字段写错会让**整份文件**校验失败（`mcp list` 可看到具体错误）；标 ⚠ 的字段只是自身回落默认。
- `ask`/`run` 遇到文件校验失败只在 stderr 打 `warn:` 并继续用 env/默认值；`mcp list` 则直接退出 2（自检语义）。

### 4.2 字段总表

| 字段 | 类型 | 默认 | 非法值 | 说明 |
| --- | --- | --- | --- | --- |
| `schema_version` | int | 2 | ❌ >2 拒绝 | 配置格式版本；v1 文件加载时一次性迁移 |
| `system_prompt` | string \| null | 内置默认 | ❌ 其他类型拒绝 | `""`=禁用系统提示；null/缺省=内置默认；仅新会话注入（作为首条 system 消息随会话保存，恢复会话不重注入） |
| `workspace` | string | 空（=进程 cwd） | ⚠ 类型错→空 | v3 工具根目录 |
| `tool_approval` | string | `"ask"` | ❌ 非 `ask/allowlist/off` 拒绝 | TUI 审批档位 |
| `tool_allowlist` | string[] | `[]` | ❌ 非字符串数组拒绝 | `allowlist` 档的放行名单（工具名，含 `server__tool`） |
| `max_tool_rounds` | int>0 | 8 | ⚠ 回落 8 | 单回合工具循环上限；到顶后以无工具请求收尾 |
| `tool_timeout_seconds` | int>0 | 30 | ⚠ 回落 30 | 单次工具执行超时（`run_bash` 可被参数覆盖） |
| `log_file` | string | 空（=默认 /tmp 路径） | ⚠ 类型错→空 | 元数据日志路径；`"off"` 关闭 |
| `log_level` | — | — | 忽略 | **会话级字段**：文件里写了也不生效、下次保存丢弃（同旧字段 `debug_log`/`debug_log_aux`） |
| `mcp_servers` | object | `{}` | ❌ 条目非法拒绝；⚠ 整体非 object 跳过 | server 名 → 条目，见 [§4.3](#43-mcp_servers-条目) |
| `providers` | object | `{}`（空=未配置/env-only） | ❌ 非 object 或任一档案非法拒绝 | v7 唯一的 LLM 持久化面：档案名 → 档案条目，见 [§4.4](#44-多-provider-档案providers--active_provider) |
| `active_provider` | string | 空 | ❌ 档案表非空时必填且必须命中（fail-closed） | v7 活动档案名；生效值由它派生，不再有顶层展开字段 |

v7（[设计 013](design/013-named-providers.md)）起**顶层 `base_url`/`api_key`/`model`/`context_window`/`reasoning_effort` 五字段废弃**：LLM 配置只存在于档案条目内（[§4.4](#44-多-provider-档案providers--active_provider)）。v2 文件里出现这些键按未知字段忽略；v1 文件的顶层值迁移进 `default` 档案。

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

### 4.4 多 provider 档案（`providers` / `active_provider`）

v6（[设计 012](design/012-multi-provider.md)）引入命名档案与聚合菜单；v7（[设计 013](design/013-named-providers.md)、[ADR 009](decisions/009-named-providers-schema-v2.md)）把档案表变成**唯一**的 LLM 持久化面：provider 必须命名，顶层五字段废弃，未命名态不再存在。

- **档案条目字段**（全部可选，非空才校验）：`base_url`（http/https 绝对 URL，末尾 `/` 自动去除；无 MCP 那条 loopback 限制）、`api_key`（红线同 [§1.4](#14-安全红线所有入口共同)）、`model`（该档案最近选中的模型）、`context_window`（❌ 存在但非正整数拒绝；状态栏用量百分比分母）、`reasoning_effort`（原样透传，不做取值校验）。
- **档案名**限 `[A-Za-z0-9_-]{1,32}`（菜单/日志展示用）。`/provider add <name>` 允许先建空档案再补 URL/key；**首个档案自动激活**。
- **生效值派生（单向）**：运行时 `current` = 活动档案五字段 + 非空 env 叠加（`MINI_AGENT_MODEL=provider/model` 限定名先选档案）。TUI 编辑命令（`/provider <url>`、`/login`、`/model`、`/context`、`/reasoning`）直接改活动档案再重派生；CLI `--provider/--model` 只改本次进程的 `current`，从不写档案。切换档案整体覆写窗口/effort（不合并，上一个模型的元数据不残留）。
- **自动 default**：无任何档案时，上述 TUI 持久化命令自动创建并激活 `default` 档案（首启零摩擦）；env-only（CI 脚本）状态不落盘。
- **env 恒赢**：三个 LLM env 任一非空即进入会话级 override 态——切档案/改 key 只写盘，本进程生效值不变（`/config` 显示 `env override in effect`，选择时另有 `[warn]`）；unset 后重启恢复。
- **一次性迁移**：v1 文件加载时，未命名顶层值折成 `default` 档案（名冲突用 `default-2`…）；已有活动档案的顶层展开值直接丢弃。迁移只在内存发生，**下次保存才落 v2**（此前旧二进制仍可读原文件）。日志记一条 `config_migrate` 元数据事件。
- **写盘形态**：只有存在档案时才写 `providers`/`active_provider` 两键；`schema_version=2`。**旧二进制读 v2 文件被版本门禁拒绝**（破坏性变更，有意）；升级前建议备份 config.json，回滚即恢复备份。
- 多开进程间配置不同步（last-writer-wins），档案并发编辑可能互相覆盖；档案数无硬上限，聚合拉取顺序执行、耗时随档案数线性增长（建议 ≤5 个）。

示例（key 为占位符）：

```json
{
  "schema_version": 2,
  "providers": {
    "deepseek": { "base_url": "https://api.deepseek.com/v1", "api_key": "<deepseek-key>", "model": "deepseek-chat" },
    "moonshot": { "base_url": "https://api.moonshot.cn/v1", "api_key": "<moonshot-key>", "model": "kimi-k2-0905-preview", "context_window": 262144 }
  },
  "active_provider": "deepseek"
}
```

## 5. TUI 斜杠命令与按键

TTY 下 `run` 进入 TUI。输入 `/` 触发补全（Tab 循环候选，含参数档位提示）；`/help` 列出摘要。以下"持久化"列对应 [§1.3](#13-持久化与仅本次会话)。

### 5.1 连接与模型

v6 起支持多 provider 档案，v7 起档案必须命名（[§4.4](#44-多-provider-档案providers--active_provider)）：`/provider add` 建档 → `/login <name>` 登录 → `/models` 聚合菜单 → 序号或 `/model <provider>/<model>` 一步切换端点+key+模型。无档案时持久化命令自动创建 `default` 档案，单 provider 用法体验不变。

| 命令 | 参数 | 持久化 | 说明 |
| --- | --- | --- | --- |
| `/provider` | （无参数）\| `list` | — | 列出全部档案（key 只显 `***set***`，活动档案标 `<- active`）与用法；生效值看 `/config` |
| `/provider` | `<url>` | ✅ | 设置活动档案的 LLM 端点（无档案时自动建 `default`）；重置鉴权探测 |
| `/provider` | `add <name> [url]` | ✅ | 新建/更新档案（url 覆盖、已存 key/model 保留），提示下一步 `/login <name>`；名字限 `[A-Za-z0-9_-]{1,32}`；首个档案自动激活 |
| `/provider` | `rm <name>` | ✅ | 删除档案（其 URL/key 一并删除）；删除的是活动档案时自动切换到剩余首个档案，无剩余则回到未配置态（`[info]` 提示） |
| `/login` | `[key]` | ✅ | 给活动档案设置 key（无档案时自动建 `default`）；不带参数进入掩码输入（Enter 确认，空取消）；保存后重置鉴权探测、可能自动拉取模型列表 |
| `/login` | `<name> [key]` | ✅ | 首 token 命中档案名时登录该档案（掩码输入标注档案名）；key 只进该档案，不改当前连接，除非它正是生效档案 |
| `/model` | `<name>` | ✅ | 按字面模型 id 设置到活动档案；若最近一次 `/models` 结果中该 id 恰属一个档案则自动切换过去，属多个档案则报错并列出候选（fail-closed，不猜） |
| `/model` | `<provider>/<model>` | ✅ | 限定名且档案存在：原子切换 URL+key+model，写会话元数据 `meta.provider`；前缀未知则按字面 id（模型名可含 `/`）。切换均重置用量读数；env 覆盖生效时只写盘并 `[warn]` 本进程不生效 |
| `/models` | — | — | 聚合拉取所有配置齐全档案的 `GET /models`（env-only 无档案时拉当前生效值；env 覆盖态额外拉生效路由）：顺序执行、每档案连接 5s/总 15s 上限；多来源时菜单项带 `provider/` 前缀，`<- current` 标当前。数字选择器：Enter 选定（同时切档案）、Esc 收起；各档案鉴权探测独立更新提示符 |
| `/reasoning` | `[值\|off]` | ✅ | 原样透传 `reasoning_effort`；不带参数打开缓存档位选择器（low/medium/high/off 为常见提示）；服务端 400 时提示、不自动回退 |
| `/context` | `[tokens\|off\|0\|probe]` | ✅（probe 除外） | 设置/清除 `context_window`；不带参数显示当前值或目录建议值；`probe` 显式发一次超大请求探测上限（需配置齐全，≤30s，绝不自动触发） |

Tab 补全：`/login ` 补档案名，`/model ` 补聚合菜单里的（限定）模型 id，`/provider ` 补 `add/rm/list`。`/config` 会显示 `providers=N active=…`，env 覆盖生效时附 `env override in effect`。

### 5.2 会话与工作区

| 命令 | 参数 | 持久化 | 说明 |
| --- | --- | --- | --- |
| `/sessions` | — | — | 列出会话，标注当前 |
| `/session` | `<id\|前缀> \| new` | 会话切换 | 需回合空闲；唯一前缀可用（歧义列候选）；保存并解锁旧会话→加载并锁定新会话（失败回滚）；temp 会话经 `new` 保持 temp，恢复旧会话则结束 temp |
| `/cwd` | `[path]` | 会话戳 | 查询/重绑 workspace（须为已存在目录）；重绑会话戳、系统提示、工具注册表与侧栏 |
| `/preview` | `<path>` | — | workspace 内文件预览（`.md` 渲染；正文走 UI 通道，不进模型上下文，v5） |
| `/exit` | — | — | 保存并退出（temp 丢弃） |

### 5.3 审批与工具

| 命令 | 参数 | 持久化 | 说明 |
| --- | --- | --- | --- |
| `/approve` | `[ask\|allowlist\|off]` | ✅ | 不带参数：显示当前档位（allowlist 档另列名单）；带参数：切换并写盘，回合执行中会先等本回合结束再生效。三档语义见表下说明 |
| `/tools` | — | — | 重建注册表并列出 名字·来源·描述 与当前审批策略；**改完 MCP 配置免重启的生效手段** |
| `/mcp` | — | — | 列出配置的 server（脱敏） |

`/approve` 三档在 TUI 中的语义（默认 `ask`；初始值取 config/env，切换即持久化）：

| 档位 | 工具调用时 | 备注 |
| --- | --- | --- |
| `ask` | 每次弹审批块询问 | 默认档 |
| `allowlist` | 命中 config `tool_allowlist` 名单（含 `server__tool`）→ **免问直接执行**；未命中 → **照问** | 与 CLI `--tools allowlist` 的"未命中一律拒绝"不同（[§2.3](#23---tools-四档语义)）；名单为空 = 全部照问（切换时会提示） |
| `off` | 全部免问直接执行 | 无沙箱：切换时回显警示，状态行常显红色 `auto-tools` |

任一档位下，按过 `a` 的工具名本会话内免问（仅内存，见下方按键表）。档位每回合快照生效：回合执行中切换会先等该回合结束（同 `/cwd`、`/tools` 的 join 纪律），不会中途改变正在等待的审批。

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
| [设计 012：多 provider](design/012-multi-provider.md) | providers/active_provider、`/provider add/rm/list`、`/login <name>`、聚合 `/models`、限定名切换（v6） |
| [设计 013：命名档案与配置统一](design/013-named-providers.md) | schema v2、顶层 LLM 字段废弃、current 单向派生、自动 default、v1 迁移（v7） |
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