# agent-frameworks：多智能体编排框架

## 来源与版本
- 类型：第三方仓库。
- URL 或本地路径：
  - AutoGen：https://github.com/microsoft/autogen
  - LangGraph：https://github.com/langchain-ai/langgraph
  - CrewAI：https://github.com/crewAIInc/crewAI
  - MetaGPT：https://github.com/FoundationAgents/MetaGPT
  - OpenHands：https://github.com/All-Hands-AI/OpenHands
- 固定 commit SHA / tag / 产品版本：未固定。这些仓库迭代很快，引用具体设计前按任务当天的默认分支或指定 tag 重新核对。
- 最近核对日期与阅读状态：**未读**。本文件仅登记来源与借鉴意图，尚未在线核对任何源码。
- 许可证及核实依据：均未核实（记忆中多为 MIT 系宽松许可，**不作为依据**）。复制代码前必须逐仓库核实。

## 参考目的与范围
- 借鉴什么：
  - AutoGen：多 Agent 对话与 GroupChat 的消息路由、人类参与（human-in-the-loop）暂停点设计。
  - LangGraph：用图/状态机建模 Agent 工作流；检查点（checkpoint）、中断恢复、可序列化状态的接口设计。
  - CrewAI：角色分工式任务编排（Role/Goal/Backstory → Task → Crew）的声明式建模。
  - MetaGPT：SOP（标准作业程序）驱动多角色协作，将流程写成可执行规约的思路，与本模板"需求 → 设计 → 开发 → 验证"工作流形态相近，优先精读。
  - OpenHands：事件流（event stream）+ 沙箱执行 + 动作/观察解耦的完整产品架构。
- 重点文件或章节：待读取后填写精确路径。
- 不参考什么：各框架的云部署、计费、前端界面；LangGraph 的 LangChain 生态绑定；任何依赖向量数据库的检索子系统（本模板明确不提供）。
- 何时阅读：设计 mini_agent 的编排层、消息/状态模型、可中断性时。
- 在任务中的角色：设计参考。不得越过本模板"技术栈中立、无第三方 Python 依赖、不提供自治执行器"的约束。

## 访问方式
在线读取 GitHub 仓库；如需离线精读，clone 到本地 `.references/`（只读，记录 checkout 的 commit SHA），不放入第一方源码。无法访问时说明影响，不凭记忆假装核对。

## 本地差异
本模板是 AI 工作约定与文档脚手架，不是运行时框架；上述项目均为可执行框架。差异核心：本模板不引入任何运行时依赖，编排规则以 Markdown 工作流文档表达，由外部 AI 工具遵循执行。借鉴时只取模式（状态建模、检查点语义、角色划分），不取代码与依赖。

## 实际采用情况
尚未核对，无采用结论。开始设计编排相关文档前先阅读对应来源并更新本文件。
