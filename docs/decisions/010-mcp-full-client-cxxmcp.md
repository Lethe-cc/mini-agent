# 010：MCP 全能力客户端路线——引入 cxxmcp（已接受，取代 ADR 005）

- 状态：已接受（2026-11-21，维护者批复 021 Q1"批准"，含本 ADR 与 ADR 002 修订）。
- 日期与关联任务：2026-11-21；[021：MCP 全能力客户端迁移设计](../design/021-mcp-full-client.md)。
- 决策人或审批依据：方向输入为维护者 2026-11-21 对话"未来要 resources/prompts/sampling/elicitation/tasks/追新版本"；依赖引入授权为维护者同日对 [021 Q1](../design/021-mcp-full-client.md) 的批复"批准"（真实记录，同批 Q2"延续，但是未来可能会拓展"、Q3/Q4 按建议默认）。生效前提 1、2、3 均已满足（前提 3 核实于 M0、归档于 M1 阶段 A `9f344c3`，见下）。M1 已实现并全门禁验收，默认后端已翻转 cxxmcp（2026-11-22，[021 §8](../design/021-mcp-full-client.md)）。

## 背景

维护者确认 MCP 将从 tools 外设成长为全能力核心：resources、prompts、sampling、elicitation、tasks、跟进协议新版本。这推翻 [ADR 005](005-mcp-transport-abstraction.md) 的两个前提：

1. **tools-only 裁剪不再成立**。sampling/elicitation 是服务端发起的请求，现有消息级 `recv(timeout)` 抽象（[mcp.hpp](../../include/mini_agent/mcp.hpp)）没有服务端主动请求的入口；tasks（SEP-2663）需要长任务状态机。自写路线下每项能力都要动抽象本身。
2. **协议版本钉死不再成立**。现钉 2025-06-18（回退 2025-03-26）；"追新版本"要求持续跟进协商与 wire 演进，自写等于每次演进重新设计+自证。

约束不变的部分：C++17 基线（CMake `cxx_std_17`）、Linux 为主、冷启动预算、单线程 poll 循环 + TurnBridge 的并发形态、[ADR 006](006-tool-approval-policy.md) 审批边界（发生在工具层，不随 MCP 底座变化）。

## 备选方案

- 方案 A（保留自写，扩展抽象至全能力）：零新依赖、1274 行全量可审计；但 recv() 模型需重设计为请求-响应+通知双通道，resources/prompts/sampling/elicitation/tasks 逐项实现，版本演进逐项跟进，正确性锚定仍靠自写 mock（共享同一理解偏差）。能力面越大，该路线的边际成本越高。
- 方案 B（cxxmcp，caomengxuan666/cxxmcp，MIT）：C++17（默认标准即 17，与基线一致）；typed DTO + `core::Result<T>` 错误契约；Transport 契约在请求-响应层（`send(JsonRpcRequest)->Result<JsonRpcResponse>` + notification/server-request 双通道），比本项目消息级 recv() 更忠实于 MCP 语义；分页内建（discover_all_tools）；协议协商范围 2024-11-05 → 2026-07-28 + DRAFT；tasks SEP-2663、stateless wire SEP-2575 已实现；自述官方 conformance client 501/501（自报数据，未独立复跑）。依赖闭包：vendored header-only tl::expected + nlohmann/json（与本项目同库）；HTTP 路径需 cpp-httplib（`CXXMCP_ENABLE_HTTP=ON`）+ OpenSSL（TLS）。风险：pre-1.0（RC 形态）、单一维护者、34★ 社区规模小；HTTP 路径与 libcurl 形成双 HTTP/TLS 栈。
- 方案 C（其余社区候选）：Neumann-Labs/mcp-cpp（C++20，65★，0.1 beta，无 conformance 声明）——传输三层与 ADR 005 同构，可作第二意见参考；yurirocha15/mcp-cpp-sdk（C++20 + Boost.Asio/Beast + OpenSSL + jsoncons，1★）——依赖最重、社区最小，但 CI 跑官方 conformance 且公开缺口文件，工程诚实度高；GopherSecurity/gopher-mcp（C++14 + OpenSSL REQUIRED，156★，95 open issues，全传输 + 6 语言绑定）——最重，超需求。C++20 两项与本项目 C++17 基线冲突（升基线是另一项独立决策，不在本 ADR 内做）。
- 方案 D（官方 C++ SDK）：截至 2026-11-21 官方 SDK 组无 C++ 实现（TS/Python/Go/Java/Rust/C#/Swift），不可选。

以上候选均于 2026-11-21 经 GitHub 元数据 + README/LICENSE/DEPENDENCY_POLICY/CMake 层核对（cxxmcp 另实读 client/http_transport.hpp、client/session.hpp 头文件；源码未精读；conformance 数字均为项目自述）。登记见 [references/agent-protocols.md](../references/agent-protocols.md)。

## 决策

建议方案 B：引入 cxxmcp 作为 MCP 客户端底座，取代自写 [src/mcp/](../../src/mcp) 三层；工具注册/审批/上下文注入层（[tools.hpp](../../include/mini_agent/tools.hpp)、ADR 006）不动。分期与门禁见 [021 设计](../design/021-mcp-full-client.md)：spike 验证先行（四 preset 编译、现有双 mock + deepwiki 重放、独立复跑官方 conformance、poll 循环桥接确认），spike 不达标则回到方案 A，不进入迁移。

**生效前提（缺一不可）**：

1. ~~维护者显式批准本 ADR（依赖引入授权）。~~ **已满足**：维护者 2026-11-21 批复 Q1"批准"。
2. ~~ADR 002 修订获批~~ **已满足**：同批批复覆盖 ADR 002 修订（MCP 子系统豁免"零新增依赖"取向，登记 cxxmcp（vendored，固定 tag/SHA + LICENSE 归档）+ cpp-httplib（vendored header-only）+ OpenSSL（系统库，新增运行期依赖点；LLM 传输仍走 libcurl，双栈共存为已知代价）），修订记录见 [ADR 002](002-http-json-dependency.md)。
3. 引入前逐仓库二次核实许可证与 vendored 组件署名义务（tl::expected、nlohmann/json、cpp-httplib 各自 LICENSE），沿用 ADR 002 的 vendoring 纪律。**核实已完成**（M0 spike，2026-11-21，`.references/` 内逐文件实读：cxxmcp MIT、cpp-httplib MIT、nlohmann/json MIT、tl::expected CC0，见 [021 §8](../design/021-mcp-full-client.md)）；**归档已完成**（M1 阶段 A `9f344c3`，2026-11-22：LICENSE 全文 + 固定 SHA + 裁剪清单落入 [third_party/cxxmcp/README](../../third_party/cxxmcp/README.md)，56 文件逐字节比对上游一致）。前提 3 关闭。

**边界（2026-11-21 批复随附）**：M0 spike 是否放行迁移由 spike 结果决定——四 preset 编译、mock/deepwiki 重放、conformance 独立复跑、并发桥接任一不达标即回到方案 A；OAuth/auth 目标保持 OFF（Q2"延续，但是未来可能会拓展"——延续 2026-11-02 裁剪，未来启用须另行人审设计，不在本次授权内）。

## 后果

- 收益：六项能力从"逐项设计+自证"变为"启用+适配+外部一致性验收"；协议演进由上游承担、官方 conformance 锚定；错误契约与分页等灰区行为不再自造。
- 代价：ADR 002 零新增依赖取向被打破（显式豁免 + ADR 记录）；OpenSSL 成为新运行期系统依赖；双 HTTP/TLS 栈（libcurl 供 LLM、cpp-httplib+OpenSSL 供 MCP）——统一栈是后续独立议题，不在本次范围。
- 风险与缓解：pre-1.0 API 变动 → vendored 固定 SHA，升级走独立评审；单一维护者 → 保留 ADR 005 时代自写实现的 git 历史作回退底座；可审计性弱化（ADR 006 的审批安全论证部分依赖 1274 行全量可知）→ client-only 构建（server/auth 目标默认 OFF 保持 OFF）、不启用不需要的能力目标，未用路径不进二进制；conformance 自报 → spike 内独立复跑，不达标即否决。
- 迁移成本与验证：见 021 §5/§8（分 M0 spike → M1 底座替换 → M2 能力扩展三期，每期过既有门禁）。

## 关联

[021 设计](../design/021-mcp-full-client.md)；取代 [ADR 005](005-mcp-transport-abstraction.md)（其 tools-only/版本钉死前提已被方向输入推翻；OAuth 裁剪批复延续，见 021 Q2 批复记录）；修订 [ADR 002](002-http-json-dependency.md)（2026-11-21 修订已批）；[ADR 003](003-tui-approach.md)（自写最小层取向的既有豁免先例：md4c，ADR 004）；[ADR 006](006-tool-approval-policy.md)（不受影响，审批在工具层）；[references/agent-protocols.md](../references/agent-protocols.md)。