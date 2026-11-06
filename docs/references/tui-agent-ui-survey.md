# TUI 终端人机交互界面调研：Codewhale / Pi / opencode / Codex CLI

## 文档状态

- 性质：外部参考资料调研报告（设计参考角色，非本项目规范）。
- 调研日期：2026-11-06。方法：实读本机 checkout 源码（只读，未运行任何被调研项目的代码），关键结论经抽查复核。
- 源码位置与修订：

| 项目 | 本地路径 | 修订 | 技术栈 |
| --- | --- | --- | --- |
| Codewhale | `/home/marvin/code/repos/AI/Codewhale` | `4df5d2914`（main，v0.10.2 时期） | Rust + ratatui（fork `codewhale-ratatui`）+ crossterm + syntect |
| Pi | `/home/marvin/code/repos/AI/pi` | `2c8d1feb1`（main） | TypeScript，自研 `packages/tui` 差分渲染库 |
| opencode | `/home/marvin/code/repos/AI/opencode` | `69269e4ebe`（dev） | TypeScript + Solid.js + `@opentui/core`/`@opentui/solid`（已从 Go+bubbletea 迁移） |
| Codex CLI | `/home/marvin/code/repos/AI/codex-main` | 无 git 元数据（快照） | Rust + ratatui + crossterm + pulldown-cmark + syntect/two-face |

旁证：`/home/marvin/code/repos/AI/pi-agent-book`（讲 pi 架构的书，`book/02-runtime.md`、`book/04-memory.md` 含 TUI 交互章节）。

- 抽查更正两条（调研初稿有误，已按源码修正）：Codewhale **没有** `crates/chord`（chord 一词在其代码里指组合键类型如 `ComposerSubmitChord`）；pi 的 `packages/chord` 是跨进程热重载对象框架，**与键位无关**。下文均以复核后事实为准。
- 许可证层沿用 [coding-agents 索引](coding-agents.md) 2026-11-05 核对结果：Codewhale/Pi=MIT、codex=Apache-2.0；opencode 本轮未重核，借鉴思路合规、复制代码前须核当天 LICENSE。

## 1. 总体格局：两条渲染路线 + 一条分层共识

四个项目按"终端界面所有权"清晰地分成两派：

| | 内联（写进 scrollback） | 全屏（alt-screen） |
| --- | --- | --- |
| 代表 | Pi regular 模式；mini-agent（本项目） | Codewhale 默认、Codex CLI 默认、opencode |
| 好处 | 保留终端原生 scrollback/mouse-copy，SSH/tmux 友好，退出后历史可查 | 布局自由（侧栏、overlay、滚动区），无 auto-scroll 记账难题 |
| 代价 | 行级回退计数必须精确（硬换行不变量）；块不能触发滚动 | 脱离终端原生滚动，需自建滚动/搜索/选择 |

值得注意的趋同：**Codewhale、Codex CLI、Pi 三个都同时支持两种模式且可切换**——Codewhale 有 `/fullscreen` `/inline` 运行时命令（`crates/tui/src/tui/app.rs`），Codex 有 `tui_alternate_screen = auto/always/never` + `--no-alt-screen`（`codex-rs/tui/src/lib.rs` `determine_alt_screen_mode()`，且 auto 档考虑 SSH 场景），Pi 直接做成两个渲染后端（`TuiMainScreen`/`TuiAltScreen`）。只有 opencode 锁全屏。对 mini-agent（当前纯内联）而言，"内联为默认 + 保留切换到全屏的架构缝隙"是业界主流答案。

分层共识：四者都把 **agent 核心与 TUI 前端解耦**（Codex 的 `core`/`app-server`、opencode 的 server+SSE client、Pi 的 `pi-tui` 独立库 + coding-agent 前端、Codewhale 的 tui/core 分离），印证本项目 `agent_core` 静态库 + 双入口（CLI/TUI）的合理性。

## 2. 逐项目要点

### 2.1 Codewhale（Rust/ratatui，功能最"重"的一派）

- 布局：transcript + composer + **InfoLine 状态栏**（model、`ctx NN%`、cost、TTFT、tok/s、余额，`crates/tui/src/tui/infoline.rs`）+ sidebar（agent roster/session 列表）+ footer 快捷键提示 + command palette（Ctrl+P）+ model/session/theme picker。
- 流式：`StreamDisplayClock` 16ms 节拍合并 bursty SSE chunk；**显示速率上限 2400 B/s** 防止刷屏看不清（`crates/tui-kit/src/streaming/mod.rs`）；每 content block 独立 `StreamBuffer`，增量 markdown parser 只提交完整行（不显示半写的 code fence）；工具输出不缓冲直接显示。已知局限自述："catch-up staged, not wired"、无逐字打字机。
- Markdown：自研 AST + syntect 高亮，`IncrementalCodeHighlighter` 跨行保留 syntect parse state（长代码块流式高亮不重扫）。OSC 8 可点击链接。
- 输入：多行 composer、**完整 vim mode**（Normal/Insert/Visual，`vim_mode.rs` 硬编码键位）、后台模糊 `@` 文件扫描（`mention_completion.rs`）、斜杠菜单 + Tab 补全。
- 审批：**底部锚定 inline card**（不是弹窗遮罩），显示 shell 命令预览或 diff；按键 `y` 允许一次 / `n` 拒绝 / `a` session 级始终允许 / `S` 保存精确路径规则并允许 / `Esc` 取消。审批档位 `ApprovalMode = Suggest | Auto | Bypass | Never`；`[approval] default_selection` 可配默认高亮项；policy.rs 有"presentation-level stakes"（风险等级呈现）。已知缺口：**deny 规则不能在审批 UI 里保存**。
- 中断：Ctrl-C 一次中断 turn、二次（armed 提示）退出；Esc 走 `next_escape_action()` 优先级链（关菜单→取消请求→清输入）。
- 特色：`/undo` 一键撤销整个 last request 及文件改动；`/color` 给每个 session 的 composer 染色；鼠标拖选**复制为 markdown 源码**（`selection_copy_markdown`）；`low_motion` 减少 spinner 动画；`/workbar terminal` 查看 agent 的 shells；pet 粒子动画（彩蛋）。

### 2.2 Pi（自研 TS 差分渲染库，minimal + hackable）

- 哲学：TUI 是与 agent 逻辑解耦的独立库（`packages/tui`，README 自述 "differential rendering"）；界面可被扩展系统深度改造——扩展可**替换 header/footer/editor**、注册自定义组件/overlay/全屏 screen、为工具调用提供自定义 renderer（`packages/coding-agent/docs/tui.md` 的 `ctx.ui` API）。
- 渲染：regular 模式内联进 scrollback；alt-screen 模式保存 `lastFrame` 逐行 diff、只重写变化行，用 **CSI 2026 同步输出**防撕裂——与 mini-agent 的帧模型几乎同构。光标定位用 `CURSOR_MARKER`（零宽 APC 序列，终端忽略、TUI 解析后放硬件光标，服务 IME 候选窗）+ fake cursor（反显单字符，硬件光标可见时省略避免双光标）。
- 节流：`MIN_RENDER_INTERVAL_MS` + `setTimeout` 合并帧；**延迟敏感的键盘输入走 `requestImmediateRender()` 跳过节流**——"输入路径绕过渲染节流"是它区别于其他项目的明确做法。
- 输入：**Emacs 风格键位 + kill-ring**（`ctrl+w/u/k/y`、`alt+b/f`、yank-pop，`keybindings.ts`/`kill-ring.ts`），无内建 vim mode；`shift+enter`/`ctrl+j` 换行；`@` 模糊补全带防抖；斜杠命令集成同一 autocomplete。
- 审批：**刻意不做逐工具审批**（`packages/coding-agent/docs/usage.md:27`："Pi does not ask before every tool call"），改为一次性 **project trust**（首次进目录信任 `.pi/` 本地配置）+ 沙箱建议 + 扩展可用 `tool_call` handler 自行实现拦截。这是四者中最激进的取舍，理由：审批疲劳让确认退化为盲按回车。
- 状态：footer 显示 pwd、git 分支、tokens、context 用量、model（`components/footer.ts`）；OSC 7501 Program Status 把"忙碌"报给支持的工具栏（终端级进度条）。
- 主题：`system`（**OSC 10/11/4 查询终端真实配色，按 WCAG 4.5:1 对比度动态推导**）、`dark`、`light`、自定义 JSON 主题（OKHSL/OKLCH）、**热加载 + 终端外观变化自动切换明暗对**。
- 模式：interactive / print / JSON（JSONL 事件流）/ RPC 四种运行模式分离，TUI 只是宿主之一。
- 痛点（CHANGELOG 可见）：终端能力探测负担极重（Kitty 键盘协议、OSC 查询、OSC 8、Kitty 图像协议各有 fallback）；CJK/宽字符/emoji 宽度反复修；长 transcript 性能有专门 bench。

### 2.3 opencode（Solid.js 渲染进终端，产品化最重的 UI）

- 技术栈独一：`@opentui` 把 **Solid.js 细粒度响应式 + JSX 组件模型**跑在终端原语（BoxRenderable/ScrollBoxRenderable/TextareaRenderable）上，TUI 与 desktop/web 共享组件层思路。
- 布局：全屏 alt-screen；顶部 **session tabs**（数字键 1-10 直切、未读脉冲动画）+ 左侧 sidebar + 中间对话滚动区 + 右侧 composer 面板（Subagents/Shell/Terminals 子任务标签）——把"多会话/多子任务"做成一等界面概念。
- 流式：SSE → store → Solid `batch()` 合并更新，无虚拟 DOM；tree-sitter WASM 做语法高亮（唯一用 tree-sitter 的）。
- 审批：**diff 视图按终端宽度自适应 unified/split（>120 列上 side-by-side）**；权限三选 **Allow once / Always allow（项目级持久）/ Reject**（`packages/tui/src/routes/session/permission.tsx`）。
- 输入：命令面板（Ctrl+P）、`@` 文件/agent 引用补全、Leader key（Ctrl+X）组合键系统、`opencode.json` 全量键位可配；无 vim mode。
- Resume：`--continue` / `--session <id>`；server/client 分离可远程连。
- 痛点（代码注释可见）：大 diff 需要虚拟滚动、宽字符渲染裁剪、setTimeout workaround。

### 2.4 Codex CLI（Rust/ratatui，工程化调度最细）

- 渲染调度：`FrameRequester` 统一**合并帧绘制请求** + `FrameRateLimiter` 限最大帧率；流式有 `AdaptiveChunkingPolicy`——按队列深度/最老行龄在 **smooth 与 catch-up 两档自适应切换**（积压时跳节拍直接追平，这是 Codewhale 想做没接线、Codex 已落地的机制）；`MarkdownStreamCollector` 按换行提交，保证不渲染半行。
- 布局：默认 alt-screen（`auto` 档会考虑 SSH 场景），可 `--no-alt-screen` 回内联；`ChatWidget` = committed HistoryCell 列表 + active_cell（与 mini-agent 的"历史区定格 + live 块"同一模型）；**Ctrl+T 打开 transcript overlay** 看完整历史。
- 审批：`AskForApproval = Never | UnlessTrusted | OnRequest | Granular`；审批弹窗 `ExecApprovalRequest` 展示命令、cwd、**execpolicy 修正提案**（审批界面里直接提议把规则持久化进策略文件——比"session 级始终允许"更进一步）；apply_patch 走 diff 预览 + 失败展示。
- 输入：自研 `Textarea`（多行、词换行）、**vim mode 可 `/vim` 切换**、`@` 文件搜索弹窗（`FileSearchPopup` + `mention_codec`）、斜杠命令全集（`/model /status /compact /resume /statusline /vim` …，`slash_command.rs`）。
- 状态：`/status` 卡片（token 用量、**context 剩余百分比**、model、approval/sandbox 档）；**status line 项目可配**（`status_line` 数组 + 颜色开关，`codex-rs/config/src/types.rs`）。
- 通知：`notification_method = bel | osc9 | 自定义命令` + 触发条件（如 unfocused）——长任务离开终端时的打扰控制。
- 中断/恢复：Ctrl-C 双击退出（带"press again"提示态）；`codex resume` / `/resume` 进全屏 session picker。

## 3. 横向对比表

| 维度 | Codewhale | Pi | opencode | Codex CLI | mini-agent（现状） |
| --- | --- | --- | --- | --- | --- |
| 渲染栈 | ratatui fork | 自绘差分库 | Solid.js+@opentui | ratatui | 自绘 ANSI（内联） |
| 内联/全屏 | 双模，运行时切换 | 双模 | 仅全屏 | 双模（auto 考虑 SSH） | 仅内联 |
| 流式节流 | 16ms 节拍+2400B/s 限速 | MIN_RENDER_INTERVAL+输入旁路 | Solid batch | FrameRequester+自适应 catch-up | DeltaBuffer 快照+帧 diff |
| 半行安全 | 增量 parser 只提交整行 | 帧级差分 | Solid 局部更新 | MarkdownStreamCollector | md::wrap 整行不变量 |
| 语法高亮 | syntect 增量 | 主题 token | tree-sitter WASM | syntect+two-face | ANSI 手写 md.cpp |
| 审批形态 | 底部 inline card，y/n/a/S/Esc | **无逐调用审批**（trust+sandbox） | once/always/reject 选项 | 弹窗+execpolicy 修正案 | y/n/a 区块+diff preview |
| diff 展示 | inline diff 预览 | 扩展 renderer | 自适应 unified/split | unified+失败态 | unified（LCS，git 配色） |
| vim mode | 完整 | 无 | 无 | 有（/vim） | 无 |
| 状态栏 | InfoLine（model/ctx%/cost/TTFT/tok·s） | footer（pwd/git/tokens/ctx/model） | sidebar footer+usage 弹窗 | 可配 status line | 状态行 |
| 撤销 | /undo 整轮回滚 | — | — | — | 无 |
| 键位配置 | 无 keymap 配置 | 键位表可改+扩展 | 全量可配+leader | keymap 系统 | 硬编码 |
| 多会话 UI | sidebar/roster | session picker | tabs+子任务面板 | resume picker | 多进程（一进程一会话） |

## 4. 对 mini-agent 的启示（按投入产出排序）

1. **输入路径绕过渲染节流**（Pi）：本项目 poll ≤50ms tick 已把延迟压在 50ms 内，但键盘事件到达应立即触发帧——低成本、可感知。
2. **流式"整行提交"已是共性**：四家全部只提交完整行/完整 fence（Codewhale `commit_complete_lines`、Codex `MarkdownStreamCollector`），本项目 live 块按 `md::wrap` 整行记账与之同构，方向正确；可补的是 **Codewhale 的显示速率上限**（burst 时防刷屏）与 **Codex 的自适应 catch-up**（积压追平）。
3. **审批键语义向 `y/n/a/S` 收敛**：本项目已有 Allow/Deny/AllowSession；Codewhale 的 `S`（保存精确路径规则）与 Codex 的 execpolicy 修正案提示 v4 可做**持久 allowlist 编辑**，比 session 级更省点击。同时借鉴 Codewhale 的教训：deny 规则也要能在审批 UI 里保存。
4. **审批默认高亮项可配**（Codewhale `[approval] default_selection`）：配合"审批疲劳"现实，给用户选择默认风险档的权力。
5. **Esc 优先级链**（Codewhale `next_escape_action()`：关菜单→取消请求→清输入）：本项目 Esc 目前只做中断，语义可细分。
6. **`/undo` 整轮回滚**（Codewhale）：v3 已有文件工具+git commit，"撤销上一请求的全部文件改动"对编码 agent 是高频安全网；与本项目"turn 失败回退历史到轮边界"的哲学一致，扩展到文件系统层顺理成章。
7. **通知打扰控制**（Codex `notification_method` + 条件）：长 turn 离开终端的 BEL/OSC 9 通知，实现小、价值直接。
8. **alt-screen 第二形态**（三家共选）：架构上把"帧→终端"抽象为可换后端（Pi 的 TuiMainScreen/TuiAltScreen 接口分离是范例），将来若需要侧栏/子任务面板（opencode 方向）再启用；v1 内联仍是合理默认。
9. **status line 项目可配**（Codex）：本项目状态行已有连通标志/auth 状态，做成 `config` 数组成本不高。
10. **明确不做**：opencode 的 Solid.js/JSX 栈（违背零依赖与 C++ 约束）、逐字打字机（Codewhale 自述没做且列为局限）、Pi 的无审批哲学（本项目 ADR 006 审批即控制，方向相反且成文）。

## 5. 使用边界

- 本文是**模式调研**，非实现规范；落到设计须按 [templates/design.md](../../templates/design.md) 另出设计文档并记 ADR。
- 所有结论基于 2026-11-06 本地 checkout；外部项目演进快，引用前按当天源码复核（尤其 opencode 迁移后结构多变）。
- 未运行任何被调研项目；界面细节来自源码与文档，动态行为（动画、闪烁、性能）未经实测。
