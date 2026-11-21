# Agent 快速上手手册（agent handbook）

> 面向**新会话/新机器上的任意编码 Agent**：用最短路径获得与长期维护者相当的上下文。
> 本文是"地图+约定+陷阱"，**不重复**权威文档内容，只给链接与速查。先读 [AGENTS.md](../AGENTS.md)（统一工作约定），再按需深入。

## 1. 这是什么

mini-agent：C++17 单二进制终端 AI Agent（OpenAI 兼容 SSE + 工具审批 + MCP + 自绘 TUI）。
业务背景见 [project-context.md](project-context.md)；技术架构见 [architecture.md](architecture.md)。

## 2. 模块地图（019 拆分后）

源码按层分目录（`src/`），公共头契约在 `include/mini_agent/`（不随源码移动）：

| 目录 | 职责 | 关键文件 |
| --- | --- | --- |
| src/cli | 入口/子命令/退出码 | cli_main / cli_commands / cli_common |
| src/tui | 自绘 TUI | tui.cpp(主循环) / tui_slash.hpp / tui_frame.hpp / tui_turn / tui_workers / tui_sidebar / tui_preview / tui_input / tui_screen.hpp / tui_render.cpp |
| src/core | 内核 | loop(run_turn) / chat / session(+serde) / config / context / log |
| src/provider | OpenAI 兼容 | provider_openai / provider_parse / models_dev |
| src/tools | 工具层 | tools(registry+bash) / tools_file / tools_git / subprocess / workspace / diff |
| src/mcp | MCP 客户端 | mcp_rpc / mcp_stdio / mcp_http |
| src/render | 渲染 | md / md_geometry / highlight |

测试：`tests/cpp`（CTest）、`tests/pty_*_smoke.py`（端到端）、`tests/test_*.py`（模板）。
编排：`scripts/check.py`/`cpp_check.py`；小工具 `tools/`；设计 `docs/design/`；决策 `docs/decisions/`；参考 `docs/references/`。

## 3. 工作约定（务必遵守）

- **提交**：可自动 `git commit`（ADR 008 预授权），**commit message 用英文**；**push 留人工**，不自动推送。
- **分阶段**：重构/大改按阶段小步提交，每阶段验证绿才推进；不绿即回退该阶段。
- **设计先行**：复杂改动先写 `docs/design/NNN-*.md` 草案并提交，再实现；图**一律 PlantUML**（不用 mermaid）。
- **文档单一来源**：同一事实只保一份；旧稿并入权威文档后删除，不存矛盾副本。
- **回归优先**：修缺陷先补能暴露问题的测试；代码+测试+必要文档一起交付。
- **尊重未提交修改**：不擅自回退/覆盖用户改动。
- 工作流见 [workflows/](../workflows/)（implement/fix/review/design/finalize）。

## 4. 验证速查

```bash
python scripts/check.py --fast                 # 单 preset build+CTest，快速反馈
python scripts/check.py --require-initialized  # 基线+模板+项目门禁
python scripts/check.py --strict-project       # 全质量命令
cmake --build --preset linux-gcc-debug && ctest --test-dir build/linux-gcc-debug
python tests/pty_tui_smoke.py build/linux-gcc-debug/mini-agent   # TUI 端到端(~85s)
python tests/pty_cli_smoke.py build/linux-gcc-debug/mini-agent   # CLI(~11s)
python tools/cold_start_check.py build/linux-gcc-debug/mini-agent # ≤100ms
```

四 preset：linux-{gcc,clang}-{debug,release}。分层门禁与用例清单见 [design/testing/](design/testing/README.md)。
pty 支持 `--grep <标签>` 只门禁匹配断言；等待已自适应（勿改回固定长窗口）。

## 5. 陷阱清单（长期维护沉淀，改前必读）

- **主循环控制流**：tui.cpp 输入分支内嵌字节循环 `continue`≠外层 `continue`，退出走 `goto done`；抽取须人工/AST 逐条翻译，**不得用全局正则**（见 [019 §8](design/019-module-split.md)）。
- **作用域遮蔽**：搬移闭包时内部局部可能遮蔽外部同名（如 /compact 的 `Chat history`）；重命名只改代码段、保护字符串/注释（扫描器），或先改内部局部名。
- **GCC -Wchanges-meaning**：run_loop 内 ctx struct 成员与局部同名会报错；用 `#pragma GCC diagnostic ignored` 包裹（仅 GCC）。
- **System 消息位置**：严格网关（vLLM/LiteLLM）要求 System 在 messages[0]；瞬态提示折进首位 System，无首位退化末尾 User 并按下标跳过（008 D7）。
- **构建慢**：models.dev 联网刷新会拖慢每次构建；门禁/本地用 `-DMINI_AGENT_REFRESH_MODELS_DEV=OFF`（cpp_check 默认 OFF）。
- **IPv6 坏**：环境 IPv6 不通致 urllib 挂起；抓取固定 AF_INET。
- **pty 大输入**：单次 os.write 大块会死锁；分块写并抽干。
- **侧栏高亮检测**：用 `\x1b[27m`（反显重置），不是 `\x1b[7m`（composer 光标也用反显）。
- **git 路径**：status 输出相对仓库根，非 workspace；workspace 为子目录时需对仓库根解析。
- **C++ 拆文件**：新 TU 需显式 POSIX 头（fcntl/unistd 等），不靠传递包含。

## 6. 当前状态与剩余工作

- 019 拆分：**P1–P4 全部完成**（键位分发 `c1a57f3` → `tui_keys.hpp`，LoopCtl 方案，见 019 §8）；
  **tui.cpp 3626→1268**。run_loop 仅剩 poll/抽干/join/完成回调调度 + done 清理。
- 状态机集中描述：[020-state-machines.md](design/020-state-machines.md)（连接/回合/审批/worker/pane/session）。
- 学习资源单一来源：[ai-agent-learning.md](ai-agent-learning.md)。

## 7. 建议阅读顺序

1. AGENTS.md → 2. 本文 → 3. project-context + architecture → 4. 按任务选 design/NNN + references → 5. testing + 020（涉及验证/状态时）。
不要无差别全量加载；区分"已读/仅链接/不可访问"。