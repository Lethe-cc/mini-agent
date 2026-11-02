# agent-protocols：协议与极简实现

## 来源与版本
- 类型：第三方仓库 / 官方规范。
- URL 或本地路径：
  - Model Context Protocol 规范：https://modelcontextprotocol.io
  - MCP 参考实现与服务器集合：https://github.com/modelcontextprotocol/servers
  - smolagents：https://github.com/huggingface/smolagents（CodeAgent：以代码代替 JSON 工具调用）
- 固定 commit SHA / tag / 产品版本：未固定。MCP 规范版本化明显，引用时以任务当天的规范版本号为准。
- 最近核对日期与阅读状态：**未读**。仅登记来源与借鉴意图。
- 许可证及核实依据：均未核实。复制代码前必须逐仓库核实。

## 参考目的与范围
- 借鉴什么：
  - MCP：工具/资源/提示的接口契约与能力协商方式，评估本模板未来是否需要以 MCP server 形式暴露工作流约定。
  - smolagents：CodeAgent 范式（Agent 直接写代码而非结构化调用）与其极简 Prompt 工程，作为"最小抽象"的对照样本。
- 重点文件或章节：待读取后填写精确路径。
- 不参考什么：MCP 各语言的官方 SDK 实现细节（除非确定要接入）；smolagents 的 Hub 集成与模型托管。
- 何时阅读：设计工具接口契约、考虑与外部 AI 工具（VS Code Copilot、Claude Code 等）互通时。
- 在任务中的角色：协议事实核对 + 设计参考。任何"已符合 MCP 规范"的表述必须以当天官方文本为准，不得凭记忆断言。

## 访问方式
在线读取规范站点与 GitHub 仓库。规范文档使用版本化 URL；无法访问时说明影响。

## 本地差异
本模板不绑定任何协议；`AGENTS.md`/docs 体系面向"AI 工具读取 Markdown"这一最低公分母。接入 MCP 属于架构变化，需先更新 `docs/architecture.md` 并走设计流程。

## 实际采用情况
2026-11-02 为 v2 设计（[008](../design/008-mcp-tools.md)）在线核对官方版本化规范（modelcontextprotocol.io，specification/2025-06-18 的 Key Changes/Lifecycle、specification/2025-03-26 的 Transports/Tools），未读 SDK 源码、未 clone servers 仓库：

- 采用：stdio 传输（换行分隔 JSON-RPC、stdout 仅协议消息、stderr 可自由日志）、initialize/initialized 生命周期与版本协商、tools/list、tools/call、结果 content[]+isError、"客户端 MUST 视 annotations 为不可信"与 human-in-the-loop WARNING（转化为 ADR 006 默认 ask 策略）。同日二次核对 2025-06-18 Transports 的 Streamable HTTP 细则并纳入采用：每消息一个 POST（Accept 双类型）、响应 JSON 或 SSE 流二选一、通知收 202、`Mcp-Session-Id` 会话管理（捕获/携带/404 重初始化/DELETE 注销）、`MCP-Protocol-Version` 头（06-18 起后续请求 MUST）。
- 不采用（v2 裁剪）：HTTP 侧服务器发起的 GET 常开流与 resumability/Last-Event-ID、OAuth 发现流程（仅静态 Bearer）、resources、prompts、sampling、elicitation、roots、progress；不使用 JSON-RPC batching（2025-06-18 已移除，禁用后两版本通用）。
- 版本基线：2025-06-18，initialize 支持回退 2025-03-26。规范继续版本化演进，实现前按当天文本复核。
- servers 参考仓库与 smolagents 仍未核对，无许可证结论；本仓库未复制任何第三方代码，仅按协议文本自写。
