# mini-agent 技术方案 019：代码模块拆分与抽象（草案）

## 文档状态

- 状态：**已实现 / 待人工评审**（2026-11-16 起草；2026-11-21 P4 收官，P1–P4 全部落地——P1 `04b36f1`、P2 `a879bcd`、P4 `c1a57f3`，实施记录见 §5/§8）。行为保持拆分；TuiState 完整状态对象未建，以 TurnState 首片 + Ctx 引用替代（§8）。
- 动因：维护者 2026-11-16 提出"每个代码文件都很长，考虑拆成较小/较抽象的模块"。本文给出可行的、行为保持的分阶段拆分方案。
- 核对基线：main `b419ba8`（018 合入后）。行数实测见 §1。
- 约束：不新增第三方依赖；保持依赖方向 `mini-agent → agent_core → {curl,json,md4c,spdlog}`（core 不含 CLI/TUI，TUI 不含 HTTP）；保持 `run_turn(Chat&,Provider&)` 纯函数式入口；每阶段以现有测试（unit + pty 双环境 + 四 preset）为门禁。

## 1. 现状与问题

| 文件 | 行数 | 主要问题 |
| --- | --- | --- |
| src/tui/tui.cpp | 3622 | `run_loop` 单函数 ~3100 行，~40 个 lambda 闭包共享局部状态；`handle_slash` ~940 行；键位/帧渲染/侧栏/预览/回合全在一处 |
| src/tools/tools.cpp | 1383 | ToolRegistry + run_subprocess + run_bash + FileRuntime + GitRuntime + workspace 边界混居 |
| src/provider/provider_openai.cpp | 1251 | 纯解析（SSE/tool_calls/models/probe/估算）与 curl 传输混居 |
| src/cli/cli_main.cpp | 1115 | 四个子命令 + 公共助手 + main 同文件 |
| src/tui/tui_render.cpp | 974 | 已拆出的纯渲染（良好，保持） |
| src/render/md.cpp | 802 | 几何（width/wrap/clip）与 md4c 渲染混居 |
| src/core/config.cpp | 777 | 文件加载/迁移/校验 与 env 合并/mcp spec 混居 |
| src/core/session.cpp | 612 | IO/锁 与 JSON/Markdown 序列化混居 |

**根因**：TUI 的"闭包共享状态"模式使逻辑无法跨翻译单元移动；其余文件是"多职责同文件"，可按类/纯函数机械拆分。

## 2. 抽象原则

1. **先纯后状态**：先抽无状态纯函数（易单测、零风险），再抽类，最后动 TUI 状态。
2. **TUI 引入显式状态对象** `TuiState`：把 `run_loop` 的共享局部状态收进一个 struct，lambda 改为接收 `TuiState&` 的自由函数/成员，才能跨文件拆分。这是唯一需要的"新抽象"。
3. **按职责建翻译单元**，头文件分"公共契约"（include/mini_agent/*.hpp 保持）与"内部"（src/*_internal.hpp）。
4. **行为保持**：每阶段独立提交，测试全绿才进下一阶段；不顺手改逻辑。

## 3. 目标模块（agent_core）

| 新文件 | 内容（从哪来） | 性质 |
| --- | --- | --- |
| provider_parse.cpp/.hpp | SseLines、ToolCallAccumulator、request_body、parse_models_json/meta、parse_context_probe_error、estimate_context_tokens、context_overflow_message、retryable_provider_error、http_error_body_message（provider_openai） | 纯，单测 |
| provider_openai.cpp | curl complete_chat + 重试循环（保留） | IO |
| provider_discovery.cpp | list_models/list_model_capabilities/probe_context_window + fetch_models_body | IO |
| subprocess.cpp/.hpp | run_subprocess + BoundedOutputSink（tools） | 进程管理，共享 |
| workspace.cpp/.hpp | resolve_within_workspace（tools） | 边界，纯+FS |
| tools_registry.cpp | ToolRegistry + to_openai_tools_json + sanitize_display + cap_tool_output | 聚合 |
| tools_bash.cpp | BuiltinRuntime/run_bash + parse_bash_arguments | 工具 |
| tools_file.cpp | FileRuntime | 工具 |
| tools_git.cpp | GitRuntime | 工具 |
| md_geometry.cpp | display_width/wrap/clip（md） | 纯 |
| md_render.cpp | md4c 回调 render（md） | 纯 |
| session_store.cpp | IO/锁/list/remove/backup（session） | IO |
| session_serde.cpp | session_to/from_json + session_to_markdown（session） | 纯 |
| config_file.cpp | 加载/保存/迁移/校验（config） | IO |
| config_env.cpp | env 合并 + to_provider + parse_mcp_cli_spec（config） | 纯+env |

## 4. 目标模块（TUI / CLI）

TUI（先建 `tui_state.hpp` 的 TuiState，再拆）：
| 新文件 | 内容 |
| --- | --- |
| tui_screen.cpp | RawMode + LiveScreen + term_columns/rows + split_ansi_paragraphs |
| tui_render.cpp | 现有纯渲染/补全（保持） |
| tui_slash.cpp | handle_slash（命令分发） |
| tui_sidebar.cpp | start_sidebar/enter_sidebar_dir/insert_sidebar_selection + 预览 pane（load/render/open/close/set_mouse） |
| tui_turn.cpp | start_turn/drain_tool_events/approval 桥/models fetch/probe/compact worker |
| tui_input.cpp | 键位分发/composer 编辑/历史召回 |
| tui.cpp | run_loop 编排 + 帧渲染 + 状态初始化 |

CLI：
| 新文件 | 内容 |
| --- | --- |
| cli_main.cpp | main 分发 + print_usage + sigint + load_app_config/make_store |
| cli_ask.cpp / cli_run.cpp / cli_session.cpp / cli_mcp.cpp | 各子命令 |
| cli_common.cpp | print_provider_error/provider_with_retry_log/apply_*_flags |

## 5. 分阶段计划（低→高风险）与当前进度

- **P1 纯函数抽取** —— **已完成**（提交 `04b36f1`）：`md_geometry`、`subprocess`、`workspace`、`provider_parse`。零状态、行为保持；四 preset CTest 3/3 + pty TUI 195 PASS + CLI ALL PASS + check.py。
- **P2 类/子命令抽取** —— **已完成**（CLI `a879bcd`；tools 拆分见后续提交）：CLI → `cli_common`/`cli_commands`/`cli_main`（1115→86）+ `session_serde`；tools → `tools_file`（FileRuntime）/`tools_git`（GitRuntime），tools.cpp 保留 registry+bash（1095→222）。**config_file/env 拆分暂缓**（config.cpp 777 行、env/file 合并高度耦合，价值低）。
- **P3 TUI 状态对象 + screen 抽取** —— **screen 层已完成**：`tui_screen.hpp` 承载 RawMode/term size/tint/LiveScreen/split_ansi_paragraphs/steady_now（tui.cpp 3626→3195 行）；四 preset + pty 双环境（常规）验证。TuiState 完整状态对象未单独建：P4 以 `TurnState` 首片（tui_turn）+ 键位分发 `Ctx` 引用聚合替代达成同等解耦（见 §8），完整状态对象化留作后续可选重构。
- **P4 TUI 逻辑拆分** —— **已完成**（键位分发提交 `c1a57f3`）：预览 → `tui_preview`；侧栏扫描 → `tui_sidebar`；models/probe/compact worker → `tui_workers`；**TurnState 首片**（回合/审批共享状态 + `run_turn_worker`）→ `tui_turn`；**slash 群** → `tui_slash.hpp`（`handle_slash_impl` 模板 over 引用上下文；作用域感知扫描重命名，只改代码段、保护字符串/注释；内部遮蔽局部 `history`→`hchat`；tui.cpp →1917）。**input 助手**（hist_up/down、mark_edited、insert_sidebar_selection）→ `tui_input.hpp`；**idle 帧渲染** → `tui_frame.hpp`（`render_frame_impl`，纯渲染无控制流；tui.cpp →1694）。**主循环键位分发** → `tui_keys.hpp`（`dispatch_input_impl(Ctx&, buf, n)` 返回 `LoopCtl`；406 行字节循环整段迁出，控制流按 §8 人工翻译；tui.cpp **1694→1268**）。

每阶段一个提交；任何阶段测试不绿即回退该阶段。

**目录分层（2026-11-17，提交 `f698521`）**：src/ 不再平铺，按模块分为 `cli/ tui/ core/ provider/ tools/ mcp/ render/`（git mv 保留历史）；内部头文件随模块同目录，公共契约保持 `include/mini_agent/`；CMake 源路径、src/README.md 分层树、文档链接/ prose 路径同步重映射。纯重组、无逻辑变化。

**宽环境“停滞”复核结论（2026-11-17）**：此前记录的“宽 USER approval 停滞”经复核**非产品缺陷**——pty 尺寸硬编码 80 列（env COLUMNS/LINES 不生效），给足预算（~200s）宽环境完整 195 PASS、exit 0；早前 100s 超时截断系 CPU 负载下的测试计时伪影。冒烟对墙钟敏感，CI 以常规环境+充足 job 超时运行。无需产品代码修改。

## 6. 风险与缓解

- **TUI 帧/行记账**（v7.1/v7.2 同类）：P3/P4 以 193 条 pty 断言 + 宽 USER 环境为门禁；LiveScreen 不变量不动。
- **闭包捕获遗漏**：TuiState 收状态时逐 lambda 核对捕获列表；编译器 + ASan 兜底。
- **头文件循环依赖**：内部头只向下依赖；公共契约不变。
- **收益**：单文件 ≤~1000 行；纯解析/几何/序列化可独立单测；TUI 逻辑可分块维护；为多 Agent/编排预留清晰边界。

## 7. 不做

- 不引入新依赖、不改公共 API/退出码/日志红线；不重写 LiveScreen 帧模型；不做"真滚动窗口"等未实现能力；不合并/重命名现有公共头。

## 8. P4 TuiState 设计与实施记录（已全部落地）

**首片已落地**（`tui_turn.hpp/.cpp`）：`TurnState` 拥有 bridge/审批原子量/session_allowed_tools(+mutex)/
config_snapshot/tool_registry/max_tool_rounds/approval_policy/result/reasoning_live；run_loop 以引用别名
保持主线程代码字节不变；worker body 移入 `run_turn_worker(TurnState&,...)`。验证：四 preset + pty 195/0 + CLI。
剩余 slash/input 按同法扩 TuiState 后迁移。

已抽尽可独立参数化的单元（preview/sidebar/workers）。剩余 turn/slash/input 三大闭包群
共享同一组可变控制状态，必须先建 `TuiState`（`src/tui/tui_state.hpp`）才能跨文件拆分。

**共享状态清单**（run_loop 局部，需收进 TuiState）：
- 终端/渲染：`LiveScreen screen`、`cols/rows`、`colors_enabled`、composer/cursor、history、`last_prompt_chars`
- 回合：`worker/turn_done/result/TurnResult`、`streaming`、`abort_requested`、`turn_start`、`reasoning_live`、`panel_tool_rows`、`tools_active`
- 审批：`bridge(TurnBridge)`、`approval_policy/abort/expanded`、`session_allowed_tools(+mutex)`、`config_snapshot`、`tool_registry`、`max_tool_rounds`
- 侧栏/预览：`sidebar_*`、`preview_pane`、`sidebar_browse`
- 模型/认证：`models_*`、`model_caps`、`auth/auth_probes`、`model_menu_open/choices/targets`、`reasoning_menu_open`
- 配置/会话：`config&`、`session&`、`store&`、`logger`（主线程独占）、`session_ids/stale`

**迁移方式**：`run_loop` 声明 `TuiState S;`，闭包 `[&]`→`[&S]`、成员 `x`→`S.x`；
再把三大群改为接收 `TuiState&` 的自由函数移入 `tui_turn.cpp`/`tui_slash.cpp`/`tui_input.cpp`。
worker 线程只读快照 + 写 bridge/result（join-then-read 纪律不变）；`logger`/`screen` 仍主线程独占。

**门禁**：每步四 preset CTest + pty TUI 195/宽 USER + CLI；审批/中断/行记账断言必须全绿；
不绿即回退该步。ASan/UBSan（CI quality）兜底线程/越界。

**实施注意（2026-11-18 实测）**：turn/审批群无内部遮蔽，别名法一次成功；slash 群有内部局部遮蔽
（/compact 的 `Chat history`）与字符串字面量含同名单词，全局正则会误改。最终用**作用域感知扫描**
（逐字符区分代码/字符串/字符/注释/raw-string，仅重命名代码段）+ 内部局部预改名（history→hchat）成功，
pty 195/0。input/帧渲染沿用同一扫描器。

**input/帧渲染迁移方案（最后一步）**：二者是主 `for(;;)` 循环体（键位分发 ~900–1300、
帧渲染 ~1300–1917），含 `continue`/循环控制，不能像 slash 那样整块搬为纯函数。方案：把循环体
抽为 `enum class LoopCtl { Next, Exit, Render };` 返回的两个模板函数
`dispatch_input_impl(Ctx&, LoopCtl&)` 与 `render_frame_impl(Ctx&)`，`continue`→`return LoopCtl::Next`、
退出→`Exit`；run_loop 只留 poll/抽干/join/调度。沿用作用域感知扫描器重命名，逐段小步+pty 门禁。
小型输入助手（hist_up/down、mark_edited、insert_sidebar_selection）可先随迁。

**控制流陷阱（2026-11-19 实测）**：主循环输入分支内嵌 `for(i over buf)` 字节循环，其 `continue`
指内层循环；外层 `continue` 才是“下一帧”；退出走 `goto done`（循环后清理标号）。三者同名不同义，
正则/扫描器无法区分，**不得自动抽取**。LoopCtl 方案须人工/AST 逐条翻译：内层 continue 保留、
外层 continue→`return Next`、goto done→`return Exit`，并保留 done 清理路径。此为最后一步，
建议专注会话手工执行+pty 门禁。

**键位分发落地（2026-11-19，提交 `c1a57f3`）**：406 行 POLLIN 字节循环整段迁入
`src/tui/tui_keys.hpp` 的 `dispatch_input_impl(Ctx& S, const char* buf, ssize_t n)`，
返回 `enum class LoopCtl { Next, Exit, Render }`。签名带 buf/n 是因为 ESC 解析会改 `i` 并
回读缓冲区，函数须拥有整个内层字节循环。实际翻译结果：内层 `continue` 原样保留；
4 处 `goto done`（streaming/compact 双 Ctrl-C、空闲 2s 内双 Ctrl-C、Ctrl-D、`/exit`）→
`return LoopCtl::Exit`，调用点收到 Exit 后执行 `goto done` 走原清理路径；**键位分发段内不存在
外层 continue**（仅有的两处外层 continue 在 compact_done/turn_done 完成回调里，按设计留在
run_loop），故 `Next` 当前保留未用（头文件注释已说明）。状态名 `S.` 前缀由一次性作用域感知
工具 `tools/p4_migrate.py` 完成（跳过字符串/注释/成员访问/遮蔽局部量），控制流与去缩进人工执行；
5 个本地 lambda（handle_slash/hist_up/hist_down/mark_edited/insert_sidebar_selection）保持
裸名转发 `*_impl(S)`。SlashCtx 补 `tab_state` 引用成员；InputCtx 及 4 个包装 lambda 因此死代码化，
一并删除。完成回调（models/compact/probe/sidebar/turn_done）与 `done:` 清理**有意不抽**，
保留各自 goto/continue 于 run_loop。验证：四 preset CTest 3/3 零告警、pty TUI 195/0
（常规 + 宽 USER=abcdefghijklmnopqrstuvwxyz）、CLI ALL PASS、check.py --strict-project PASS。