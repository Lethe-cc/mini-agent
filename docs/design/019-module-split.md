# mini-agent 技术方案 019：代码模块拆分与抽象（草案）

## 文档状态

- 状态：**草案 / 待人工评审**（2026-11-16）。纯提案，**未实现、未提交**；不改变任何现有行为。
- 动因：维护者 2026-11-16 提出"每个代码文件都很长，考虑拆成较小/较抽象的模块"。本文给出可行的、行为保持的分阶段拆分方案。
- 核对基线：main `b419ba8`（018 合入后）。行数实测见 §1。
- 约束：不新增第三方依赖；保持依赖方向 `mini-agent → agent_core → {curl,json,md4c,spdlog}`（core 不含 CLI/TUI，TUI 不含 HTTP）；保持 `run_turn(Chat&,Provider&)` 纯函数式入口；每阶段以现有测试（unit + pty 双环境 + 四 preset）为门禁。

## 1. 现状与问题

| 文件 | 行数 | 主要问题 |
| --- | --- | --- |
| src/tui.cpp | 3622 | `run_loop` 单函数 ~3100 行，~40 个 lambda 闭包共享局部状态；`handle_slash` ~940 行；键位/帧渲染/侧栏/预览/回合全在一处 |
| src/tools.cpp | 1383 | ToolRegistry + run_subprocess + run_bash + FileRuntime + GitRuntime + workspace 边界混居 |
| src/provider_openai.cpp | 1251 | 纯解析（SSE/tool_calls/models/probe/估算）与 curl 传输混居 |
| src/cli_main.cpp | 1115 | 四个子命令 + 公共助手 + main 同文件 |
| src/tui_render.cpp | 974 | 已拆出的纯渲染（良好，保持） |
| src/md.cpp | 802 | 几何（width/wrap/clip）与 md4c 渲染混居 |
| src/config.cpp | 777 | 文件加载/迁移/校验 与 env 合并/mcp spec 混居 |
| src/session.cpp | 612 | IO/锁 与 JSON/Markdown 序列化混居 |

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
- **P3 TUI 状态对象 + screen 抽取** —— **screen 层已完成**：`tui_screen.hpp` 承载 RawMode/term size/tint/LiveScreen/split_ansi_paragraphs/steady_now（tui.cpp 3626→3195 行）；四 preset + pty 双环境（常规）验证。**TuiState 状态对象待建**，是 P4 的前提。
- **P4 TUI 逻辑拆分** —— **待实施**（slash/sidebar/turn/input），依赖 P3 的 TuiState。

每阶段一个提交；任何阶段测试不绿即回退该阶段。

**宽环境“停滞”复核结论（2026-11-17）**：此前记录的“宽 USER approval 停滞”经复核**非产品缺陷**——pty 尺寸硬编码 80 列（env COLUMNS/LINES 不生效），给足预算（~200s）宽环境完整 195 PASS、exit 0；早前 100s 超时截断系 CPU 负载下的测试计时伪影。冒烟对墙钟敏感，CI 以常规环境+充足 job 超时运行。无需产品代码修改。

## 6. 风险与缓解

- **TUI 帧/行记账**（v7.1/v7.2 同类）：P3/P4 以 193 条 pty 断言 + 宽 USER 环境为门禁；LiveScreen 不变量不动。
- **闭包捕获遗漏**：TuiState 收状态时逐 lambda 核对捕获列表；编译器 + ASan 兜底。
- **头文件循环依赖**：内部头只向下依赖；公共契约不变。
- **收益**：单文件 ≤~1000 行；纯解析/几何/序列化可独立单测；TUI 逻辑可分块维护；为多 Agent/编排预留清晰边界。

## 7. 不做

- 不引入新依赖、不改公共 API/退出码/日志红线；不重写 LiveScreen 帧模型；不做"真滚动窗口"等未实现能力；不合并/重命名现有公共头。