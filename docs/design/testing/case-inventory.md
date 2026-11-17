# 测试用例分组清单

权威清单按载体分组；函数名以 `tests/cpp/agent_test.cpp` 等源码为准，本文随
新增用例更新。体系与运行方式见 [README](README.md)。

## 1. agent_test（CTest `agent.core`，55 个 test 函数）· [详细设计](01-unit-contract.md)

plain `check()` 断言、无框架依赖；`main()` 顺序调用全部 `test_*`。

| 分组 | 用例 |
| --- | --- |
| 模型/会话 | `role_round_trip` `chat_json_round_trip` `chat_parse_fails_closed` `id_safety` `session_schema_rules` `session_store_atomic_and_lock` `session_schema_bumps_for_tools` `session_markdown_export` `v413_session_prefix` `v415_session_recency` |
| 循环/工具 | `loop_success_and_failure` `loop_failure_leaves_history` `tool_calls_round_trip` `tool_loop_rounds_and_commit` `max_tool_rounds` `run_bash_and_helpers` `file_tools` `git_tools` `unified_diff` `approval_preview_rows` `v1_request_shape_regresssion` |
| 配置/Provider | `config_round_trip_and_precedence` `provider_profiles` `models_listing` `config_batched_hint` `login_status` `connection_prompt` `config_v2_fields` `mcp_cli_specs` `v3_config_and_prompt` `v4_config_fields` `v42_capabilities_and_menu` `v43_probe_parse` `v47_models_dev_catalog` `v418_parse_token_count` `v6_provider_arg_completion` `provider_retry_helpers` |
| 用量/状态 | `v4_usage_collection` `v4_status_segments` `v411_turn_panel` `v44_debug_log` `v417_turn_bridge` |
| 渲染/TUI 纯函数 | `md_width_and_wrap` `md_render_subset` `md4c_gfm_extensions` `highlight` `highlight_in_render` `tui_helpers` `tool_ui_helpers` `history_styling` `v410_arg_completion` `pane_helpers`（018） |
| 上下文 | `context_compact`（015） `context_017`（017，28 断言） |
| 日志 | `logger` |

## 2. mcp_test（CTest `mcp.client`，7）· [详细设计](02-mcp-client.md)

`handshake_and_tool_round_trip` `foreign_and_error_messages` `registry_wires_mcp_tool`
`stdio_transport_real_child` `stdio_transport_dies_clean` `http_transport_round_trip`
`call_restart_after_child_death`。stdio/HTTP 用 `mock_mcp_server.py` / `mock_mcp_http_server.py` E2E。

## 3. project_test（CTest `starter.project_name`）

模板骨架 `starter/project.hpp` 名称契约（非业务）。

## 4. pty_tui_smoke（~195 检查，真实二进制 + 进程内 mock）· [详细设计](03-pty-tui-smoke.md)

启动横幅/提示符连通标志、流式回合与历史定格、历史着色、行宽/回退不变量、
侧栏单级浏览器/`p` 预览/pane 滚轮/Esc 关闭、审批 diff 行、`/context`（含 K/M）/
`/compact`/`/reasoning`、模型菜单与多 provider 切换、日志红线（密钥/正文零泄露）。

## 5. pty_cli_smoke（43 检查，mock_openai_server 子进程）· [详细设计](04-pty-cli-smoke.md)

`ask` 流式/中断退出码 1/一次性不落 session、`run` line-mode、
`session list|rm|export|prune`、`--provider/--model` 覆盖与未知 provider 退出码 2、
配置错误契约。

## 6. 模板维护 unittest（52：test_profiles 32 + test_template 20）· [详细设计](05-template-unittest.md)

profile 装配/初始化/占位符/质量命令规则；含预期失败用例（如 rust 缺 Cargo.lock
必须先失败），其 FAIL 输出为**预期负例**，不代表门禁失败。

## 7. 冷启动预算 · [详细设计](06-cold-start.md)

`tools/cold_start_check.py`：`--version` 启动代理 + 即时 mock 的 ask 全就绪路径，
中位数 ≤100ms（成功标准 #1），CI `smoke` job 同命令。