# coding-agents：终端编码 Agent 实现

## 来源与版本
- 类型：第三方仓库。
- URL 或本地路径：
  - mini-swe-agent：https://github.com/SWE-agent/mini-swe-agent（约百行 Python 的最小 Agent 循环，与本项目形态最接近，**优先精读**）
  - SWE-agent：https://github.com/SWE-agent/SWE-agent（ACI：面向 LLM 设计的 Agent-Computer Interface）
  - Aider：https://github.com/Aider-AI/aider（终端结对编程；diff/editformat 与错误恢复）
  - Goose：https://github.com/block/goose（本地可扩展 Agent，早期 MCP 集成）
  - Crush：https://github.com/charmbracelet/crush（Go 终端编码 Agent，TUI 工程质量）
  - opencode：https://github.com/sst/opencode（TypeScript 终端 Agent，插件体系）
  - Hermes Agent：https://github.com/NousResearch/hermes-agent（Nous Research，Python 自我改进型个人 Agent，约 259k stars）
  - Pi：https://github.com/earendil-works/pi（earendil-works，TypeScript 极简可扩展 agent harness，约 119k stars，pi.dev）
  - Codex CLI：https://github.com/openai/codex（Rust + ratatui 终端编码 Agent）
  - Codewhale（原 deepseek-tui）：https://github.com/codewhale-hq/Codewhale（Rust 终端编码 Agent，约 41k stars；2026-08-14 从 `deepseek-tui` 更名，社区独立维护、与 DeepSeek 官方无隶属）
  - DeepSeek Harness（dsh）：https://github.com/deepseek-ai/deepseek-harness（DeepSeek 官方开源 harness，TypeScript，约 273k stars，"Everything is a Plugin"，底层 Cordis；developer preview）+ 文档 https://deepseek-harness.github.io/deepseek-harness/
- 固定 commit SHA / tag / 产品版本：未固定，引用前按任务当天重新核对。
- 最近核对日期与阅读状态：**部分核对（2026-11-01；2026-11-05 增补 Codewhale / DeepSeek Harness）**——codex/crush/ftxui/pi 许可证经 GitHub API 与仓库文件核对；Hermes、Pi 经当天 README 与仓库元数据核对（见"实际采用情况"）；Codewhale、dsh 经当天仓库元数据、README、LICENSE、CHANGELOG 与 Homebrew formula 核对。mini-swe-agent、SWE-agent、Aider、opencode 源码仍未读取。
- 许可证及核实依据：Hermes = MIT（README badge，当天核对）；**Pi = MIT**（license API + 仓库根 LICENSE 头部核对，Copyright (c) 2025 Mario Zechner）；Crush = FSL-1.1-MIT（LICENSE.md 开头核对，非 OSI 开源，**禁止代码复制**）；openai/codex = Apache-2.0（license API 核对）；**Codewhale = MIT**（2026-11-05 LICENSE 文件实读，头部版权为 "Copyright (c) 2024-2025 DeepSeek-TUI Contributors"，印证前身谱系）；**DeepSeek Harness = MIT**（2026-11-05 LICENSE 文件实读，Copyright (c) 2026 DeepSeek；另附 THIRD_PARTY_NOTICES.md 披露依赖许可证）；FTXUI = API 未返回，未核实；其余未核实。复制代码前必须逐仓库核实。

## 参考目的与范围
- 借鉴什么：
  - mini-swe-agent：最小可用的"提示 → 工具调用 → 观察 → 循环"Agent 内核，作为阅读起点。
  - SWE-agent：工具接口如何为 LLM 而设计（输出裁剪、行号编辑、兜错提示），ACI 理念。
  - Aider：编辑格式选型（search/replace block、unified diff）、模型输出错误后的重试与修复流程、repo map 上下文选取。
  - Goose / Crush / opencode：工具（tool）注册与扩展机制、会话持久化、多 Provider 抽象的工程组织方式。
  - Codewhale：与本项目形态最近的 Rust 终端 harness（Ratatui TUI、modes/permissions 审批模型、`/receipts` 审计回执、子 Agent、沙箱文档）；其拆出的 codewhale-ratatui 组件库（workbar/composer/themes）可作 TUI 布局参照。跨语言只取模式不取代码。
  - DeepSeek Harness：架构对照组——"万物皆插件"（Cordis 运行时）与本项目"MCP client + 内置工具 + ADR 006 审批"是同一问题的两种边界划法；读 docs/architecture.md 用于评审时论证取舍，不引入其依赖或代码。
- 重点文件或章节：待读取后填写精确路径。
- 不参考什么：各产品的 IDE 集成、遥测、付费服务；Aider 的 benchmark 脚本；任何需要安装重依赖才能看懂的原型代码。
- 何时阅读：实现或改进 mini_agent 的工具层、编辑协议、会话循环、错误恢复时。
- 在任务中的角色：设计参考。本模板 Python 部分要求零第三方依赖，借鉴时只取模式与协议设计，不引入依赖。

## 访问方式
在线读取 GitHub 仓库；离线精读时 clone 到本地 `.references/`（只读，记录 commit SHA）。无法访问时说明影响，不凭记忆假装核对。

## 本地差异
这些项目是"会执行的 Agent 程序"，本模板当前是"给外部 AI 工具读的工作约定"。若未来 mini_agent 演进为自带 Agent 循环的实现，本组来源是首要对标对象；现阶段仅借鉴其接口设计思想来打磨模板的 workflow 文档与检查脚本。

## 实际采用情况
2026-11-01 完成许可证与形态的部分核对（GitHub API/仓库文件，main 分支，未 clone 源码）：

- openai/codex：Apache-2.0（license API 确认）。Rust + ratatui。借鉴结论：事件与渲染分离——SSE delta 进队列、主循环按帧抽干重绘，不在回调里直接 printf 刷屏；Ctrl-C 双层语义（先中断请求、再退出）。
- charmbracelet/crush：**FSL-1.1-MIT（非 OSI 开源）**，LICENSE.md 开头已核对。**只读设计思路，禁止复制任何代码进本仓库**。借鉴结论：状态行承载运行态（model/session/"esc to interrupt"）；启动 splash 一次性打印。
- ftxui/ftxui：API 未返回许可证信息，**未核实**；ADR 003 备选方案，若未来改选必须先补核实。
- Aider / opencode / mini-swe-agent / Goose：仍未核对源码。借鉴结论（Aider）：错误行进历史区而非弹界面；提交即清空输入行。与本项目"失败轮不留脏历史"（[007 第 5 节](../design/007-agent-v1.md)）对应：错误只进展示层，不进 Chat 存储。

**Pi（2026-11-01 README/LICENSE/usage.md 核对，MIT）**：交互文档 usage.md 当日已核对（首个精读到交互层细节的来源）。可借鉴且已采纳/待定：(1) **streaming 期消息排队**（Enter 排队 steering / Alt+Enter 任务后追加 / Escape 停止 / Alt+Up 取回队列）——比本项目现行"streaming 中须先 Ctrl-C"更进一步，已在 2026-11-01 实现进 TUI（PTY 冒烟覆盖）；Alt+Enter/Alt+Up 变体未做；(2) `Ctrl+X`/`/copy` 复制末条回复（低成本，v1.x 候选）；(3) 会话命令族 `/new /resume /name /fork /compact` 与 `pi --continue`——为 5.5 跨 session/上下文扩展点的既定交互形态（v2）；(4) 页脚含 context 用量与 cost（v2，需 usage 字段）。不参考：`@` 文件、`!` shell、工具输出/thinking 折叠（依赖 tool calling，v1 排除）、Shift+Enter 多行（ADR 003 锁定单行）。极简 harness 路线，与 Hermes 的全功能取向相反、与本项目 "mini" 定位更近。三点架构借鉴：(1) `pi-ai` 独立统一 Provider 层 ↔ 本项目 `Provider` 抽象、agent loop 独立成库 ↔ `run_turn`——印证 007 分层方向；(2) 有意**不做** subagents/plan mode 以保内核小，与 007 第 2 节 v1 排除 tool calling 同款论证，可作评审旁证；(3) print/JSON/RPC 多驱动模式 ↔ `ask` 的非交互契约（其 JSON 输出模式是 `ask` 未来 `--json` 的参考形状）。扩展包生态（skills/extensions 经 npm 分发）不参考——本项目技术栈中立、无包治理需求。另注：本模板 user-guide 的 `.pi/` 忽略规则与 005 设计文档的 "pi 入口实测" 表明 Pi 本就是模板验证的目标 AI 工具之一。

以上借鉴要点已落到 [007 第 5.6 节 TUI 帧模型](../design/007-agent-v1.md)（规划中）。

**Hermes Agent（2026-11-01 README 核对，MIT）**：TUI 特性与本项目的交互目标最贴近。已采纳进 5.6 契约的两项：**interrupt-and-redirect**（Ctrl-C 中断当前响应后输入行立即可用，直接给新指令替换原请求，而非仅取消）与**斜杠命令 Tab 自动补全**（输入行前缀匹配，自绘可实现）。跨 session 记忆路线（SQLite FTS5 全文检索 + LLM 摘要）印证 5.5 "跨 session" 预留的可行性，属 v2 范畴。其 skills 自创建/自改进闭环与多渠道网关明确不参考（超出本项目范围）。

**Codewhale / DeepSeek Harness（2026-11-05 元数据 + README/LICENSE/CHANGELOG/Homebrew formula 核对，源码未 clone）**：

- 命名混淆风险（引用这两个名字前必读）：社区项目 `deepseek-tui`（2026-01 创建）已于 **2026-08-14 更名 Codewhale**（Homebrew formula 有 `deprecate! ... because: "renamed to codewhale"` 记录；README 自我声明 "started as deepseek-tui … not affiliated with DeepSeek"）；DeepSeek 官方 harness **deepseek-harness（dsh）**发布于次日 2026-08-13。见到 "deepseek-tui" 按资料创建时间判断指代对象；GitHub 上另有一批蹭名仿冒安装仓库，安装二进制只认 `codewhale-hq` 与 `deepseek-ai` 两个组织。
- Codewhale（MIT，~41k★，Rust/Ratatui）：与本项目定位最接近的现役终端 harness——provider 抽象（40+ 家与本地模型）、modes/permissions（docs/MODES.md、docs/SANDBOX.md）、`/receipts` 变更与调用回执（对应本项目审批与交付说明的审计需求）、子 Agent、Runtime API（docs/RUNTIME_API.md）与非交互驱动模式（对应 `ask` 契约方向）。默认模型为 DeepSeek V4.1 Flash，README 含赞助商业链接（orcarouter），引用其"默认推荐"时注意利益相关。
- DeepSeek Harness（MIT，~273k★，TypeScript，developer preview）："Everything is a Plugin"，插件即一切（UI/工具/行为皆为 dsh 插件，`dsh-plugin` topic 生态），默认 Web UI（`npx @deepseek-ai/dsh web`，127.0.0.1:3080），TUI 反而是社区插件壳（dsh-tui/oh-my-dsh/Martty 等）；底层 Cordis 运行时 + 论文 2608.25512（时空可组合）。官方 README/SAFETY.md 强调会破坏兼容、运行前读安全须知。借鉴定位：**架构对照样本**——与本项目 [ADR 005](../decisions/005-mcp-transport-abstraction.md)（MCP 传输抽象）/ [ADR 006](../decisions/006-tool-approval-policy.md)（工具审批）形成"插件 vs MCP 工具"两种扩展边界的答案，写设计评审时可引；源码未读前不采纳任何实现细节。
- 两项目源码均**未精读**；许可证虽为 MIT（可合规借鉴思路），涉及代码复制时仍需按当天文件复核并遵守署名条款（Codewhale LICENSE 要求保留版权与许可声明）。

**采用范围（2026-11-01 维护者确认）：主要借鉴 TUI 交互**——composer 布局、原地流式、状态行、Ctrl-C 双层语义与中断重定向、错误回显、最小斜杠命令与 Tab 补全、恢复提示；不借鉴多面板/鼠标/高亮/IDE 嵌入。各仓库具体实现文件仍未读取；跨语言（Go/Rust/Python→C++）只取模式不取代码，本仓库无第三方 TUI 依赖。
