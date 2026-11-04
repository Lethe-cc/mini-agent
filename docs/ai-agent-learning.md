# AI Agent 开发学习资源整理（截至 2026-11）

> 目标读者：准备开发 / 正在开发 LLM Agent 的工程师与团队。
> 分为七部分：必读文章、必读论文、开源项目、业界方案、协议与标准、安全与可靠性、学习路径建议。
>
> **校验说明（2026-11-05）**：本轮逐条核对了官方页面 / arXiv 摘要 / GitHub 元数据。
> 修正项：MRKL 的 arXiv 编号（原文写错）、Flowise 与 Daytona 已归档、Anthropic 多 Agent 文的现址、
> Agent Skills 已捐给 AAIF、AutoGen 仓库并未归档（是维护模式）。星标与下载量随仓库/页面当天数字更新，仍属相对参考。
> 未能在本环境访问的来源（例如 huggingface.co、部分 Google Cloud 页面）已在文末标注，不代表链接失效。

---

## 一、必读文章（工程实践类，优先读）

### Anthropic 工程博客（目前公认最有价值的 Agent 工程实战系列）
| 文章 | 时间 | 核心内容 |
|---|---|---|
| [Building Effective Agents](https://www.anthropic.com/engineering/building-effective-agents) | 2024-12 | Agent 设计第一课：workflow（提示链、路由、并行、编排-工作者、评估-优化）与真正的 agent 的边界；「能简单就不要复杂」。注意页首已声明「工具格局已变」，最新观点见下方 Managed Agents |
| [How We Built Our Multi-Agent Research System](https://www.anthropic.com/engineering/multi-agent-research-system) | 2025-06 | LeadResearcher + 并行子 Agent 架构、Prompt 工程、Agent 评估（LLM-as-judge + 人工）、生产可靠性（长任务、状态持久化、错误级联） |
| [Effective Context Engineering for AI Agents](https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents) | 2025-09 | 上下文工程取代提示工程：compaction（压缩重启）、结构化笔记（外部记忆/文件系统）、子 Agent 隔离上下文三种范式 |
| [Writing Tools for Agents](https://www.anthropic.com/engineering/writing-tools-for-agents) | 2025-09 | 工具即提示：选型（实现哪些/不实现哪些）、命名空间、返回有意义的上下文、token 效率、用 Claude Code 自动优化工具集 |
| [Building Agents with the Claude Agent SDK](https://www.anthropic.com/engineering/building-agents-with-the-claude-agent-sdk) | 2025-10 | Claude Code SDK 更名 Claude Agent SDK；「harness 同构」思路：编码 Agent 的循环可直接复用为通用 Agent 底座 |
| [Effective Harnesses for Long-Running Agents](https://www.anthropic.com/engineering/effective-harnesses-for-long-running-agents) | 2025-11 | 跨上下文窗口的长任务：initializer agent + coding agent 双角色、每轮留可交接产物（进度文件/PR）、避免「一次做太多」 |
| [An Agent Harness for Long-Running Application Development](https://www.anthropic.com/engineering/harness-design-long-running-apps) | 2025-11–12 | 前端质量向的长任务 harness：自我批评与迭代、设计判断如何编码进 harness（含「context anxiety」案例） |
| [Building Multi-Agent Systems: When and How to Use Them](https://claude.com/resources/articles/building-multi-agent-systems-when-and-how-to-use-them) | 2026-01 | 何时该拆多 Agent：按「上下文边界」而非「角色」拆分；「多 Agent 常常被用在单 Agent 更好的场景」；编排-子 Agent 之外的 swarm / capability / message-bus 模式 |
| [Claude Managed Agents](https://www.anthropic.com/engineering/managed-agents) + [文档](https://platform.claude.com/docs/en/managed-agents/overview) | 2026 | 新结论：harness 是对「模型还不能做什么」的假设，会随模型换代失效（Sonnet 4.5 的 context-reset 在 Opus 4.5 上成了死重量）；厂商开始托管长任务 Agent 运行时 |
| [When to Fold Subagents into Your Main Agent Loop](https://claude.com/resources/articles/building-agents-when-to-fold-subagents-into-your-main-agent-loop) + [迁移工具 single-agent-loop-kit](https://github.com/anthropics/single-agent-loop-kit) | 2026 | 2026 年最重要的方向修正：企业部署里 router / planner / 领域子 Agent 折回主循环常更优；子 Agent 仍擅长校验、并行工作、独立权限域 |
| [Demystifying Evals for AI Agents](https://www.anthropic.com/research/demystifying-evals-for-ai-agents) | 2026 | Agent 评估方法论：为什么单次人工试用不够、如何设计任务集与判分、回归评测怎么进 CI |
| [Multi-Agent Coordination Patterns](https://claude.com/resources/articles/multi-agent-coordination-patterns) / [Common Workflow Patterns for AI Agents](https://claude.com/resources/articles/common-workflow-patterns-for-ai-agents-and-when-to-use-them) / [Context Management](https://claude.com/resources/articles/context-management) / [Memory](https://claude.com/resources/articles/memory) / [Agent Skills](https://claude.com/resources/articles/skills) / [Harnessing Claude's Intelligence](https://claude.com/resources/articles/harnessing-claudes-intelligence) | 2025–2026 | Claude 资源中心的成系列短文：协调模式、工作流选型、上下文与记忆产品化、Skills 编写（含 [Skill Creator 测试与度量](https://claude.com/resources/articles/improving-skill-creator-test-measure-and-refine-agent-skills)）、[平台侧 Agent 可观测性](https://claude.com/resources/articles/agent-observability-on-the-claude-platform) |

### 其他高质量实践文章
- **Lilian Weng《LLM Powered Autonomous Agents》（2023-06）**：[原文](https://lilianweng.github.io/posts/2023-06-23-agent/)。Plan + Memory + Tool Use 的经典框架图，入行必读的综述博文。
- **Simon Willison 博客**（[agents 标签](https://simonwillison.net/tags/agents/)，另见 [The Lethal Trifecta](https://simonwillison.net/2025/Jun/16/the-lethal-trifecta/)）：对 function calling、MCP、提示注入风险的持续、冷静的工程评论。
- **Cognition《Don't Build Multi-Agents》（2025-06）**：[原文](https://cognition.com/blog/dont-build-multi-agents)（域名已是 cognition.com）。与 Anthropic 观点互补的反方论述——上下文碎片化会让并行 Agent 产生冲突决策；两条原则：Share context、Actions carry implicit decisions。读两边再做判断。
- **LangChain《Context Engineering for Agents》**：[原文](https://blog.langchain.dev/context-engineering-for-agents/)。把上下文工程拆成 write / select / compress / isolate 四类动作，并说明 LangGraph 如何原生支持——比读单一厂商观点更成体系。
- **OpenAI《A Practical Guide to Building Agents》**（[PDF](https://cdn.openai.com/business-guides-and-resources/a-practical-guide-to-building-agents.pdf)）+ [Agents SDK（Python）](https://openai.github.io/openai-agents-python/) / [JS](https://openai.github.io/openai-agents-js/) 文档：Guardrails、handoffs、编排模式官方指南。
- **Google ADK 文档**（[adk.dev](https://adk.dev/) / [adk-docs](https://google.github.io/adk-docs/)）：低代码 Studio 与代码优先 ADK 的双轨设计，Session、Memory Bank 等产品化抽象。
- **AWS Bedrock AgentCore 文档**（[产品页](https://aws.amazon.com/bedrock/agentcore/) / [用户指南](https://docs.aws.amazon.com/bedrock/latest/userguide/agents.html)）+ [Strands Agents SDK](https://strandsagents.com/)：组件化（运行时/身份/记忆/浏览器/代码沙箱/可观测性）地拆解「生产 Agent 需要哪几块基础设施」，是目前把边界讲得最清楚的厂商文档之一。
- **Microsoft Agent Framework 文档**：[overview](https://learn.microsoft.com/en-us/agent-framework/overview/)（.NET / Python / Go 三语言，声明式配置与 AutoGen/SK 迁移指南）。
- **agentgid.com、JetBrains / N-iX / Alice Labs 等 2026 年框架横评**（如 [agentgid](https://agentgid.com)）：了解 LangGraph / CrewAI / Microsoft Agent Framework / ADK / Mastra 的现状与选型结论（见第四部分）。横评只当线索，选型以官方文档与当天仓库状态为准。
- **Anthropic《Our Framework for Developing Safe and Trustworthy Agents》**（[原文](https://www.anthropic.com/news/our-framework-for-developing-safe-and-trustworthy-agents)）：把「可信 Agent」拆成可审计的能力/控制维度，适合做内部上线检查表的起点。

---

## 二、必读论文

### 经典 7 篇（今天主流 Agent 的每个组件都能在这里面找到原型）
| 论文 | arXiv | 定义了什么 |
|---|---|---|
| **ReAct**: Synergizing Reasoning and Acting (2022) | [2210.03629](https://arxiv.org/abs/2210.03629) | Thought→Action→Observation 循环，Agent Loop 的原型 |
| **MRKL Systems** (2022) | [2205.00445](https://arxiv.org/abs/2205.00445)（注意：旧版误写 2201.03883） | 路由器 + 专家模块（LLM/知识图谱/离散推理）的神经-符号模块化架构，今天「router + tools」的原型 |
| **Toolformer** (2023) | [2302.04761](https://arxiv.org/abs/2302.04761) | 模型自学何时调用工具：工具调用从提示技巧变成可训练能力 |
| **Reflexion** (2023) | [2303.11366](https://arxiv.org/abs/2303.11366) | 用自然语言把失败「反思」存进情景记忆，不改权重强化学习 |
| **Generative Agents** (2023) | [2304.03442](https://arxiv.org/abs/2304.03442) | 记忆流 + 反思 + 规划三件套（斯坦福小镇） |
| **Voyager** (2023) | [2305.16291](https://arxiv.org/abs/2305.16291) | 技能库 = 可执行的记忆，终身学习（今天的 Claude Skills 同源思想） |
| **SWE-agent** (2024) | [2405.15793](https://arxiv.org/abs/2405.15793) | ACI（Agent-Computer Interface）：界面设计本身就是系统能力，约束（如只看 100 行）是特性而非限制；[代码](https://github.com/SWE-agent/SWE-agent) |

进阶可补：Chain-of-Thought（[2201.11903](https://arxiv.org/abs/2201.11903)）、Tree of Thoughts（[2305.10601](https://arxiv.org/abs/2305.10601)）、CoALA 认知架构（[2309.02427](https://arxiv.org/abs/2309.02427)）、CodeAct（[2402.01030](https://arxiv.org/abs/2402.01030)，用代码统一动作空间）、Code as Policies（[2209.07753](https://arxiv.org/abs/2209.07753)，同一思路的机器人版）、HuggingGPT（[2303.17580](https://arxiv.org/abs/2303.17580)，LLM 作为控制器调度模型即工具）、Gorilla（[2305.15334](https://arxiv.org/abs/2305.15334)，大规模 API 调用正确性）、MemGPT（[2310.08560](https://arxiv.org/abs/2310.08560)，虚拟上下文管理/分层记忆）、DeepSeek-R1（[2501.12948](https://arxiv.org/abs/2501.12948)，可验证奖励的 Agentic RL 训练）。

### 评测与环境（做产品前必看）
- **SWE-bench**（[2310.06770](https://arxiv.org/abs/2310.06770)，[站点](https://www.swebench.com)）：真实 GitHub issue 修代码基准，coding agent 的事实标准；认准 Verified 子集。
- **WebArena**（[2307.13854](https://arxiv.org/abs/2307.13854)，[站点](https://webarena.dev)）/ **OSWorld**（[2404.07972](https://arxiv.org/abs/2404.07972)，[站点](https://os-world.github.io)）：网站/操作系统环境中的自主 Agent 评测。
- **AgentBench**（[2308.03688](https://arxiv.org/abs/2308.03688)）、**GAIA**（[2311.12983](https://arxiv.org/abs/2311.12983)）、**tau-bench**（[2406.12045](https://arxiv.org/abs/2406.12045)，工具+对话客服评测，含 pass^k 稳定性指标）。
- **TheAgentCompany**（[2412.14161](https://arxiv.org/abs/2412.14161)）：模拟软件公司里的真实数字化工作（浏览/文档/会议/项目管理），最接近「企业白领任务」的评测。
- **An Illusion of Progress?**（[2504.01382](https://arxiv.org/abs/2504.01382)）：Web Agent 评测的方法学批判——线上环境漂移、判分噪声会让「进步」失真；自建 eval 前必读。
- **AI Agents That Matter**（[2407.01502](https://arxiv.org/abs/2407.01502)）：重要提醒——Agent 基准普遍缺少成本对照与统计显著性，「花 10 倍 token 换 2% 提升」不该算赢。做技术选型时把成本纳入评测口径。

### 失败模式与多 Agent 科学性（2025–2026 新增，工程价值高于多数框架教程）
- **Why Do Multi-Agent LLM Systems Fail?**（[2503.13657](https://arxiv.org/abs/2503.13657)）：把多 Agent 失败归纳成可命名的分类（MAST），并指出多数失败源于系统设计而非模型能力——可直接转成你的评审清单。
- **Towards a Science of Scaling Agent Systems**（[2512.08296](https://arxiv.org/abs/2512.08296)）：单 Agent 基线够强时，多 Agent 协调收益递减；工具密集任务有「多 Agent 税」；无中心校验的架构错误传播更严重——用数据说明「架构-任务对齐」决定协作成败。

### 综述（快速建立全景图，2025–2026 最新）
- Large Language Model Agent: A Survey on Methodology, Applications and Challenges（[2503.21460](https://arxiv.org/abs/2503.21460)，综述 329 篇）
- Agentic Large Language Models, a survey（[2503.23037](https://arxiv.org/abs/2503.23037)，按 Reason/Act/Interact 组织）
- Survey on Evaluation of LLM-based Agents（[2503.16416](https://arxiv.org/abs/2503.16416)）/ Evaluation and Benchmarking of LLM Agents: A Survey（[2507.21504](https://arxiv.org/abs/2507.21504)，企业视角）
- A Survey on the Optimization of Large Language Model-based Agents（[2503.12434](https://arxiv.org/abs/2503.12434)，ACM CSUR，参数化 vs 免参数优化）
- A Survey of AI Agent Protocols（[2504.16736](https://arxiv.org/abs/2504.16736)，MCP/A2A 等协议的两维分类）
- LLM-based Agentic Reasoning Frameworks: A Survey from Methods to Scenarios（[2508.17692](https://arxiv.org/abs/2508.17692)）
- 记忆方向可补：LoCoMo 超长对话记忆基准（[2402.17753](https://arxiv.org/abs/2402.17753)）——Mem0/Letta/Zep 横评普遍用它，读横评前先读它才知道分数意味着什么。

> arXiv 编号已逐条核对（2026-11-05）。个别论文有版本迭代（v2/v3），引用时以最新版本的摘要为准。

---

## 三、开源项目

### 1. 编排框架（代码优先）
| 项目 | 定位 | 现状（2026-11 核对） |
|---|---|---|
| **[LangGraph](https://github.com/langchain-ai/langgraph)**（langchain-ai，~44k★） | 图结构状态机 + checkpoint，控制力最强 | 活跃（[文档](https://docs.langchain.com/oss/python/langgraph/overview)）；生产采用率高；配 LangSmith 可观测性；Python/JS。另有 [Deep Agents](https://github.com/langchain-ai/deepagents)——在其上封装「规划 + 子 Agent + 文件系统」的成品模式，值得读源码 |
| **[CrewAI](https://github.com/crewAIInc/crewAI)**（~60k★） | 角色制「船员」协作，上手最快 | 活跃；原型首选，生产常迁到 LangGraph；Flows 提供确定性编排 |
| **[OpenAI Agents SDK](https://github.com/openai/openai-agents-python)**（Python，~30k★ / [JS](https://github.com/openai/openai-agents-js)） | 轻量、handoffs + guardrails | 官方推荐路径；[文档](https://openai.github.io/openai-agents-python/) |
| **Claude Agent SDK**（Anthropic，[文档](https://platform.claude.com/docs/en/agent-sdk/overview)；harness 本体见 [claude-code](https://github.com/anthropics/claude-code)） | Claude Code 同款 harness 开放版 | 厂商 SDK 中采用最广的一档，适合深度绑定 Claude 的团队；[官方 quickstarts（含 autonomous-coding）](https://github.com/anthropics/claude-quickstarts) |
| **[Google ADK](https://github.com/google/adk-python)**（~22k★） | 多语言（Py/TS/Go/Java）、与 GCP Agent 平台打通 | 活跃；[文档](https://google.github.io/adk-docs/) / [adk.dev](https://adk.dev/)，配套评估/部署工具链 |
| **[Microsoft Agent Framework](https://github.com/microsoft/agent-framework)**（~15k★） | AutoGen + Semantic Kernel 合并的继任者 | 活跃，[文档](https://learn.microsoft.com/en-us/agent-framework/overview/)（.NET/Python/Go），支持 A2A。修正：**[AutoGen](https://github.com/microsoft/autogen)（~62k★）仓库并未归档**，而是进入维护/演进模式——表述应为「官方推荐新项目用 Agent Framework」，不是「代码不可用」 |
| **[Pydantic AI](https://github.com/pydantic/pydantic-ai)**（~21k★） | 类型安全的 Python Agent | 活跃；轻量、DX 好，适合工程化团队 |
| **LlamaIndex agents / Agentic RAG**（[llama_index](https://github.com/run-llama/llama_index)） | RAG 原生 Agent | 文档密集型场景；[Agentic RAG cookbook](https://github.com/run-llama/llama_index_cookbook) |
| **[smolagents](https://github.com/huggingface/smolagents)**（HuggingFace，~30k★） | 极简 code-first agent | 学习原理好材料；配套 [Agents Course](https://huggingface.co/learn/agents-course)（本环境无法访问 huggingface.co，链接未核对） |
| **[Mastra](https://github.com/mastra-ai/mastra) / Vercel AI SDK / Inngest AgentKit** | TypeScript 生态 | TS 团队首选 |
| **[Eino](https://github.com/cloudwego/eino)**（字节） | Go 原生编排框架 | Go 技术栈团队（本环境访问 github.com 该仓库超时，未能当天复核） |
| **[AgentScope](https://github.com/agentscope-ai/agentscope)**（阿里） | 生产级多 Agent + Runtime/沙箱/Channels（钉钉、飞书） | 2.0 重写：面向高能力模型「少编排、多借力」，附 Agent Service、工作区隔离 |
| **[Qwen-Agent](https://github.com/QwenLM/Qwen-Agent)**（阿里） | Qwen 模型原生：function calling、代码解释器、RAG、MCP | 国产模型生态适配 |
| **[MetaGPT](https://github.com/FoundationAgents/MetaGPT) / [ChatDev](https://github.com/OpenBMB/ChatDev) / [CAMEL](https://github.com/camel-ai/camel)** | 软件公司模拟、多 Agent 研究 | 研究型（MetaGPT 已迁到 FoundationAgents 组织）；读论文式实现好于读其生产化承诺 |

### 2. 低代码 / 平台型
- **[Dify](https://github.com/langgenius/dify)**（10 万+ stars 量级，开源）：Agentic 工作流 + RAG 流水线 + 插件市场 + LLMOps，私有化部署事实标准。
- **[n8n](https://github.com/n8n-io/n8n)**（fair-code）：自动化平台长出 Agent 能力，适合已有 n8n 流程的团队。
- **[Langflow](https://github.com/langflow-ai/langflow)**（15 万+★）：可视化编排，活跃。**修正：[Flowise](https://github.com/FlowiseAI/Flowise) 已于 2026-08 归档**（最后推送 2026-08-13），选型时视为停止维护。
- **[Coze Studio](https://github.com/coze-dev/coze-studio)**（字节，开源版 2 万+★）：零代码 Bot/多 Agent，中文场景 + 抖音/微信生态；注意开源的是 Studio，云端扣子功能更全。
- **[FastGPT](https://github.com/labring/FastGPT)、[RAGFlow](https://github.com/infiniflow/ragflow)**（InfiniFlow）：知识库问答 / 端到端 RAG 引擎。

### 3. 编码 Agent（研究 harness 设计的最佳样本）
- **[OpenHands](https://github.com/OpenHands/OpenHands)**（9 万+★，注意组织已从 All-Hands-AI 迁到 OpenHands）：开源软件开发 Agent 平台，可在 Docker/VM 沙箱跑 Claude Code、Codex、Gemini CLI 等第三方后端。
- **[Cline](https://github.com/cline/cline)**（VS Code，~71k★）、**[Aider](https://github.com/Aider-AI/aider)**、**[Goose](https://github.com/block/goose)**（Block）、**[OpenCode](https://github.com/sst/opencode) / [Crush](https://github.com/charmbracelet/crush)**（终端）、**[Gemini CLI](https://github.com/google-gemini/gemini-cli) / [Codex CLI](https://github.com/openai/codex)**（官方开源）。
- **Spec-Kit / Superpowers 等规格驱动与技能扩展**：**[GitHub Spec Kit](https://github.com/github/spec-kit)**（规格驱动开发官方工具）；Goose 的 [extensions/skills 生态](https://github.com/block/goose)。
- **[SWE-agent](https://github.com/SWE-agent/SWE-agent) / [mini-swe-agent](https://github.com/SWE-agent/mini-swe-agent)**：Princeton 出品，ACI 思想的教学级实现；mini-swe-agent 只有千行级代码，**是「读一个完整 Agent Loop」的最佳单文件教材**。
- 本仓库自身（mini_agent）的循环、工具审批与 MCP 接入即对照这些样本的最小实现。

### 4. 记忆层（2026 年最活跃的基础设施层之一）
- **[Mem0](https://github.com/mem0ai/mem0)**（~68k★）：drop-in 记忆 API（用户/会话/Agent 三级），配套论文与 [LoCoMo](https://arxiv.org/abs/2402.17753) 式评测；2026 新算法：单遍抽取 + 实体链接。
- **[Letta](https://github.com/letta-ai/letta)**（原 MemGPT，~25k★）：有状态 Agent 运行时 + MemFS（git 支撑的记忆文件系统）+「dreaming」记忆整理。
- **[Zep / Graphiti](https://github.com/getzep/graphiti)**（~32k★）：时间知识图谱记忆。**[LangMem](https://python.langchain.com/docs/langmem/)**（LangChain）、**[Cognee](https://github.com/topoteretes/cognee)**（图谱化）。
- **claude-mem、OpenDream**：编码会话记忆 / 跨框架记忆整合（写回 AGENTS.md）。
- **OpenViking**（火山引擎）：Agent「上下文数据库」，统一 Memory/RAG/Skills，`viking://` 虚拟文件系统范式（仓库位置本轮未能核实，见 [火山文档入口](https://www.volcengine.com/docs/84291)）。
- 参考基准：**agent-memory-bench**（对 Mem0/Letta/LangMem/Zep 的召回率横评，[仓库](https://github.com/hamza-dev-tech/agent-memory-bench)，小规模但方法清楚；最好者也仅 ~58% 召回——记忆仍是未解问题，值得读）。
- 一句话判断：记忆层的**检索准确率**远未达到产品级，先假设它会错，把关键事实同时写入结构化文件（AGENTS.md / 进度文件），不要只依赖向量召回。

### 5. 浏览器 / 计算机操作
- **[browser-use](https://github.com/browser-use/browser-use)**（~120k★，Python 网页操作 Agent 库，生态最热）、**[Skyvern](https://github.com/Skyvern-AI/skyvern)**（~23k★，视觉+DOM 混合）、**[Stagehand](https://github.com/browserbase/stagehand)**（~26k★，Browserbase，把 act/extract/observe 原语建在 Playwright 上）、**[Playwright MCP](https://github.com/microsoft/playwright-mcp)**。
- 沙箱/执行环境：**[E2B](https://github.com/e2b-dev/E2B)**（~15k★）、**microsandbox**（独立仓库，用于 Agent 代码执行隔离）、[Daytona](https://github.com/daytonaio/daytona)——**修正：Daytona 主仓库已于 2026-07 归档**，使用前先确认其后继形态；商业沙箱（Modal、Together Computer 等）仍在扩张。
- 研究环境：**OSWorld**（[2404.07972](https://arxiv.org/abs/2404.07972)）、WebVoyager（在线网页 Agent 评测，项目页以官方站点当天可达为准）。
- 现实提醒：浏览器 Agent 的可靠性仍受站点 DOM 漂移与判分噪声影响（见 [An Illusion of Progress?](https://arxiv.org/abs/2504.01382)）。上生产前先自建 10 个真实任务跑一遍，别看公开榜单。

### 6. 工具与内容基础设施
- **MCP 生态**：[官方 servers 仓库](https://github.com/modelcontextprotocol/servers)（~92k★）、[官方 registry](https://registry.modelcontextprotocol.io) 与 [registry 仓库](https://github.com/modelcontextprotocol/registry)、[社区 awesome 列表](https://github.com/punkpeye/awesome-mcp-servers)、[MCP 中文文档镜像](https://modelcontextprotocol.info)。
- 内容获取：[Firecrawl](https://github.com/mendableai/firecrawl) / [Jina Reader](https://github.com/jina-ai/reader)（网页→LLM-ready）；Agent 搜索 API：Exa、Tavily（[tavily-mcp](https://github.com/tavily-ai/tavily-mcp)）。
- 模型网关/本地推理：[LiteLLM](https://github.com/BerriAI/litellm)（多模型代理与成本记账）、Ollama / [vLLM](https://github.com/vllm-project/vllm)。
- 「CLI-as-tools / Code Mode」路线：与其注入上百个工具 schema，不如把工具暴露成 CLI 或代码 API 让 Agent 自己写调用（本仓库 [docs/references/cli-as-tools.md](references/cli-as-tools.md) 已登记该参考）。这是 2026 年对 MCP 工具膨胀问题的主流回应之一。

### 7. 评估与可观测性
- 开源：**[Langfuse](https://github.com/langfuse/langfuse)**（~37k★，最流行自托管，[docs](https://langfuse.com/docs)）、**[Arize Phoenix](https://github.com/Arize-ai/phoenix)**（[docs](https://arize.com/docs/phoenix)）、**[Opik](https://github.com/comet-ml/opik)**（Comet）、**[AgentOps](https://github.com/AgentOps-AI/agentops)**、**[DeepEval](https://github.com/confident-ai/deepeval)** / **[promptfoo](https://github.com/promptfoo/promptfoo)**（断言式评测、红队与回归）。
- 商业：**[LangSmith](https://docs.smith.langchain.com/)**（框架无关）、Braintrust、Galileo。
- 基准：SWE-bench Verified、WebArena、GAIA、tau-bench、TheAgentCompany；Anthropic 的 [Demystifying Evals for AI Agents](https://www.anthropic.com/research/demystifying-evals-for-ai-agents) 讲清了「评测怎么进 CI」这件事。
- 落地口径：评测集与判分标准先于功能开发存在（本仓库 `tests/` + `tools/` 的做法即此思路）；LLM-as-judge 要有锚定样例与人工抽检，否则分数漂移。

---

## 四、业界方案（厂商栈，按生态选择）

### OpenAI
- **Agents SDK**（[Py](https://github.com/openai/openai-agents-python) / [JS](https://github.com/openai/openai-agents-js)，自管循环 + handoffs + guardrails）→ **AgentKit**（工作构建器、ChatKit 嵌入）→ **Agents API / Responses API**（托管 Codex harness、自动上下文压缩、多 Agent 编排、托管沙箱、MCP 支持）。Responses API 为底座。
- ChatGPT 内：Deep Research、Operator/浏览器 Agent、Agent Mode。
- 学习材料：[OpenAI Cookbook](https://cookbook.openai.com)（本环境访问被拦截，链接按官方入口给出）与官方 [Practical Guide PDF](https://cdn.openai.com/business-guides-and-resources/a-practical-guide-to-building-agents.pdf)。

### Anthropic
- **Claude Agent SDK + Claude Code**（harness 同构，业界大量产品直接基于 Claude Code 构建）；**Claude Managed Agents**（[工程文](https://www.anthropic.com/engineering/managed-agents) / [文档](https://platform.claude.com/docs/en/managed-agents/overview)）——把长任务运行时、上下文重置、沙箱交给平台；**Agent Skills**（[SKILL.md 标准](https://agentskills.io)，2026 已捐给 AAIF，70+ 平台采纳）、**Memory tool**（beta）、Research 多 Agent 系统；Claude on AWS / Microsoft Foundry / Google Vertex 三条云分发。
- 值得读的 Anthropic 侧「反直觉」结论：harness 里的保护性假设会随模型升级失效，需定期删代码（见 Managed Agents 一文的 Bitter Lesson 引述）。

### Google
- **ADK（开源）+ Gemini Enterprise Agent Platform**（原 Vertex AI 更名）：低代码 Agent Studio、Agent Engine 托管运行时、Sessions/Memory Bank（长期记忆服务）、Agent Registry（Agent 与 MCP 注册中心 + 托管 OAuth）、Agent Marketplace、基于 OpenTelemetry 的 Cloud Observability。A2A 发起者。
- [ADK 文档](https://google.github.io/adk-docs/) / [adk.dev](https://adk.dev/)。GCP 产品页在本环境不可达，具体能力以当天官方文档核对。

### Microsoft
- **Microsoft Agent Framework**（[文档](https://learn.microsoft.com/en-us/agent-framework/overview/)，AutoGen + Semantic Kernel 合并，声明式 Agent 配置 + 迁移助手，.NET/Python/Go）、**Azure AI Foundry Agent Service**（托管、VNET 隔离）、**Copilot Studio**（低代码 + MCP 兼容）。
- **VS Code Copilot Chat**（[仓库](https://github.com/microsoft/vscode-copilot-chat)，已归档并入 VS Code 主仓库）：想读产品级 Agent 客户端实现的样本，看它归档前的历史。

### AWS
- **Bedrock AgentCore**（[产品](https://aws.amazon.com/bedrock/agentcore/) / [文档](https://docs.aws.amazon.com/bedrock/latest/userguide/agents.html)：运行时、身份、记忆、浏览器/代码沙箱、可观测性组件化）+ **[Strands Agents](https://github.com/strands-agents/sdk-python)**（开源 SDK）；与 GCP 的跨云互通（2026-07 GA）。

### 国内生态（速览）
- **阿里**：百炼（模型+Agent 工厂）/ [AgentScope](https://github.com/agentscope-ai/agentscope) / [Qwen-Agent](https://github.com/QwenLM/Qwen-Agent) / 通义系产品。
- **字节**：[Coze Studio](https://github.com/coze-dev/coze-studio)（扣子）+ HiAgent（企业版）+ [Eino](https://github.com/cloudwego/eino)；豆包系。
- **腾讯**：ADP 智能体开发平台（→ AgentOps，Connector+Skills 广场）/ 元器 / [WeKnora](https://github.com/Tencent/WeKnora)（开源知识库，~36k★）。
- **百度**：千帆 AgentBuilder（Agent-first 重构）。
- **华为**：panAI / openJiuwen（开源，企业版同源）、昇思系。
- **智谱**：AutoGLM（手机/浏览器 GUI Agent）、清言；**Manus**（通用自主 Agent 产品范式）；**实在智能**（RPA+Agent、无 API 遗留系统操作）等垂直路线。
- 桌面/个人 Agent 开源新贵：**[OpenClaw](https://github.com/openclaw/openclaw)**（个人 AI 助手 harness，39 万+★量级，衍生出 [zeroclaw](https://github.com/zeroclaw-labs/zeroclaw)（Rust 轻量版）、mimiclaw（嵌入式）等社区实现）。**这是 2026 年最重要的开源 Agent 消费级现象**：多渠道接入（WhatsApp/Telegram/钉钉式 IM）、常驻设备、自配置；也是「Agent 安全边界」问题最尖锐的样本——它把提示注入直接接到了有 shell 权限的进程上。

### 垂直明星产品（参考其交互设计）
Cursor、GitHub Copilot（Agent 模式）、Devin（Cognition）、Genspark、Perplexity（Deep Research）、Replit Agent 等。

---

## 五、协议与标准（2026 已定型的关键层）

- **MCP**（[Model Context Protocol](https://modelcontextprotocol.io/docs/getting-started/intro)，Anthropic 2024-11 发起，**2025-12-09 捐给 Linux Foundation 的 Agentic AI Foundation / AAIF**（[公告](https://www.anthropic.com/news/donating-the-model-context-protocol-and-establishing-of-the-agentic-ai-foundation)，与 OpenAI、Block 共同创立，Google/Microsoft/AWS/Cloudflare/Bloomberg 支持）：Agent↔工具/数据 的垂直协议；tools/resources/prompts 三原语，JSON-RPC 2.0；**[2026-07-28 规范](https://modelcontextprotocol.io/specification/2026-07-28)改为无状态**——去掉 `initialize` 握手与 `Mcp-Session-Id`，版本与客户端能力改为每次请求走 `_meta`，新增 `server/discover`（[changelog](https://modelcontextprotocol.io/specification/2026-07-28/changelog)），便于负载均衡与横向扩展；OAuth 授权体系。历史规范：2024-11-05 / 2025-03-26 / 2025-06-18 / 2025-11-25 / 2026-07-28。
- **A2A**（[Agent2Agent](https://a2a-protocol.org/)，Google 2025-04 发起 → Linux Foundation，[spec v1.0](https://a2a-protocol.org/latest/specification/) 于 2026-03，2026-08 加入 AAIF，150+ 组织支持）：Agent↔Agent 的水平协议；[Agent Card](https://github.com/a2aproject/A2A) 发现、有状态 Task 生命周期（submitted/working/input-required/…）、跨厂商 opaque 协作；官方 SDK：Py/JS/Java/Go/.NET/Rust。
- **AAIF 托管项目**（[aaif.io](https://aaif.io/about)）：MCP、A2A、**AGENTS.md**、**Agent Skills**（[2026 捐赠公告](https://www.anthropic.com/news/donating-agent-skills-to-the-agentic-ai-foundation)，70+ 平台采纳，含 ChatGPT/Cursor/GitHub Copilot/VS Code）、goose、agentgateway。看到「某协议归谁管」的争议时，先看它是否在 AAIF 名单里。
- **[AGENTS.md](https://agents.md/)**：事实标准——项目里给任意编码 Agent 读的说明文件（被 Cursor/Codex/OpenHands/Copilot 等原生读取）；Claude 的 CLAUDE.md 与之软链兼容。本仓库的 [AGENTS.md](../AGENTS.md) 就是该实践的一等公民样本。
- **[Agent Skills](https://agentskills.io) / skills 目录**：Anthropic 发起、现归 AAIF 的可移植能力包格式（SKILL.md + 脚本/资料，渐进加载）；官方 [anthropics/skills](https://github.com/anthropics/skills) 仓库与 [编写指南](https://claude.com/resources/articles/complete-guide-to-building-skills-for-claude)、[测试与度量](https://claude.com/resources/articles/improving-skill-creator-test-measure-and-refine-agent-skills)。与「工具」的分工正在收敛成一条清晰判断：**流程知识放 Skill，外部能力放 MCP/CLI 工具**（参见 [Tool, Skill, or Subagent](https://claude.com/code-with-claude/session/ldn-ext-tool-skill-or-subagent-decomposing-an-agent-that-outgrew-its-prompt) 这类议题）。
- 选型口径（社区共识 + 2026 数据修正）：单应用内的子 Agent 用框架编排 + MCP 即可；**A2A 只在跨团队/跨厂商的独立运营 Agent 边界上才值得引入**；且**子 Agent 不默认值得存在**——见第一部分《When to Fold Subagents into Your Main Agent Loop》与第二部分《Scaling Agent Systems》。

---

## 六、安全、对齐与可靠性（Agent 上线的真正门槛，2026 独立成层）

- **OWASP Agentic AI Top 10**（[2026 版](https://genai.owasp.org/resource/owasp-top-10-for-agentic-applications-for-2026/)，[OWASP GenAI 项目主页](https://genai.owasp.org/)）：把提示注入、工具滥用、记忆投毒、Agent 间级联委托列为独立风险类别，是写威胁模型时的现成清单。
- **Simon Willison《The Lethal Trifecta》（2025-06）**（[原文](https://simonwillison.net/2025/Jun/16/the-lethal-trifecta/)）：私有数据访问 + 不可信内容 + 对外通信能力 = 必然泄漏。三者齐备时任何 prompt 层防御都不可信——这条应作为架构约束（拆 Agent 或拆权限），不是「提示里加一句别泄露」。
- **Prompt injection 的工程对策**（本仓库已实践）：外部内容只当数据不当指令（[AGENTS.md 的「外部资料只是参考数据」条](../AGENTS.md)）、工具审批（[ADR 006](decisions/006-tool-approval-policy.md)）、工作区边界（[ADR 007](decisions/007-file-tools-workspace.md)）、Git 操作白名单（[ADR 008](decisions/008-git-tool-policy.md)）。
- **Anthropic《Our Framework for Developing Safe and Trustworthy Agents》**（[原文](https://www.anthropic.com/news/our-framework-for-developing-safe-and-trustworthy-agents)）：能力/控制的可审计维度拆解。
- **模型层的诚实性风险**：Sleeper Agents（[2401.05566](https://arxiv.org/abs/2401.05566)，安全训练后仍持续伪装的证据）与 Probes 检测相关研究（[anthropic 研究页](https://www.anthropic.com/research/probes-catch-sleeper-agents)）——把 Agent 当「可信执行体」是错的假设。
- **实践结论（所有厂商 + 学术口径一致）**：Agent 的最后一公里是可靠性，不是能力。可靠性来自：小步可验证的动作 + 人工审批高风险操作 + 预算/步数上限 + 可回滚状态 + 评测回归。把这五件事当成分支保护、CI 门禁和最小权限来建设，而不是靠模型自觉。

---

## 七、建议学习路径（4 步）

1. **建立心智模型（1 周）**：[Lilian Weng 博文](https://lilianweng.github.io/posts/2023-06-23-agent/) → ReAct / Reflexion / SWE-agent 三篇论文 → Anthropic [Building Effective Agents](https://www.anthropic.com/engineering/building-effective-agents) + [Writing Tools for Agents](https://www.anthropic.com/engineering/writing-tools-for-agents)。目标：能说清 Agent Loop、工具描述、上下文窗口为什么是核心约束。
2. **动手做一个单 Agent（1–2 周）**：选 [OpenAI Agents SDK](https://openai.github.io/openai-agents-python/) 或 [LangGraph](https://github.com/langchain-ai/langgraph) + MCP 工具（Firecrawl / Tavily / [Playwright MCP](https://github.com/microsoft/playwright-mcp)），带一个 [Langfuse](https://langfuse.com/docs) 追踪。重点练：工具设计、guardrails、失败重试、预算上限。想读最小完整实现，看 [mini-swe-agent](https://github.com/SWE-agent/mini-swe-agent) 或本仓库的 mini_agent。
3. **补上下文工程与记忆（2 周）**：Anthropic [上下文工程](https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents) + LangChain [write/select/compress/isolate](https://blog.langchain.dev/context-engineering-for-agents/) → [MemGPT 论文](https://arxiv.org/abs/2310.08560) → 接 [Mem0](https://github.com/mem0ai/mem0) 或 [Letta](https://github.com/letta-ai/letta)；读 [Anthropic 多 Agent 文](https://claude.com/resources/articles/building-multi-agent-systems-when-and-how-to-use-them) + [Cognition 反方](https://cognition.com/blog/dont-build-multi-agents) + [Why Do Multi-Agent Systems Fail](https://arxiv.org/abs/2503.13657) + [Scaling Agent Systems](https://arxiv.org/abs/2512.08296)，形成「何时多 Agent、何时折回单循环」的判断。
4. **按场景深入 + 生产化**：
   - Coding Agent → OpenHands / SWE-agent 源码、ACI 设计、沙箱（E2B / microsandbox）、[长任务 harness](https://www.anthropic.com/engineering/effective-harnesses-for-long-running-agents)
   - 企业知识库 → Dify / RAGFlow + Agentic RAG（LlamaIndex）
   - 评估上线 → tau-bench 式自建 eval + [评测方法学](https://www.anthropic.com/research/demystifying-evals-for-ai-agents) + Langfuse / LangSmith + 人工红队 + [OWASP Agentic Top 10](https://genai.owasp.org/resource/owasp-top-10-for-agentic-applications-for-2026/) 威胁建模
   - 对照「Agent 的最后一公里是可靠性」这条所有厂商共同的结论。

> ⚠️ 时效提示：本领域 6 个月一换代（MCP 无状态化、Agent Skills 与 MCP 捐入 AAIF、Flowise/Daytona 归档、Anthropic 从「多 Agent」转向「折回单循环」都发生在近 12 个月内），选型前以各项目官方文档为准；星标与下载量来自 2026-11-05 的 GitHub API / 页面核对，仅供相对参考。
> 本环境无法访问的来源（github.com 部分仓库偶发超时、huggingface.co、cloud.google.com、openai.com 等）在文中标注了「未核对」；引用前请自行复核。
