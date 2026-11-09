# Agent Harness 技术与实现调研报告

> 性质：技术调研报告（设计参考角色，非本项目规范）。
> 日期：2026-11-09。方法：以本仓库已登记的参考资料为主干（[references/](references/README.md) 各文件及其核对记录），关键外部论点已当天在线复核（Anthropic 工程博客、MCP 官方规范、ReAct 论文摘要）。
> 引用规则：外部链接仅为参考数据；本仓库借鉴时**只取模式与协议设计，不复制代码**，各来源许可证以 [coding-agents.md](references/coding-agents.md)、[agent-protocols.md](references/agent-protocols.md) 当天核对记录为准。

---

## 1. 什么是 Agent Harness

**Harness（挂具/执行框架）** 指包在大模型外面的那层运行时软件：模型本身只做"文本进、文本出"的推理，harness 负责把它变成能持续干活的 Agent。一个常被引用的划分来自 Anthropic 的 [Building Effective Agents](https://www.anthropic.com/engineering/building-effective-agents)（2024-12，2026-11-09 复核页面仍在，页首已标注工具格局变化）：

- **Workflow**：LLM 与工具由**预定义代码路径**编排（提示链、路由、并行、编排-工作者、评估-优化等模式）。
- **Agent**：LLM **动态决定自己的流程和工具使用**，保留对"怎么完成任务"的控制权。

两者都属于 "agentic systems"，而 **harness 就是承载这两者的宿主程序**——同一个模型放进不同 harness（Claude Code、Codex CLI、mini-agent），行为差异主要来自提示词组织、工具 schema、循环策略、错误恢复和上下文管理，全部是 harness 层的设计决策。

学术源头可追溯到 [ReAct: Synergizing Reasoning and Acting in Language Models](https://arxiv.org/abs/2210.03629)（Yao et al., ICLR 2023）：让模型交替产出"推理轨迹"与"动作"，动作接入外部环境并把观察结果回填——这正是今天所有 harness 主循环（思考 → 调工具 → 观察 → 再思考）的原型。

### 1.1 Harness 与相关概念

| 概念 | 关系 |
|---|---|
| Agent 框架（LangGraph、AutoGen、CrewAI 等，见 [agent-frameworks.md](references/agent-frameworks.md)） | 偏"库/编排层"：给开发者搭 workflow/agent 的积木；harness 偏"完整可运行的宿主"（含 UI、会话、审批） |
| Eval harness（[SWE-bench](https://github.com/SWE-bench/SWE-bench)、[AgentBench](https://github.com/thunlp/AgentBench)、[WebArena](https://github.com/web-arena-x/webarena)，登记于 [agent-benchmarks.md](references/agent-benchmarks.md)） | 传统含义：把被测对象"夹住"自动运行的调度代码——拉镜像、跑 agent、比对 patch、打分 |
| Chat 客户端 | 只有 Provider + 历史消息，没有工具循环/护栏/会话管理，不算完整 harness |

终端编码 Agent（Codex CLI、Aider、opencode、Crush、Codewhale、DeepSeek Harness、Pi 等，全部登记于 [coding-agents.md](references/coding-agents.md)）是 harness 这个词现在最常见的所指：**编码 agent harness = 模型接入 + 工具循环 + 终端前端的自洽程序**。

---

## 2. Harness 的核心子系统

以业界共识 + 本项目实现（[architecture.md](architecture.md)、[007-agent-v1.md](design/007-agent-v1.md)、[008-mcp-tools.md](design/008-mcp-tools.md)）归纳，一个 agent harness 由以下子系统构成：

### 2.1 Provider 层（模型接入）

- 职责：统一封装不同 LLM 后端（OpenAI 兼容、Anthropic、本地网关），屏蔽线格式差异；流式（SSE）优先。
- 关键设计点：
  - **多协议映射**：opencode 为每个协议单独维护映射表（openai-chat 直发 `reasoning_effort`、anthropic 走 `thinking.budget_tokens` 且预算必须 < max_tokens、DashScope 系 `enable_thinking`），见 [context-reasoning-survey.md §2.4](references/context-reasoning-survey.md)。
  - **能力元数据目录**：Codex CLI 从 `GET /models` 拉完整 ModelInfo 目录（含 `context_window`、`supported_reasoning_levels`）并磁盘缓存；opencode/Pi 用 models.dev 中央目录 + 随仓库快照兜底；mini-agent 用 `config.context_window`（权威）> 网关 caps > models.dev 内嵌目录（构建期 TSV 快照，零网络）的四级兜底链（[010-model-metadata.md](design/010-model-metadata.md)）。
  - **诚实原则**（四家调研的共同底线）：目录没数就不假装知道；未知模型给保守常数或 None，不猜窗口。
- mini-agent 对应：[provider_openai.cpp](../src/provider_openai.cpp)（HTTPS + SSE）、[config.cpp](../src/config.cpp)、[models_dev.cpp](../src/models_dev.cpp)。

### 2.2 Agent 循环（Loop）

- 职责：请求 → 解析回复（文本 / tool_calls / stop reason）→ 执行工具 → 结果回填历史 → 再请求，直到停止条件。
- 停止条件是 harness 的"自主度旋钮"：模型不再调工具、达到 max turns、用户中断、成本/时间预算。
- 编排形态的取舍回引 Anthropic 的 workflow/agent 二分：**循环本身可以很简单（mini-swe-agent 约百行），复杂度应放在工具接口而非编排花型**——"能简单就不要复杂"是其核心建议。
- mini-agent 对应：[loop.cpp](../src/loop.cpp) + [chat.cpp](../src/chat.cpp)（`run_turn`），v2 工具调用循环见 008 设计。

### 2.3 工具层（Tool Layer）

harness 区别于 chat 客户端的核心。三个子问题：

**a) 工具如何为 LLM 而设计（ACI，Agent-Computer Interface）**
[SWE-agent](https://github.com/SWE-agent/SWE-agent) 提出的概念（论文 [arXiv:2405.15793](https://arxiv.org/abs/2405.15793)）：工具接口要面向模型而非面向人——输出裁剪（只回必要行区间）、行号化编辑、失败时返回"可行动的提示"而非裸栈回溯。这是 harness 工程质量的最大杠杆点。

**b) 编辑协议选型**
[Aider](https://github.com/Aider-AI/aider) 的实践：search/replace block vs unified diff 两种模型可稳定产出的格式，配错误后的重试与自动修复流程；repo map 做上下文选取。mini-agent 对应 [diff.cpp](../src/diff.cpp)。

**c) 工具扩展：内置 vs 插件 vs MCP**
两条主流边界划法（本仓库 [ADR 005](decisions/005-mcp-transport-abstraction.md)、[ADR 006](decisions/006-tool-approval-policy.md) 与 DeepSeek Harness 的对照结论）：
- **DeepSeek Harness（dsh）**："Everything is a Plugin"（Cordis 运行时，UI/工具/行为皆插件）——扩展即代码。
- **MCP 路线**：[Model Context Protocol](https://modelcontextprotocol.io)（开放规范，JSON-RPC 2.0）——扩展即协议对端。Host（LLM 应用）内建 Client 连接外部 Server；Server 提供 **Tools / Resources / Prompts** 三类能力，Client 可提供 Sampling / Roots / Elicitation。灵感同构于 Language Server Protocol（LSP）。
- mini-agent 采用 MCP client 路线：stdio（换行分隔 JSON-RPC，stdout 仅协议消息）+ Streamable HTTP（每消息一个 POST，响应 JSON 或 SSE 二选一，`Mcp-Session-Id` 会话管理、`MCP-Protocol-Version` 头），版本基线 [2025-06-18 spec](https://modelcontextprotocol.io/specification/2025-06-18)，v2 明确裁剪 resources/prompts/sampling/elicitation/roots/progress/OAuth。实现：[mcp_stdio.cpp](../src/mcp_stdio.cpp)、[mcp_http.cpp](../src/mcp_http.cpp)、[mcp_rpc.cpp](../src/mcp_rpc.cpp)。

**d) 工具的另一形态：CLI-as-tools**
Anthropic 的 [Effective Context Engineering](https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents) 与业界实践都指出：对终端 harness 而言，把 `rg`/`git`/编译器这类现成 CLI 直接当工具暴露，往往比为每个能力写结构化 tool 更省 token、更通用。本仓库登记于 [cli-as-tools.md](references/cli-as-tools.md)。

### 2.4 上下文管理（Context Engineering）

Anthropic 提出 "context engineering" 概念取代 "prompt engineering"（[原文](https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents)）：上下文是有限资源，harness 要管理的不只是提示词，而是"系统提示 + 工具 + 外部数据 + 历史消息"的整体配比。harness 层的具体机制：

- **主动压缩（compaction）**：接近窗口阈值时把旧历史摘要化。参考组共同做法：阈值吃目录数字——Codex 窗口 ×95% 做可用量、90% 触发压缩；opencode 动态 output 预算（`WINDOW_HEADROOM=0.01`）；Pi 公式 `tokenCount > contextWindow - reserveTokens`；Codewhale 保守常数 102_400（=128k 的 80%）兜底。详见 [context-reasoning-survey.md §1](references/context-reasoning-survey.md)。
- **历史裁剪 vs 摘要**：mini-agent v1 尚无压缩（缺口已记录，调研结论指出"若加压缩，Codex 的 effective_percent 模型最简可抄"）。
- **前缀缓存意识**：opencode 发现顶层 effort/参数变化会击穿 provider prompt cache，做了逐消息标记下沉——这是只有 harness 层才会遇到的问题（[context-reasoning-survey.md §2.4](references/context-reasoning-survey.md)）。
- **上下文选取**：Aider repo map、sub-agent 独立上下文窗口（Anthropic [多 Agent 研究系统](https://www.anthropic.com/engineering/multi-agent-research-system)）都是"把对的 token 放进窗口"的手段。

### 2.5 会话与状态持久化

- 一进程一会话（mini-agent [ADR 001](decisions/001-session-model.md)）vs 跨会话恢复（Pi 的 `/new /resume /name /fork /compact` 命令族，为 5.5 扩展点既定形态）。
- 原子写 + 文件锁（mini-agent [session.cpp](../src/session.cpp)，sessions 目录 0600）。
- 失败轮不留脏历史：错误只进展示层，不进 Chat 存储（007 §5 设计，Aider"错误行进历史区而非弹界面"的变体）。

### 2.6 前端与交互层

CLI（非交互单次调用，`ask` 契约）、TUI（交互式多轮）、以及 RPC/JSON 驱动模式（Pi 的 print/JSON/RPC 三模式是"被上层程序当子进程驱动"的参考形状）。TUI 调研（[tui-agent-ui-survey.md](references/tui-agent-ui-survey.md)、[md-renderers.md](references/md-renderers.md)）的 harness 级共识：

- 事件与渲染分离：SSE delta 进队列，主循环按帧抽干重绘（Codex）。
- 状态行承载运行态（model/session/interrupt 提示，Ctrl-C 双层语义：先中断请求、再退出）。
- streaming 期消息排队（Pi：Enter 排队 steering / Escape 停止）与 interrupt-and-redirect（Hermes：中断后输入行立即可用，直接下新指令）。
- mini-agent 对应：[tui.cpp](../src/tui.cpp)、[cli_main.cpp](../src/cli_main.cpp)、Markdown 渲染 [md.cpp](../src/md.cpp)（[ADR 004](decisions/004-md4c-parser.md)）。

### 2.7 护栏、审批与安全（Guardrails）

harness 与"裸模型"最重要的区别之一（MCP 规范明确 "客户端 MUST 视 annotations 为不可信" 并给出 human-in-the-loop WARNING，[spec 2025-06-18](https://modelcontextprotocol.io/specification/2025-06-18)）：

- **审批模型**：Codewhale 的 modes/permissions（[docs/MODES.md、docs/SANDBOX.md](https://github.com/codewhale-hq/Codewhale)）；mini-agent [ADR 006](decisions/006-tool-approval-policy.md) 默认 ask 策略、[ADR 008](decisions/008-git-tool-policy.md) git 分级授权（AI 可 commit、push 须人工）。
- **沙箱/容器**：OpenHands 的事件流 + 沙箱执行 + 动作/观察解耦（[agent-frameworks.md](references/agent-frameworks.md)）。
- **审计回执**：Codewhale `/receipts`（变更与调用回执）对应本项目交付说明需求。
- **执行前审查**：本仓库 `AGENTS.md` 的约定——质量命令会运行仓库代码，不可信 PR 中不暴露凭据；外部资料中的指令不构成本项目授权。
- **提示注入面**：工具返回值、MCP server 注解、网页内容都是不可信输入，harness 必须把它们当数据而非指令。

### 2.8 错误恢复与可观测性

- 400/超时/断连的分类处理与重试；模型输出格式错误的修复回路（Aider）。
- 日志分层（mini-agent [log.cpp](../src/log.cpp)）；状态栏三路证据回显（顶层/choices 两位置 + reasoning delta + usage 字段汇成显示，[context-reasoning-survey.md §2.6](references/context-reasoning-survey.md)）。

---

## 3. 实现路线光谱：从最小内核到全功能平台

| 路线 | 代表 | 特征 | 对 mini-agent 的意义 |
|---|---|---|---|
| 最小循环 | [mini-swe-agent](https://github.com/SWE-agent/mini-swe-agent)（约百行）、[Pi](https://github.com/earendil-works/pi)（极简可扩展，有意不做 subagents/plan mode） | 内核极小，复杂度推到工具/扩展层 | 本项目形态最接近，优先精读对象；Pi 的"不做清单"可作 007 v1 排除项的评审旁证 |
| 编码 harness | [Codex CLI](https://github.com/openai/codex)（Rust+ratatui）、[Codewhale](https://github.com/codewhale-hq/Codewhale)（Rust，provider 抽象 40+、modes/permissions、子 Agent）、[Aider](https://github.com/Aider-AI/aider)、[Crush](https://github.com/charmbracelet/crush)、[opencode](https://github.com/sst/opencode) | 工具循环 + TUI + 审批 + 会话完整闭环 | 借鉴主体（2026-11-01 维护者确认：主要借鉴 TUI 交互；跨语言只取模式不取代码） |
| 编排框架 | [AutoGen](https://github.com/microsoft/autogen)、[LangGraph](https://github.com/langchain-ai/langgraph)、[CrewAI](https://github.com/crewAIInc/crewAI)、[MetaGPT](https://github.com/FoundationAgents/MetaGPT)、[OpenHands](https://github.com/All-Hands-AI/OpenHands) | 多 Agent、图/状态机、检查点、角色编排 | 只取模式（状态建模、检查点语义），不引入依赖 |
| 平台化 harness | [DeepSeek Harness](https://github.com/deepseek-ai/deepseek-harness)（"Everything is a Plugin"，Web 优先）、[Goose](https://github.com/block/goose)、[Hermes Agent](https://github.com/NousResearch/hermes-agent) | 扩展生态、多前端、插件治理 | 架构对照样本：评审时论证"插件 vs MCP 工具"两种边界划法的取舍 |
| 代码即工具 | [smolagents](https://github.com/huggingface/smolagents)（CodeAgent：模型直接写代码而非 JSON 工具调用） | 另一种工具调用范式 | "最小抽象"对照样本 |

---

## 4. mini-agent 的 harness 定位（本项目现状）

对照 §2 各子系统，mini-agent（C++、零新增第三方依赖、Linux 优先）已具备完整 harness 骨架：

| 子系统 | 实现 | 成熟度 |
|---|---|---|
| Provider | [provider_openai.cpp](../src/provider_openai.cpp)（OpenAI 兼容 + SSE）+ [models_dev.cpp](../src/models_dev.cpp) 元数据 | v1 已验证，v4.2/v4.7 目录链落地 |
| Agent 循环 | [loop.cpp](../src/loop.cpp) / [chat.cpp](../src/chat.cpp) | v1 chat + v2 工具循环（008） |
| 工具层 | [tools.cpp](../src/tools.cpp)：内置 `run_bash` + MCP client（[mcp_stdio.cpp](../src/mcp_stdio.cpp) / [mcp_http.cpp](../src/mcp_http.cpp)） | v2 已落地 |
| 审批护栏 | ADR 006 默认 ask、ADR 008 git 分级 | 已落地 |
| 会话持久化 | [session.cpp](../src/session.cpp) 原子写 + 锁 | v1 已验证 |
| 前端 | [cli_main.cpp](../src/cli_main.cpp) + [tui.cpp](../src/tui.cpp)（自绘帧模型、streaming 排队、Ctrl-C 双层） | v1/v1.x 已验证 |
| 上下文压缩 | 仅显示 ctx%，无 compaction | **与四家参考组的最大缺口**（调研 §3 结论 2） |
| 跨会话命令族 | `/new /resume /fork /compact` 形态 | v2/v4 规划中 |

设计判断链：v1 有意排除 tool calling 保内核小（007 §2，与 Pi 同款论证）→ v2 以 MCP + 内置工具补工具层（008，ADR 005 传输抽象）→ 元数据/上下文策略按 [context-reasoning-survey.md](references/context-reasoning-survey.md) 四家对照持续演进（010）。

---

## 5. 评测 harness（Eval Harness）简述

Agent 的"好坏"最终要靠可自动执行的判据界定，这部分传统上就叫 eval harness：

- **SWE-bench**（[repo](https://github.com/SWE-bench/SWE-bench)，论文 [arXiv:2310.06770](https://arxiv.org/abs/2310.06770)）：任务→环境（Docker 镜像）→验证（测试 patch 通过与否）的契约；harness 负责拉镜像、跑被测 agent、比对结果。借鉴点是"**完成判据必须可自动执行**"，映射到本仓库即 `scripts/check.py` 与质量命令门禁。
- **AgentBench**（[arXiv:2308.03688](https://arxiv.org/abs/2308.03688)）/ **WebArena**（[arXiv:2307.13854](https://arxiv.org/abs/2307.13854)）：多环境评测维度划分，仅作参考。
- 注意时效性：基准分数随模型/数据集版本快速过期；[agent-benchmarks.md](references/agent-benchmarks.md) 登记的原则是不复制数据集内容、不执行第三方基准脚本。

Anthropic 对 agent 评估的工程经验（LLM-as-judge + 人工双轨、小样本早期即可信）见 [multi-agent-research-system](https://www.anthropic.com/engineering/multi-agent-research-system)。

---

## 6. 参考资料清单

### 规范与论文（外部，已当天复核或仓库登记）
- ReAct（2022/ICLR 2023）：https://arxiv.org/abs/2210.03629
- MCP Specification 2025-06-18：https://modelcontextprotocol.io/specification/2025-06-18（本仓库核对记录见 [agent-protocols.md](references/agent-protocols.md)）
- SWE-bench：https://arxiv.org/abs/2310.06770 ；SWE-agent / ACI：https://arxiv.org/abs/2405.15793
- Anthropic 工程系列：Building Effective Agents、Effective Context Engineering、Multi-Agent Research System（链接与核对状态见 [ai-agent-learning.md](ai-agent-learning.md)，该文 2026-11-05 已逐条校验）

### 第三方 harness 仓库
统一登记于 [coding-agents.md](references/coding-agents.md)（含许可证核对与"可借鉴/禁复制"结论）、[agent-frameworks.md](references/agent-frameworks.md)、[agent-protocols.md](references/agent-protocols.md)、[agent-benchmarks.md](references/agent-benchmarks.md)。

### 本仓库内证据
- [architecture.md](architecture.md)、[design/007-agent-v1.md](design/007-agent-v1.md)、[design/008-mcp-tools.md](design/008-mcp-tools.md)、[design/010-model-metadata.md](design/010-model-metadata.md)
- ADR：[001 会话模型](decisions/001-session-model.md)、[005 MCP 传输抽象](decisions/005-mcp-transport-abstraction.md)、[006 工具审批](decisions/006-tool-approval-policy.md)、[008 git 工具策略](decisions/008-git-tool-policy.md)
- 调研报告：[context-reasoning-survey.md](references/context-reasoning-survey.md)（2026-11-08，四家 harness 源码实读 + file:line 证据）、[tui-agent-ui-survey.md](references/tui-agent-ui-survey.md)
- 学习路径与来源总汇：[ai-agent-learning.md](ai-agent-learning.md)

### 已知不确定性（诚实标注）
- 本报告引用的外部仓库行为均以登记/核对日期的元数据与 README 为准；除 [context-reasoning-survey.md](references/context-reasoning-survey.md) 覆盖的五个项目外，其余源码未精读，引用具体实现细节前需按当天源码复核。
- Anthropic "Managed Agents" 等新页面已出现，说明其 2024 年文章的工具格局描述部分过时——工程原则（简单优先、上下文是有限资源）仍成立，具体产品形态引用前需重新核对。
