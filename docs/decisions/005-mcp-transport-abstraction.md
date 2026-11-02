# 005：自写 MCP 客户端，传输抽象与 stdio / Streamable HTTP 双实现

- 状态：提议（2026-11-02，待维护者评审；同日按维护者"同时支持 HTTP server + 传输抽象"要求修订，原稿仅 stdio。维护者同日已批复其中一点：**企业 OAuth 不做**，HTTP 认证维持静态 Bearer——该裁剪从"待评审默认"升格为已确认决策）。
- 日期与关联任务：2026-11-02；[008：mini-agent v2 技术方案](../design/008-mcp-tools.md)。
- 决策人或审批依据：AI 起草，整体未获人工批复；唯一已获批复点为维护者 2026-11-02 对话确认"目前不做企业 OAuth"。接受前其余内容不得视为既定方向。

## 背景

v2 需要让模型发现并调用外部工具，且维护者明确要求**同时支持基于 HTTP 的 MCP server**，并"对 JSON 协议数据的传输进行抽象，理论上任何进程间通信方式都可以承载协议数据"。MCP 规范本身将传输视为可插拔层（stdio 与 Streamable HTTP 是两个标准传输，允许自定义传输），协议消息统一为 UTF-8 JSON-RPC 2.0。约束：不无故新增依赖（ADR 002 已含 libcurl 与 nlohmann/json）、冷启动快、Linux 为主。

## 备选方案

- 方案 A（分层：RPC 内核 + Transport 抽象接口，stdio/Streamable HTTP 两个实现）：协议逻辑（initialize/版本协商/tools 三方法）只依赖"发送一条 JSON-RPC 消息/收取一条 JSON-RPC 消息"的抽象；新增传输（自定义 IPC、WebSocket 等）只加实现类，不改内核。零新依赖（HTTP 复用 libcurl）；代价是两种传输的消息时序模型不同（stdio 全双工字节流 vs HTTP 请求-响应内联返回），抽象必须容纳这一差异，接口设计要前置想清。
- 方案 B（协议代码直接耦合 stdio 管道，HTTP 另写一套）：首版实现最快；但协议逻辑重复两份，版本协商/错误语义演进时双改，与"方便扩展"的诉求相反。
- 方案 C（引入社区 C++ MCP SDK）：省实现量；候选库许可证与维护状态未核实（违背引入门槛），且其传输抽象是否覆盖本项目需求未知。
- 方案 D（只做 stdio，HTTP 需求出现后再重构）：被维护者明确否决——本期就要 HTTP。

## 决策

建议方案 A：定义 `McpTransport` 抽象（消息级双向通道：`send(json)` / `recv(timeout)` / `close()`，实现内部自行处理分帧与响应内联的映射），其上是与传输无关的 `McpRpcClient`（id 关联、串行 in-flight、超时与错误折叠），再上是对话层 `McpClient`（initialize/initialized/tools_list/tools_call）。首批两个实现：

1. **StdioTransport**：fork/exec 子进程，stdin/stdout 换行分隔 JSON（消息内不得含裸换行），stderr 仅收集为诊断。
2. **HttpTransport**：Streamable HTTP——每条消息一个 POST（`Accept: application/json, text/event-stream`）；响应为单 JSON 或 SSE 流（解析至本请求对应的 response 为止）；通知类收到 202 即成功；处理 `Mcp-Session-Id`（initialize 响应捕获、后续全部携带、404 触发重初始化）与 `MCP-Protocol-Version` 头（2025-06-18 起后续请求必须携带）；认证 v2.0 仅支持配置的静态 Bearer 头，OAuth 流程不实现（维护者 2026-11-02 批复"目前不做企业 OAuth"）。

协议面仍裁剪至 tools（不实现 resources/prompts/sampling/elicitation/服务端发起请求/GET 常开流）；客户端不发送也不依赖 JSON-RPC batching。版本基线 2025-06-18，回退 2025-03-26（stdio 细节按 03-26 文本，HTTP 会话/头规则按 06-18 文本，均 2026-11-02 在线核对）。自定义 IPC 传输不在本期交付，但抽象就位后即可作为独立小模块添加。

## 后果

- 收益：HTTP 与 stdio server 同库接入；未来任何 IPC 载体（Unix socket、共享内存环等）实现接口即可挂入；协议逻辑单点维护；仍零新增依赖。
- 代价：抽象接口需先容纳"响应内联于请求"与"字节流异步到达"两种模型（recv 允许从本地队列出队）；HTTP 传输的 SSE 解析、会话与重连状态机比 stdio 复杂，测试矩阵翻倍。
- 风险：HTTP server 的 Origin/CORS 与 DNS rebinding 面（规范安全节要求 server 侧校验，客户端侧我们要求配置 https 或显式 allow 明文 loopback）；`Mcp-Session-Id` 语义各家实现有出入，接入真实 server 需实测。
- 后续验证：stdio/HTTP 两个 mock server E2E、会话头与 404 重初始化路径、超时/连接失败折叠为 is_error（008 第 8 节计划，均未执行）。

## 关联
[008](../design/008-mcp-tools.md) 第 4、5 节；[ADR 002](002-http-json-dependency.md)（curl/json 底座）、[ADR 003](003-tui-approach.md)（自写最小层取向）、[ADR 006](006-tool-approval-policy.md)；[references/agent-protocols.md](../references/agent-protocols.md)。
