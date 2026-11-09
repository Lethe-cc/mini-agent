# mini-agent v5 技术方案（文件内容预览 + 编辑→diff→渲染 TUI 一体化）

## 文档状态
- 状态：已实现（2026-11-09，验证见第 8 节；待人工评审）。
- 关联任务：维护者 2026-11-09 对话请求"增强文件内容预览，同时把编辑→diff→渲染在 TUI 中做成一体化交互"。承接 [009](009-coding-v3.md)（v3 编码能力：文件工具 + diff 审批 + 工作区边界）。
- 核对日期与适用版本：2026-11-09 实读本仓库 `src/tools.cpp`、`src/tui.cpp`、`src/cli_main.cpp`、`src/session.cpp`、`include/mini_agent/tools.hpp`、`src/diff.cpp`。代码基线：main + 维护者未提交的 v4.x provider/MCP 改动（本设计不触碰这些文件）。
- 人工评审记录：无。推进方式为 AI 务实默认（design-first），文档级别为"一般功能：简版设计"。

## 1. 背景与目标

v3 落地后，文件编辑、diff、Markdown 渲染三个能力各自存在但互不相连：

1. **diff 只出现在审批环节**：`ToolRuntime::preview()` 在审批块展示"将要改什么"；批准后执行，transcript 只落一行 `[tool] write_file · ok`，用户看不到"实际改了什么"。
2. **预览不渲染**：模型 `read_file` 读回的是给模型自己看的行号文本；即使用户想让模型"展示"一个 Markdown 文件，TUI 也只吐原文，没有渲染效果。
3. **CLI 侧同样断链**：`ask --tools ask` 审批时看 diff，执行后 stderr 只有 `[tool] name ok`。

成功判据（可验证）：

- TUI 中批准一次 `edit_file`/`write_file` 后，transcript 在 `[tool]` 行下方立即回显彩色 unified diff（实际应用的改动）。
- 模型可调用 `preview_file` 向用户展示文件：`.md`/`.markdown` 经 `md::render` 渲染，其他扩展名逐行原样展示；边界与 `read_file` 一致。
- 用户可用 `/preview <path>` 不经过模型直接预览工作区内文件。
- `ask --tools …` 在 TTY 下把执行后 diff 回显到 stderr；管道（非 TTY）输出与 v3 逐字节一致。
- 不新增第三方依赖。

## 2. 范围与非目标

本期交付：

- **工具层**：`ToolRuntime::result_preview(call, result)` 可选钩子（执行后预览，与审批前 `preview()` 分离）+ `ToolRegistry` 委托；`FileRuntime` 新增 `preview_file` 工具（第六个文件工具）；`write_file`/`edit_file` 成功后计算并暂存"实际应用 diff"供 `result_preview` 消费。
- **TUI**：`ToolRound` 携带 result_preview 文本；`drain_tool_events` 在 `[tool]` 行后提交预览块（diff 复用审批块色表并抽成共享 `tint_diff_line`；markdown 走 `md::render`）；新 `/preview <path>` 斜杠命令。
- **CLI**：`ask` 的 `on_tool` 在 stderr 为 TTY 时回显 result_preview。
- **system prompt**：默认提示词区分 `read_file`（自己看）与 `preview_file`（给用户看）。

明确不做：

- 语法高亮（无依赖前提下不做 token 级着色）。
- 审批块内"渲染视图"切换（`?` 语义保持 params 切换，不扩第三态）。
- `run_bash` 输出入流（输出现已随工具结果文本给模型；transcript 回显大输出会淹没有效信息，维持 v4.11 turn panel 一行摘要）。
- 图片/二进制预览、IDE 形态。

## 3. 约束与参考资料

- 本项目约束：AGENTS.md 小步修改、零新增依赖；[ADR 003](../decisions/003-tui-approach.md) 自绘 TUI；[ADR 007](../decisions/007-file-tools-workspace.md) 软边界；[008 §5.2](008-mcp-tools.md) 工具源单线程 owner 契约；[009 §5.3](009-coding-v3.md) preview/disarm 竞态守卫。
- 无外部 API 事实依赖；无新参考资料。

## 4. 方案与关键取舍

**执行后 diff 的来源：复用 preview 的竞态快照，而不是 git。**
`FileRuntime::call()` 成功后计算 applied diff：优先用已 arm 的 race-guard 快照（审批流下与盘上旧内容逐字节相同，零额外 I/O）；`edit_file` 未 arm 时用其执行前重读正文；`write_file` 未 arm 时在 rename 前重读旧文件；新建文件对空内容 diff。备选方案 `git diff` 不适用：工作区未必是 git 仓库，且 git 显示的是"相对 HEAD"而非"这一次调用改了什么"。

**preview_file 结果不进模型上下文，正文走 UI 通道。**
工具返回给模型的文本只有一行确认（`previewed X (N bytes) for the user`），正文暂存在 runtime 成员里由 `result_preview()` 一次性消费。备选"模型把正文原样抄进回复"浪费 token 且会双重截断；备选"把正文塞回 ToolResult 由 TUI 解析"则模型也看得到全文。暂存与消费都在 loop 线程（`call` 与 `on_tool` 同线程串行），无锁、无跨线程竞态；consume-once 防陈旧重放。

**`result_preview` 是独立钩子而不是扩写 `preview()`。**
`preview()` 有"arm 竞态守卫"副作用且发生在审批前，`result_preview()` 发生在执行后，语义与时机都不同；合一线函数会迫使调用方推断所处阶段。默认实现返回空串，MCP/GitRuntime/BuiltinRuntime 行为不变。

**CLI 回显仅 TTY。**
stderr `isatty` 门控，管道场景保持 v3 字节形状（008 回归钩子惯例）。

## 5. 详细设计

### 5.1 工具层（`include/mini_agent/tools.hpp`、`src/tools.cpp`）

- `ToolRuntime::result_preview(const ToolCall&, const ToolResult&) -> std::string`：虚函数，默认空。契约与 `preview()` 对称：只读例外是消费本 runtime 在 `call()` 中留下的每调用状态；owner/线程契约同 `call()`。
- `ToolRegistry::result_preview()`：按名字委托 owner runtime，未知名/无钩子返回空。
- `FileRuntime` 新增成员 `last_applied_path_/last_applied_diff_`（write 侧）与 `last_preview_path_/last_preview_body_`（preview_file），`call()` 成功路径写入，`result_preview()` swap 消费（一次性）。
- `preview_file` 工具定义：`{"path"}` 必填；边界 `resolve_within_workspace`，NUL 即二进制拒绝，上限 `read_limit_bytes`；返回确认文本并把正文暂存。
- applied diff 计算复用 `diff_texts/diff_against_empty/to_unified_text`（009 §5.3），label 同审批预览（`a/<rel>`、`b/<rel>`）。

### 5.2 TUI（`src/tui.cpp`）

- `ToolRound` 增 `result_preview`（纯文本、未 sanitize）与 `preview_is_markdown`（`preview_file` 且路径以 `.md/.markdown` 结尾，worker 线程解析 args 判定）。
- worker 的 `on_tool`：`executed && !is_error` 时在 loop 线程调 `registry->result_preview()`，随 round 打包进 `tool_events`（仍是 worker→UI 唯一交接通道）。
- 主线程 `drain_tool_events`：`[tool]` 行后逐行 `screen.commit()` 预览块：diff 行复用抽出的共享色表 `tint_diff_line`（与审批块同一 git 默认配色），最多 40 行后 dim 截断标记；markdown 经 `md::render(body, cols)` 提交。行首两空格缩进保持 transcript "list entries" 视觉层级。
- `tint_diff_line` 为 `approval_rows` 与 transcript 块共用（原审批块内联色表提取，行为不变）。
- `/preview <path>`：不经过模型，直接读工作区内文件（同 ADR 007 边界），`.md` 渲染、其他逐行原样，500 行截断；binary/不可读/越界报 `[error]`。登记入 `kCommands` 与 `/help`。

### 5.3 CLI（`src/cli_main.cpp`）

- `ask` 的 `on_tool`：`executed && !is_error && isatty(STDERR_FILENO)` 时取 `registry->result_preview()` 写 stderr（diff 原文，无 ANSI——CLI 不假设终端色能力，与 v3 审批预览一致）。日志红线不变：日志仍只记元数据。

### 5.4 system prompt（`src/session.cpp`）

- 默认提示词 `Tool etiquette` 增一条：`preview_file` 用于向用户展示文件（终端渲染，Markdown 带格式），`read_file` 用于自己读内容。

## 6. 异常与安全设计

- 越界：`preview_file` 与 `/preview` 均走 `resolve_within_workspace`，符号链接目标复查（ADR 007 同法）。
- 陈旧重放：result_preview 暂存一次性消费；错误结果（`is_error`）与拒绝（`!executed`）一律不产预览。
- 洪泛：transcript diff 40 行、`/preview` 500 行、正文尺寸 `read_limit_bytes`（64 KiB）三重上限；`LiveScreen::commit` 自身按列硬换行。
- 注入：diff/正文行进 transcript 前过 `sanitize_display`（008 §5.4 惯例）；`md::render` 输出自带 ANSI、被 commit 分类器按 producer-styled 直通，与模型回复同通道。
- 日志红线：applied diff 与预览正文只进终端，不进 `agent.log`（pty smoke 断言文件内容不落日志）。

## 7. 兼容性与发布

- 无配置迁移。工具列表从 5 个文件工具变 6 个：模型侧 tools JSON 多一项，v3 会话恢复不受影响（工具列表每回合现建）。
- CLI 非 TTY 输出逐字节不变；`--tools off` 路径不变。
- `result_preview` 为带默认实现的虚函数：MCP 代理、GitRuntime、BuiltinRuntime 与外部实现零改动。
- 回滚：还原本设计四文件 + tools.hpp 即可，无状态格式变化（session 文件不存预览）。

## 8. 验证情况（2026-11-09 实现轮，已执行）

| 验收项 | 测试位置或证据 | 实际执行命令 | 结果 | 限制 |
| --- | --- | --- | --- | --- |
| 六文件工具注册 + preview_file 正文/边界/一次性 | `tests/cpp/agent_test.cpp` test_file_tools | `python3 scripts/cpp_check.py` | 通过 | gcc Debug |
| write/edit applied diff（arm/未 arm/新建三种 pre-image） | 同上 v5 段 | 同上 | 通过 | — |
| registry.result_preview 委托 + 无钩子工具返空 | 同上 | 同上 | 通过 | — |
| system prompt 含 preview_file | 同上 prompt 断言 | 同上 | 通过 | — |
| CTest 全量 | agent.core / mcp.client / starter | `python3 scripts/cpp_check.py` | 通过 3/3 | 未跑 clang profile（本机无 clang 门禁配置轮） |
| 项目质量门禁 | project.json quality_commands | `python3 scripts/check.py --strict-project` | 通过 | 基础检查+项目命令 |
| 格式 | 全仓库首方文件 | `python3 scripts/cpp_format.py` | 通过 | — |
| ASan | 全部单测 | `python3 scripts/cpp_check.py --sanitizers` | 通过 | gcc Debug+ASan |
| TUI 审批后 transcript 回显 applied diff | `tests/pty_tui_smoke.py` v5 断言 | `python3 tests/pty_tui_smoke.py ./build/linux-gcc-debug/mini-agent` | 通过 | pty 手动门禁 |
| `/preview` 正文展示 + 越界拒绝 | 同上 v5 断言 | 同上 | 通过 | — |
| CLI TTY applied diff 回显 | 手工：mock CODEFIX + `script` 包 TTY | `mini-agent ask --tools allowlist`（伪 TTY） | 通过 | 手工证据，未入自动化 |
| 基线对照（pty 3 项既有失败非本设计引入） | stash 前后对比 | 同上 | 通过（失败集与基线一致，列宽违例 6→5） | 失败属维护者未提交 v4.x 工作 |

## 9. 实现偏差与已知限制

- **D1（未做）**：§2 列出的 `run_bash` 输出入流未实施（非目标声明），一体化目前覆盖"编辑→diff→渲染"主线；shell 输出仍走 turn panel 一行摘要。
- **D2**：`preview_file` 暂存状态在"call 成功但 result_preview 未被调用"的极端路径（如回合在执行与回调之间抛异常）会残留到下一次同工具调用被覆盖——无害（内容自洽、一次性消费、无跨 session 泄漏），未加显式清理。
- **D3**：`/preview` 非 markdown 分支逐行原样但跳过空行为单个空格提交（LiveScreen 空行提交会丢缩进行语义），与 markdown 分支同款处理。
- 已知限制：applied diff 与审批 preview diff 在同一竞态守卫下必然一致；若审批策略为 `off`（无 preview arm），diff 由执行前重读重建，与盘上真值一致但极端并发写仍可能显示"将要 vs 实际"差异——显示层问题，不改变写入正确性。
- 本机未验证组合：clang preset、真实 LLM 端到端（需人工）。

## 最终核对
- [x] 接口、配置、路径与当前代码一致。
- [x] 已实现与规划内容清楚区分（§2 非目标、D1）。
- [x] 外部来源、版本和实际采用内容可追溯（无外部来源）。
- [x] 验证结果有证据，未执行事项如实标明（§8/§9）。
- [x] 索引登记（design README）。
