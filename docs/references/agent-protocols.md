# agent-protocols：协议与极简实现

## 来源与版本
- 类型：第三方仓库 / 官方规范。
- URL 或本地路径：
  - Model Context Protocol 规范：https://modelcontextprotocol.io
  - MCP 官方参考服务器集合：https://github.com/modelcontextprotocol/servers
  - smolagents：https://github.com/huggingface/smolagents（CodeAgent：以代码代替 JSON 工具调用）
  - MCP 官方 SDK 组（server+client 同库）：
    - https://github.com/modelcontextprotocol/typescript-sdk
    - https://github.com/modelcontextprotocol/python-sdk
    - https://github.com/modelcontextprotocol/go-sdk （与 Google 共同维护）
    - https://github.com/modelcontextprotocol/java-sdk （与 Spring AI 共同维护）
    - https://github.com/modelcontextprotocol/rust-sdk
    - https://github.com/modelcontextprotocol/csharp-sdk （与 Microsoft 共同维护）
    - https://github.com/modelcontextprotocol/swift-sdk （与 Apple 共同维护）
  - 官方调试工具：https://github.com/modelcontextprotocol/inspector （GUI/CLI 版 MCP client，连任意 server 验协议行为）
  - 第三方 C++ 实现（与本项目技术栈同族，优先对照）：
    - https://github.com/GopherSecurity/gopher-mcp （server+client；stdio/HTTP+SSE/Streamable HTTP/WebSocket/TCP 全传输）
    - https://github.com/caomengxuan666/cxxmcp （C++17、conformance-tested、streamable-http）
    - https://github.com/Neumann-Labs/mcp-cpp 、https://github.com/yurirocha15/mcp-cpp-sdk （后者 C++20+Boost.Asio，对官方 conformance suite）
  - 其他语言高星社区实现（行为参照）：
    - https://github.com/mark3labs/mcp-go （Go server+client，生态使用面广）
    - https://github.com/rust-mcp-stack/rust-mcp-sdk （Rust async server+client）
  - server 目录/发现（清单类，非代码源）：https://github.com/punkpeye/awesome-mcp-servers 、https://github.com/punkpeye/awesome-remote-mcp-servers
  - 在用远端示例：https://mcp.deepwiki.com/mcp （Streamable HTTP 公开 server，连通验证见"实际采用情况"）
- 固定 commit SHA / tag / 产品版本：未固定。MCP 规范版本化明显，引用时以任务当天的规范版本号为准。
- 最近核对日期与阅读状态：**2026-11-05 元数据+许可证核对**（GitHub 仓库元数据与 LICENSE 文件在线实读；源码仍未精读）。规范文本核对记录见"实际采用情况"。
- 许可证及核实依据：modelcontextprotocol 官方组（typescript-sdk、servers、inspector 的 LICENSE 当日实读一致）：**MIT→Apache-2.0 过渡授权**（新代码与规范贡献 Apache-2.0，文档 CC-BY-4.0；组内其余 SDK 按同政策对待，复制前仍须逐仓库确认条款细节）。GopherSecurity/gopher-mcp = **Apache-2.0**（当日实读）；caomengxuan666/cxxmcp = **MIT**（当日实读）；mark3labs/mcp-go = **MIT**（当日实读，Copyright Anthropic）。Neumann-Labs/mcp-cpp、yurirocha15/mcp-cpp-sdk、rust-mcp-stack/rust-mcp-sdk、官方组内其余 SDK、awesome 清单：**未核实**。复制代码前必须逐仓库核实。

## 参考目的与范围
- 借鉴什么：
  - MCP：工具/资源/提示的接口契约与能力协商方式，评估本模板未来是否需要以 MCP server 形式暴露工作流约定。
  - MCP client/server 开源实现（v2 自写客户端后新增的对照需求）：协议灰区行为以官方 SDK（TS/Go/Rust）与 C++ 实现为"第二意见"——尤其 Streamable HTTP 的 SSE/JSON 双响应、`Mcp-Session-Id`/`MCP-Protocol-Version` 处理、stdio 子进程生命周期、超时与断连语义；cxxmcp / yurirocha15 的 conformance 测试集可考虑作为本客户端的外部一致性验收（用官方 suite 打 mini-agent client）。server 侧样例：官方 servers 仓库的 filesystem/fetch/time 是 stdio 规范对端；deepwiki 是已在用的 Streamable HTTP 公开对端。
  - smolagents：CodeAgent 范式（Agent 直接写代码而非结构化调用）与其极简 Prompt 工程，作为"最小抽象"的对照样本。
- 重点文件或章节：待读取后填写精确路径。候选起点（按名登记，未读）：typescript-sdk `src/server`/`src/client` 传输层、mcp-go `server`/`client` 目录、cxxmcp 与 yurirocha15 的 conformance 测试目录、inspector 的 client 连接管理。
- 不参考什么：官方 SDK 的完整依赖树与构建体系（本项目约束零新增第三方依赖，只对照协议行为，不引入实现）；smolagents 的 Hub 集成与模型托管；各实现的 GUI/框架胶水层。
- 何时阅读：设计工具接口契约、考虑与外部 AI 工具（VS Code Copilot、Claude Code 等）互通时；v2 后续项（HTTP 每 server 超时、服务器发起 GET 流、progress/取消通知、OAuth）动工前，先对照官方 SDK 与 C++ 实现的对应处理再定设计。
- 在任务中的角色：协议事实核对 + 设计参考。任何"已符合 MCP 规范"的表述必须以当天官方文本为准，不得凭记忆断言。

## 访问方式
在线读取规范站点与 GitHub 仓库（元数据与 LICENSE 经 GitHub API/文件直读）。规范文档使用版本化 URL；无法访问时说明影响。离线精读某仓库时 clone 到 `.references/` 并记录 commit SHA。

## 本地差异
本模板不绑定任何协议；`AGENTS.md`/docs 体系面向"AI 工具读取 Markdown"这一最低公分母。接入 MCP 属于架构变化，需先更新 `docs/architecture.md` 并走设计流程。

## 实际采用情况
2026-11-02 为 v2 设计（[008](../design/008-mcp-tools.md)）在线核对官方版本化规范（modelcontextprotocol.io，specification/2025-06-18 的 Key Changes/Lifecycle、specification/2025-03-26 的 Transports/Tools），未读 SDK 源码、未 clone servers 仓库：

- 采用：stdio 传输（换行分隔 JSON-RPC、stdout 仅协议消息、stderr 可自由日志）、initialize/initialized 生命周期与版本协商、tools/list、tools/call、结果 content[]+isError、"客户端 MUST 视 annotations 为不可信"与 human-in-the-loop WARNING（转化为 ADR 006 默认 ask 策略）。同日二次核对 2025-06-18 Transports 的 Streamable HTTP 细则并纳入采用：每消息一个 POST（Accept 双类型）、响应 JSON 或 SSE 流二选一、通知收 202、`Mcp-Session-Id` 会话管理（捕获/携带/404 重初始化/DELETE 注销）、`MCP-Protocol-Version` 头（06-18 起后续请求 MUST）。
- 不采用（v2 裁剪）：HTTP 侧服务器发起的 GET 常开流与 resumability/Last-Event-ID、OAuth 发现流程（仅静态 Bearer）、resources、prompts、sampling、elicitation、roots、progress；不使用 JSON-RPC batching（2025-06-18 已移除，禁用后两版本通用）。
- 版本基线：2025-06-18，initialize 支持回退 2025-03-26。规范继续版本化演进，实现前按当天文本复核。
- servers 参考仓库与 smolagents 源码仍未精读；2026-11-05 完成开源实现组的元数据+许可证登记（见上）。本仓库未复制任何第三方代码，客户端全部按协议文本自写（ADR 005）。
- 开源实现参考的定位（2026-11-05）：① 规范灰区（超时、SSE 断流重连、会话过期重初始化各家差异）出现争议行为时的第二意见；② conformance 测试集可作本客户端的外部一致性验收工具（cxxmcp/yurirocha15 声明对官方 suite）；③ 未来引入服务器发起请求/progress/OAuth 等能力时的工程形态参照。引入任何代码前逐仓库二次核实许可证细节（官方组 MIT→Apache 过渡含贡献时点区分）并记 ADR；Apache-2.0 注意专利授权条款与 NOTICE 义务。
- 真实对端记录：deepwiki（https://mcp.deepwiki.com/mcp，Streamable HTTP、无鉴权）于 2026-11-04/05 由 mini-agent 客户端连通并全链路调用（tools/list 4 工具、tools/call 成功；其行为为 SSE 单响应、无进度流，与本项目实现兼容）。属"外部 API 事实"，非设计参考。
