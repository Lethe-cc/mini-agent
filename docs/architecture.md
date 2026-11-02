# 系统架构

## 当前状态

mini-agent 已完成第一版实现（2026-11-02）：Agent 内核、OpenAI 兼容 Provider、session 持久化、CLI（`ask`/`run`/`session`）、自绘 TUI、诊断日志与 Markdown 渲染全部交付并通过验证。业务功能设计见 [007：mini-agent 第一版技术方案](design/007-agent-v1.md)，关键取舍见 [决策索引](decisions/README.md)（ADR 001–004，均已接受）。

模板工具层（`scripts/`、`profiles/`、`workflows/`、`templates/`）仍在仓库中，负责检查、初始化和文档规则；它与业务代码相互独立，业务验收以 `--strict-project` 为准。

## 系统边界

- 输入：用户终端（TTY）或 stdin 管道；配置来自环境变量与 `~/.config/mini-agent/config.json`（env 优先）。
- 输出：stdout（回复正文/历史区渲染）、诊断日志（key=value 元数据，不含密钥与消息正文）。
- 外部依赖：仅 OpenAI 兼容 Chat Completions 服务（HTTPS + SSE）；无数据库、无守护进程、无网络监听面。
- 部署形态：单个原生二进制 `mini-agent`，`agent_core` 静态链接（ADR 001：一进程一会话，多开 = 多进程，文件锁防同 id 并发打开）。

## 模块与依赖方向

```plantuml
@startuml mini-agent-architecture
skinparam componentStyle rectangle
title mini-agent 模块架构（2026-11-02，均已实现）

package "可执行入口" {
  [cli_main.cpp\n子命令解析 / 退出码] as CLI
  [tui.cpp\n自绘 TUI：termios + ANSI\n帧模型 / 差分渲染 / 行编辑] as TUI
}

package "agent_core (静态库)" {
  [loop.cpp\n会话循环 run_turn()] as LOOP
  [chat.cpp\nMessage / Chat 模型] as CHAT
  [session.cpp\n加载 / 原子写 / 文件锁] as SESSION
  [provider_openai.cpp\nSSE 流式 + 中断轮询\nlist_models: GET /models] as PROVIDER
  [config.cpp\nenv + 配置文件合并] as CONFIG
  [md.cpp\n宽度 / wrap / clip + Markdown] as MD
  [log.cpp\nspdlog 包装（脱敏）] as LOG
}

package "vendored 第三方 (许可证见目录内记录)" {
  [nlohmann/json\n单头文件] as JSON
  [md4c v0.6.0\nC 解析器] as MD4C
  [spdlog\nheader-only] as SPDLOG
}

node "外部依赖" {
  [libcurl\n(系统包)] as CURL
}

cloud "OpenAI 兼容服务" as LLM
database "本地文件" as FS

CLI --> LOOP
CLI --> SESSION
CLI --> CONFIG
CLI --> TUI
TUI --> LOOP
TUI --> PROVIDER
TUI --> SESSION
TUI --> MD
TUI --> CONFIG
LOOP --> CHAT
LOOP --> PROVIDER
PROVIDER --> CURL : HTTPS + SSE
PROVIDER ..> JSON
CHAT ..> JSON
SESSION ..> JSON
SESSION --> FS : sessions/<id>.json\n(0600, 原子 tmp+rename)
CONFIG --> FS : config.json (0600)
MD ..> MD4C
LOG ..> SPDLOG
PROVIDER --> LLM

note bottom of TUI : 渲染核心与网络分离：\nworker 线程只 append delta，\n主循环 poll ≤50ms 抽干后按帧重绘
@enduml
```

依赖方向约束：`mini-agent → agent_core → {curl, nlohmann, md4c, spdlog}`；core 内不含 CLI 参数解析，TUI 不含 HTTP 细节；`loop::run_turn(Chat&, Provider&)` 为纯函数式入口，配置参数注入、无全局单例（多 Agent 扩展点）。

## TUI 帧模型（渲染与刷新）

自绘 TUI（ADR 003 + 007 §5.6）采用"历史区一次打印进终端 scrollback + 底部 live 块原地重绘"的内联模型，行记账依赖硬换行不变量（所有写入行由 `md::wrap` 按显示宽度 ≤cols 截断，`\x1b[nA` 回退计数因此精确）。

```plantuml
@startuml mini-agent-tui-frame
title TUI 帧循环与刷新机制
start
while (未退出?) is (下一帧)
:poll(stdin, ≤50ms tick);
if (有输入?) then (是)
  :解析键位\n(Ctrl-C/Ctrl-D/Ctrl-L/Enter/Tab/\nUTF-8 编辑/方向键/单独 Esc 中断);
  :更新 composer/cursor/history 状态;
else (否)
endif
if (回合完成?) then (是)
  :join worker;
  :clear_live + commit 历史行\n(You/AI 前缀, markdown 定格);
  :原子写 session;
  if (队列有排队输入?) then (是)
    :start_turn(下一条);
  endif
else (否)
  :drain DeltaBuffer 快照;
  :**输入缓存**：入参未变 → 零工作量返回;
  :**行级 diff**：与前帧逐行比较，跳过未变前缀;
  :**CSI 2026 同步输出**包裹整帧原子提交;
  :重绘 live 块\n(You: 钉顶 + 响应窗口(随终端高度)\n+ 输入行(反显光标) + Tab 菜单 + 状态行);
endif
endwhile (退出)
stop
@enduml
```

关键机制（借鉴 Pi 等开源实现，只取模式、MIT 许可）：

- **事件与渲染分离**：`Provider::complete` 的 on_delta 只追加互斥缓冲；终端写只发生在主循环帧内。
- **刷新节流三档**：输入缓存短路（空闲 tick 零开销）→ 帧内容比较（跳过无变化重绘）→ 行级 diff（只重写变化区间）。
- **流式窗口随终端高度增长**；行预算优先级：状态行 > 输入行 > 钉顶问题 > 补全菜单 > 响应窗口。
- **中断语义**：Ctrl-C / 单独 Esc 中断当前请求（历史保留），Ctrl-C 二次退出，Ctrl-D 退出。

## 数据流与核心模型

- `Chat`：`system` + `user|assistant|toolResult` 消息序列；JSON 序列化含 `schema_version`（当前 1）；Role 预留 `Tool`、Message 预留恒空 `tool_calls`（工具调用扩展点）。
- session 文件：`<sessions>/<id>.json`，元信息（id/title/model/时间）+ 消息历史；写路径 `tmp + rename` 原子替换，`.lock` 文件防同 id 并发打开（ADR 001）。
- 配置合并：env > `config.json`；`/login`/`/model`/`/provider` 在 TUI 内写回文件（0600）。密钥在任何 stdout/日志面零输出（掩码显示 `***set***`）。三条命令**各自独立保存**，可任意顺序、分属不同进程分批录入；提示行统一走 `config_next_hint()`，只报 `config N/3 · next: <下一步>`而不重列全部命令，也不回显字段值（见 007 §5.6 规则 8）。日志侧 `Logger::redact_url()` 会将 `https://user:token@host` 的 userinfo 剥为 `***@`，防止将凭据内嵌到 base_url 时泄入日志。
- 模型发现（2026-11-02）：`/provider <url>` 与 `/login <key>` 保存后，若 base_url+api_key 齐备则自动 `GET {base_url}/models`（独立 worker、15 s 上限），`/models` 可手动重列。返回 id 排序去重后以编号菜单呈现，输序号即选定（也可继续用 `/model <name>`，Esc dismiss）。日志记 `models_list status=start|ok|error count= elapsed_ms=` 与 `model_select model= source=menu`，仅元数据；密钥不入菜单也不入错误文本。
- 登录状态（2026-11-02）：上述同一次 `GET /models` 兼作认证探针（**零额外请求**），`ProviderError::http_status()` 用于区分“密钥被拒”（401/403 → rejected）与“未连上”（unreachable）；`auth_status_line` / `auth_log_token` 分类 verified / rejected / refused / unreachable / unverified，展示于历史区与 `/config` 的 `login=` 行。凭据一变即重置结论；启动不探测（不击穿冷启动预算）；日志 `login status=… models=N [http_status=N]`（见 007 §5.6 规则 9）。**聊天请求也作为认证探针**（2026-11-02 修复）：主线程在回合结束把 `chat/completions` 的结果折叠进同一个 `AuthProbe`（成功=verified，401/403=rejected），聊天来源的证据在日志中以 `source=chat` 字段区分（`AuthProbe` 本身不带来源字段），因此不跑 `/models`、或 provider 不开放 `/models` 时也能正确显示连通；verdict 变化只记一条 `login … source=chat` 日志，不再往历史区打 `[auth]` 行（提示符即反馈），abort 不改变结论。
- provider 连通状态标志（2026-11-02）：输入提示符本身即状态标志——`connection_state()` 从配置 + 上一次 `AuthProbe` 推导出 `ConnState`，`prompt_marker()` 将其渲染为带样式的 1 列前缀（`?` 暗色未配置 / `?` 黄未验证 / `~` 青探测中 / `>` 绿已验证 / `!` 红被拒 / `x` 品红不可达）。零额外请求与零额外行，marker 不含文本因此不可能泄露 endpoint 或密钥；图形与颜色双重编码，`/help` 提供图例（见 007 §5.6 规则 10）。

## 错误处理与安全边界

- 失败轮回滚：`ProviderError`（网络/HTTP/协议）只渲染进历史区，Chat 与 session 文件保持上一轮状态；中断轮同样不落盘。
- 退出码：`ask` 0 成功 / 1 运行时错误 / 2 配置错误；`--help`/`--version` 不触网、不读密钥。
- 无内置权限系统：进程以其用户权限运行文件/网络/凭据访问；需要边界时自行容器化或最小权限运行。
- 供应链：vendored 第三方（nlohmann/md4c/spdlog）附许可证与来源记录于 `third_party/`；新增依赖前核实许可证并记 ADR。

## 未实现与扩展点

| 能力 | v1 状态 | 预留 |
| --- | --- | --- |
| 工具调用 (function calling) | 不实现 | Role/字段预留，Provider 流式协议可扩展 |
| 上下文压缩 | 不实现 | 历史在内存重放，可插 summarize |
| 多 Agent / 编排 | 不实现 | `run_turn(Chat&, Provider&)` 参数注入、无全局单例 |
| 跨折行光标移动、鼠标、语法高亮 | 不实现（v2） | 帧模型行预算与 diff 结构已就位 |
| 真滚动窗口（全文档 diff） | 不实现（v2） | 需整屏重放策略，见 007 §5.6 已知局限 |

## 验证入口

- 基础检查：`python3 scripts/check.py --require-initialized`
- 严格项目门禁（configure → build → CTest）：`python3 scripts/check.py --strict-project`
- 手工冒烟（pty 驱动 TUI + mock SSE）：`python3 tests/pty_tui_smoke.py ./build/linux-gcc-debug/mini-agent`
- 单元契约：`build/<preset>/agent_test`（chat/session/loop/config/log/md/tui 纯函数）
