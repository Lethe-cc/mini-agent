# 011：MCP 服务端请求（sampling/elicitation）默认关闭与逐次审批

- 状态：**提议**（待人工评审；获批前 M2b 不进入实现）。
- 日期与关联任务：2026-11-22；[022：MCP M2 能力扩展](../design/022-mcp-capabilities-m2.md) §4 T4–T6、§5.2–§5.3、§6；[ADR 010](010-mcp-full-client-cxxmcp.md)（已接受）；[ADR 006](006-tool-approval-policy.md)（工具审批，语义不动）。
- 决策人或审批依据：维护者 2026-11-22 对 022 §5.6 Q2 批复"ok"（认可 T4–T6 语义方向并授权立项送审）。**该批复是对设计方向的认可，不构成本 ADR 文本的批准**；正式接受仍待维护者对本 ADR 的逐条人审。

## 背景

M2b 让 MCP server 可以反向发起两类请求：`sampling/createMessage`（借本机 LLM 配额生成文本）与 `elicitation/create`（向用户要结构化输入）。这打开一个与工具审批方向相反的信任面：**工具审批管"模型要对机器做什么"，服务端请求管"外部 server 要对用户/用户的钱包做什么"**。

硬约束（vendored cxxmcp `3c25a8afef0ae77031bcb7a3142bcca67d4cd3d6` 源码实证，022 §3）：

- SDK handler 在 transport 接收线程同步执行（stdio_transport.cpp:161 / http_transport.cpp:1405）——handler 内阻塞等待人审会冻结整条连接，stdio 下还阻塞后续消息读取。
- 现有工具审批是单固定槽协议（tui_bridge.hpp：publish→seq→answer，20ms 轮询 + 120s 超时）。工具调用与服务端请求可并发到达（server 处理 tools/call 时发起 sampling 是规范预期用法），共用固定槽会交叉死锁。
- CLI `ask` 与 TUI worker join 窗口没有审批 UI 消费方。
- MCP 规范对 sampling 有明确 human-in-the-loop 要求（客户端 SHOULD 让用户控制是否放行、可审查与修改请求内容）；elicitation 本身就是面向用户的提问。

## 备选方案

- 方案 A（本提案）：**per-server 配置字段默认 OFF**，OFF 不声明对应 capability；开启后 sampling 逐次审批（含内容预览）、elicitation 表单填写即审批；独立 ServerRequestBridge 队列；无 UI 消费方或超时一律明确拒绝。
- 方案 B（复用工具审批固定槽）：实现最省；但工具审批与服务端请求并发即交叉死锁（见背景），不可行。
- 方案 C（默认 ON + 逐次审批兜底）：省一次配置；代价是对所有已配置 server 暴露采样/表单请求面，server 可高频弹窗骚扰，且违背 021 §6"默认关"的既定方向。
- 方案 D（sampling 加"本会话允许"档位，类比工具审批的 `a`）：交互更顺；但一个恶意/失控 server 可在免问后连续采样刷爆 LLM 配额——工具审批的 `a` 作用于本机副作用，风险由用户自担；sampling 烧的是按 token 计费的配额，放大系数不同。首版不给（维护者 Q2 已认可此保守取向）。
- 方案 E（handler 内阻塞等待审批）：与 SDK 线程模型冲突，冻结连接（见背景），不可行。

## 决策（提案内容，待批）

1. **开关**：`McpServerConfig` 增可选字段 `sampling`/`elicitation`，默认 `false`（T9：不 bump schema_version）。OFF ⇒ 握手不声明对应 capability（最干净的拒绝，server 依约不发起）；ON ⇒ 声明 `sampling{}`（不含 tools/context 子能力）或 `elicitation{form:{}}`（不声明 url）。
2. **审批队列**：新增 ServerRequestBridge 独立队列（022 §5.3），与工具审批固定槽互不占用；队列上限 8，溢出直接拒绝；turn 结束 reset 排空未答请求为拒绝；McpClient 析构前先 `bridge.shutdown()` 再停 transport。
3. **sampling 逐次审批**：预览含 server 名、messages 摘要与 max_tokens；批准后才在 UI 消费侧经 `Provider::complete_chat` 执行（绝不在 transport 线程调 LLM）；拒绝/超时回 JSON-RPC error；`params.tools`/`tool_choice` 非空一律拒绝；**无"本会话允许"档位**。temperature/stop_sequences/include_context 首版忽略并在预览注明。
4. **elicitation**：仅 Form 模式，TUI 逐字段输入（primitive + enum），提交=Accept、Esc/超时=Decline；Url 模式一律 Decline；form_schema_validation 不开启；表单内容仅回给发起 server，不入会话历史。
5. **非交互语义**：CLI `ask`、TUI worker join 窗口无消费方 ⇒ sampling error / elicitation Decline（明确拒绝，不挂起；capabilities 照常声明，server 得到确定答案而非静默）。审批超时沿 120s；sampling 总等待=审批超时+LLM 调用超时。

## 后果

- 收益：默认零新攻击面（OFF 即不声明）；开启后每次采样/表单都有人在环，配额滥用与钓鱼表单受控；ADR 006 工具审批语义零改动；拒绝路径全部显式可审计。
- 代价：sampling 密集场景交互摩擦高（无 allow-session 缓解，为有意取舍）；新增一套桥协议与 UI 面板，实现与测试面扩大（死锁/超时/溢出/非交互矩阵，022 §8）。
- 风险：审批疲劳盲批——缓解：预览强制展示 messages 摘要与 max_tokens；队列上限防刷屏。残余风险：用户批准后 LLM 输出质量/内容不受本 ADR 约束（等同本机正常对话面）。
- 迁移成本：零（新字段缺省=旧行为；M2b 未实现前本 ADR 无运行时影响）。
- 后续验证（未执行，计划见 022 §8）：桥死锁/超时/溢出单测 + ASan/UBSan、pty TUI 审批流、mock LLM 对拍采样冒烟、非交互拒绝 E2E。

## 关联

[022](../design/022-mcp-capabilities-m2.md) §4 T4–T6、§5.2–§5.3、§6、§5.6 Q2；[021](../design/021-mcp-full-client.md) §6（默认关方向）；[ADR 006](006-tool-approval-policy.md)（工具审批，固定槽协议保留）；[ADR 010](010-mcp-full-client-cxxmcp.md)。