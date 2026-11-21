# mini-agent 技术方案 021：MCP 全能力客户端（cxxmcp 迁移）

## 文档状态

- 状态：**已接受（方向与开放问题已批）/ M0 spike 已通过（结果待人工确认放行 M1）**（2026-11-21）。**未实现、未向 third_party/ 引入任何第三方代码**；不改变任何现有行为。
- 关联任务：维护者 2026-11-21 方向输入"未来要 resources/prompts/sampling/elicitation/tasks/追新版本"；[ADR 010](../decisions/010-mcp-full-client-cxxmcp.md)（已接受，取代 ADR 005）。
- 核对日期、适用版本或代码修订：2026-11-21。本仓库基线 main `934f445`（Q1–Q4 批复落档）；cxxmcp master `3c25a8afef0ae77031bcb7a3142bcca67d4cd3d6`（M0 spike 中另选读 client.cpp/http_transport.cpp/request.hpp 及 conformance harness；全库仍未精读）。
- 人工评审记录：维护者 2026-11-21 批复——Q1"批准"（ADR 010 + ADR 002 修订，依赖引入授权）；Q2"延续，但是未来可能会拓展"（OAuth 裁剪延续，auth 保持 OFF；未来启用须另行人审设计）；Q3"ok"（M1 期钉 2025-06-18，M2 再上移）；Q4"ok"（client 子集裁剪 vendoring，M0 确认依赖闭包后定裁剪清单）。整体设计文档的逐节人工评审仍未做；M0 spike 已执行并判定通过（§8），**M1 vendoring 放行待维护者确认**。

## 1. 背景与目标

现有 MCP 客户端为自写三层（[mcp.hpp](../../include/mini_agent/mcp.hpp)：`McpTransport` 消息级抽象 → `McpRpcClient` → `McpClient`；[src/mcp/](../../src/mcp) 共 1274 行），按 [ADR 005](../decisions/005-mcp-transport-abstraction.md) 裁剪至 tools 面、版本钉 2025-06-18（回退 2025-03-26）。维护者确认 MCP 将成长为全能力核心：resources、prompts、sampling、elicitation、tasks、跟进协议新版本。

问题：消息级 `recv(timeout)` 抽象没有服务端发起请求的入口（sampling/elicitation 必需）；tasks 需要长任务状态机；每项能力与每次协议演进在自写路线下都要重动抽象并自证正确性。纯方案对比结论（2026-11-21 会话分析，落档于 ADR 010 备选节）：能力面长大的前提下，cxxmcp 的请求-响应级 Transport 契约、typed DTO + `Result<T>`、内建分页、2024-11-05→2026-07-28 协商范围与官方 conformance 锚定在设计上占优。

成功标准：迁移后现有 tools 行为逐项保持（双 mock E2E + deepwiki 实测重放全绿）；六项能力分期可用；协议版本可随上游演进；ADR 006 审批语义零变化。

## 2. 范围与非目标

范围：MCP 客户端底座替换 + 能力分期扩展 + 依赖登记与 vendoring + 测试基线升级。

非目标：
- 不做 MCP server（mini-agent 仍以 client 身份接入外部 server）。
- 不统一 HTTP/TLS 栈（LLM 传输保持 libcurl；双栈共存为已接受代价，统一是后续独立议题）。
- 不升 C++ 基线（cxxmcp 默认 C++17，与现基线一致）。
- 不动工具审批策略（ADR 006）、工具注册与上下文注入层（tools.hpp 消费方接口保持）。
- 不动 TUI/CLI 并发形态（单线程 poll 循环 + TurnBridge）。
- OAuth：现状静态 Bearer 不变；cxxmcp 的 OAuth/DPoP 目标默认 OFF 保持 OFF（原 OAuth 裁剪批复是否延续见 Q2）。

## 3. 约束与参考资料

项目约束：ADR 002 零新增依赖取向（本设计的前提是 ADR 010 + ADR 002 修订获批，未批不得引入）；C++17；Linux 为主；冷启动预算（现 ~5ms 中位数）；vendoring 纪律（固定 tag/SHA + LICENSE 归档，沿用 ADR 002/004 先例）。

外部事实（2026-11-21 核对，登记见 [references/agent-protocols.md](../references/agent-protocols.md)）：
- cxxmcp（caomengxuan666/cxxmcp，MIT 实读）：`CXXMCP_SDK_CXX_STANDARD=17` 默认；CMake 目标 protocol/transport/handler/peer/service/client/server/auth/sdk，client/server 默认 OFF；`CXXMCP_ENABLE_HTTP=OFF` 默认（HTTP 路径须 ON，携 cpp-httplib）；`CXXMCP_ENABLE_OPENSSL=OFF` 默认（TLS 须 ON）；`CXXMCP_ENABLE_AUTH=OFF` 默认；vendored header-only tl::expected + nlohmann/json；`CXXMCP_USE_SYSTEM_DEPS` 可选。自述官方 conformance server 272/272、client 501/501（**自报，未独立复跑**）。
- 客户端 API（头文件实读）：`McpClientSession(unique_ptr<Transport>, options)`，`initialize()/mark_initialized()`、`discover_tools()/discover_all_tools()`（分页内建）、`call_tool(ToolCall)->Result<ToolResult>`、prompts/resources/templates 发现、`complete`、`subscribe`；Transport 契约 `send(JsonRpcRequest)->Result<JsonRpcResponse>` + `send_notification` + `start(request_handler, notification_handler)` + `stop()`；`HttpTransportOptions` 含 extra headers、bearer、401 刷新钩子、stateless、超时（默认 30s）、`set_negotiated_protocol_version()`。
- 其余候选（mcp-cpp/mcp-cpp-sdk/gopher-mcp）与官方无 C++ SDK 的结论见 ADR 010 备选节。

本地差异：cxxmcp 是通用 SDK（server+client+auth 全家桶），本项目只取 client 路径；其线程模型（transport 内部接收线程）与本项目单线程 poll 循环需桥接（§5.3）。

## 4. 方案与关键取舍

| 取舍 | 选定方案 | 主要备选 | 理由 | ADR |
| --- | --- | --- | --- | --- |
| 客户端底座 | 引入 cxxmcp（client-only），取代自写三层 | 保留自写并扩展抽象；其余社区候选 | 能力面长大 ⇒ 请求-响应契约/typed/分页/版本演进/外部 conformance 锚定结构性占优；其余候选或 C++20 冲突或依赖更重 | [010](../decisions/010-mcp-full-client-cxxmcp.md)（提议） |
| 依赖形态 | vendored 固定 SHA（third_party/cxxmcp/），OpenSSL 走系统库 | FetchContent 网络拉取；包管理器 | 离线可构建、审计面固定，沿用 nlohmann/md4c 先例；FetchContent 破坏离线约束 | ADR 002 修订（待批） |
| HTTP/TLS | 双栈共存：LLM=libcurl，MCP=cpp-httplib+OpenSSL | 统一到 OpenSSL 自写/curl 适配 cxxmcp Transport | 统一栈动 LLM 主路径，风险不成比例；cxxmcp 有 transport_adapter 桥接口，未来自写 curl Transport 是可行的收敛路径（非本期） | ADR 010 后果节 |
| 能力分期 | M2 内 resources/prompts 先，sampling/elicitation 次，tasks 后 | 一次全开 | resources/prompts 是纯客户端请求，风险最低；sampling/elicitation 引入服务端发起请求，需审批与 UI 设计；tasks 需长任务状态机与 UI | — |
| 回退底座 | 保留自写实现的 git 历史 + 迁移期双轨开关 | 直接删除自写代码 | pre-1.0 上游风险的对冲；M1 验证全绿前不删 | — |

## 5. 详细设计（规划，未实现）

### 5.1 分期

- **M0 spike（引入前的否决门）**：`.references/` clone cxxmcp 固定 SHA；四 preset（GCC/Clang × Debug/Release）编译 client-only + HTTP + OpenSSL 组合；用其客户端重放现有 stdio/HTTP mock 场景与 deepwiki 实测；**独立复跑官方 conformance runner client suite**，与自报 501/501 对照；确认 §5.3 桥接可行。任一不达标 ⇒ 本设计废弃，回 ADR 010 方案 A，vendored 代码不进 third_party/。
- **M1 底座替换（行为保持）**：third_party/cxxmcp/ vendoring（含 tl::expected/cpp-httplib，LICENSE/SHA 归档）；新 `McpClient` 门面内部改用 `McpClientSession`，对 tools 层保持现接口（工具发现/调用/错误折叠为 is_error/诊断尾）；`MINI_AGENT_MCP_BACKEND=legacy|cxxmcp` 双轨开关，默认 legacy 直至 M1 验收全绿后翻转；现有 mock E2E、mcp_probe、deepwiki、pty/CLI 门禁全部重跑。
- **M2 能力扩展**：resources/prompts（发现 + 读取/获取，UI 注入形态届时细化设计）→ sampling/elicitation（服务端发起请求的审批与 UI，须过 ADR 006 同款人审设计）→ tasks（长任务状态机，参考 [020 状态机文档](020-state-machines.md) 增补）；版本基线随 cxxmcp 协商能力上移，测试 mock 同步升级。
- **M3 收尾**：默认翻转后移除 legacy 路径与自写三层（git 历史保留）；文档/架构总览同步。

### 5.2 依赖与构建

- vendored：cxxmcp 源码树按 client 所需子集裁剪（protocol/transport/handler/peer/service/client + vendored tl/json；server/auth 不入库），裁剪清单与上游 SHA 记录于 third_party/cxxmcp/README；cpp-httplib 单头随 cxxmcp 的 vendoring 政策取用。
- CMake：`find_package(OpenSSL REQUIRED)` 新增系统依赖点；cxxmcp 以 add_subdirectory 或裁剪后直接编入，`CXXMCP_ENABLE_HTTP=ON`、`CXXMCP_ENABLE_OPENSSL=ON`、`CXXMCP_ENABLE_AUTH=OFF`、server 目标 OFF；clang-tidy -Werror 对 third_party 豁免（沿用 nlohmann/md4c 先例）。
- 许可证归档：cxxmcp MIT、tl::expected、nlohmann/json MIT、cpp-httplib MIT——引入前逐一实读 LICENSE 并记录（ADR 010 生效前提 3）。

### 5.3 并发桥接

cxxmcp transport 内部有接收线程（HTTP SSE/stdio 读）；本项目工具调用发生在 turn worker 线程（TurnBridge 桥回 UI）。桥接方案：MCP 调用保持在 turn worker 内同步发起（`call_tool` 本身是同步 Result 返回），transport 接收线程只服务其内部队列，不触碰 Chat/TUI 状态；sampling/elicitation 的 server-request handler 在 M2 设计时经 TurnBridge 同款队列转 UI 审批，禁止在 transport 线程直接弹审批。M0 spike 须以 ASan/TSan（如可用）验证无数据竞争。

### 5.4 接口与数据

- tools 层消费接口不变（ToolRegistry 注册、schema 注入、call → content[]/isError）。
- config：`mcp_servers` 现有字段保持；新增字段（如 per-server 能力开关）在 M2 按需设计，schema 是否 bump 届时定。
- session：tools 消息格式不变；resources/prompts 注入形态属 M2 设计面。

## 6. 异常与安全设计

- 错误契约迁移：自写 bool + string* 出参 → `core::Result<T>`，门面层折叠回现有 is_error 语义，模型可见行为不变。
- 攻击面控制：client-only 裁剪，server/auth 路径不编译；未启用能力不进二进制——对冲"SDK 未用路径 = 解析攻击面"对 ADR 006 可审计性论证的弱化。
- 审批边界不变：工具调用审批仍在工具层（ADR 006）；sampling/elicitation 引入新的服务端发起面，M2 必须设计人审策略后才启用（默认关闭）。
- 传输安全：https 或显式 allow 明文 loopback 的现有约束保留；OpenSSL 证书校验默认开启。
- 供应链：固定 SHA + LICENSE 归档 + 升级走独立评审；上游 pre-1.0 破坏性变更按 vendored 快照隔离。

## 7. 兼容性与发布

- 双轨开关期 legacy 默认，行为零变化；翻转后 legacy 保留一个版本周期再删（M3）。
- 回滚：开关拨回 legacy 即回自写路径（M3 前）；M3 后回滚 = revert 提交。
- 可观测性：diagnostics_tail 语义保留（cxxmcp 诊断信息映射进现有日志红线，不含密钥/正文）。
- 冷启动：OpenSSL 初始化成本纳入 cold_start_check 复测；超预算则延迟初始化 MCP 子系统。

## 8. 验证情况

M0 spike 环境（2026-11-21，Debian Linux）：GCC 14.2.0 / Clang 19.1.7，CMake 3.31.6，OpenSSL 3.5.7；cxxmcp master `3c25a8afef0ae77031bcb7a3142bcca67d4cd3d6`（`.references/cxxmcp/`，codeload tarball，git clone 被网络阻断）；官方 conformance runner v0.2.0-alpha.12（`.references/conformance/`，需本地 Node ≥22：`.references/node22/`，系统 node 20 缺 `fs.globSync`）。spike 产物全部位于 gitignored `.references/`，未入仓库质量门禁。

| 验收项 | 测试位置或证据 | 实际执行命令 | 结果 | 限制 |
| --- | --- | --- | --- | --- |
| M0：四 preset 编译 cxxmcp client-only | `.references/spike-build/{gcc,clang}-{debug,release}/CMakeCache.txt` | `cmake -S .references/cxxmcp -B .references/spike-build/<preset> -DCMAKE_BUILD_TYPE=<Debug\|Release> -DCMAKE_CXX_COMPILER=<g++\|clang++> -DCXXMCP_BUILD_CLIENT=ON -DCXXMCP_BUILD_SERVER=OFF -DCXXMCP_ENABLE_HTTP=ON -DCXXMCP_ENABLE_OPENSSL=ON -DCXXMCP_ENABLE_AUTH=OFF && cmake --build <preset> -j` | **通过**：四 preset 全部编译成功 | client-only + HTTP + OpenSSL 组合；server/auth/examples/tests OFF |
| M0：stdio mock 重放 | `.references/spike-harness/spike_client.cpp`（build-gcc） | `spike_client stdio python3 tests/mock_mcp_server.py <tmp>` | **通过**：8/8 PASS，协商 2025-06-18 | 现有 stdio mock 场景经 cxxmcp `McpClientSession` 重放 |
| M0：HTTP mock 重放 | 同上；`tests/mock_mcp_http_server.py`（端口 8933） | `spike_client http http://127.0.0.1:8933/mcp` | **通过**：ALL PASS，含 -32601 ping 传播、DELETE→重握手恢复，协商 2025-06-18 | mock 为现有测试资产，非新增 |
| M0：deepwiki 实测重放 | 同上 | `spike_client deepwiki https://mcp.deepwiki.com/mcp` | **通过**：ALL PASS（3 tools 发现 + 调用），协商 2025-06-18 | 真实外部端点，结果受网络时点影响 |
| M0：并发桥接确认（§5.3） | 同上；build-asan（ASan+LSan）/ build-tsan（TSan） | `spike_client threads <url> 25`（4 worker 线程 × 25 次同步 call_tool）；stdio/http/deepwiki 三场景分别在 ASan、TSan 下重放 | **通过**：100/100 调用成功；ASan+LSan 干净 ×3；TSan 干净 ×3，无数据竞争/泄漏报告 | 4 线程同步调用模型 = turn worker 形态；sampling 的 server-request 桥接属 M2，未验 |
| M0：官方 conformance client suite 独立复跑（最新协商 wire） | `.references/conf-results/`（42 场景 checks.json） | `node dist/index.js client --command <everything_client> --suite all --timeout 30000 -o .references/conf-results`（everything_client 以 AUTH=ON+OpenSSL GCC Release 单独构建） | **42/48 场景完成：463 SUCCESS / 0 FAILURE / 4 SKIPPED（sep-2640 skills，按设计跳过）/ 567 INFO（不计分）**；其余 6 个 draft-auth 场景挂起（见 §9 已知限制） | runner 比上游 2026-09-15 快照新（48 场景 vs 当时 501 checks 口径），总数不可直接对照 501/501；6 场景挂起为上游 SDK 析构缺陷，非 wire 一致性失败 |
| M0：官方 conformance 钉 2025-11-25（上游同口径复现） | `.references/conf-results-20251125/`（19 场景） | 同上追加 `--spec-version 2025-11-25` | **通过**：247 passed / 0 failed / 0 warnings，runner 退出码 0——与上游 CONFORMANCE_STATUS.md 记载的 247/0 基线**逐项精确一致** | 这是与自报数字的严格同口径对照；最新 wire 的 463/0 为超集佐证 |
| M0：许可证核实（ADR 010 前提 3） | `.references/cxxmcp/LICENSE`、vendored tl/json LICENSE、cpp-httplib LICENSE 实读 | 逐文件人工（AI）实读比对 | **通过**：cxxmcp MIT、cpp-httplib MIT、nlohmann/json MIT、tl::expected CC0，均兼容 | M1 vendoring 时须将 LICENSE 全文 + SHA 归档进 third_party/（义务未完成，仅核实完成） |
| M1：现有全门禁重跑（四 preset CTest、pty、CLI、tidy、ASan、strict-project） | 既有测试资产 | 未执行 | 未执行 | M1 验收项 |
| M2：各能力验收 | 届时设计 | 未执行 | 未执行 | — |

**M0 判定：通过**。§5.1 四项否决门（四 preset 编译、mock/deepwiki 重放、conformance 独立复跑对照、并发桥接 ASan/TSan）全部达标；conformance 与自报基线在同口径（2025-11-25）下精确复现 247/0，最新 wire 超集 0 FAILURE。唯一未竟事项（6 个 draft-auth 场景挂起）已根因定位为上游 `Client::~Client()` 异常安全缺陷（见 §9），其触发条件（draft stateless wire + auth ON）在 M1 授权范围之外（Q2 auth OFF、Q3 钉 2025-06-18），不构成否决；作为已知限制记录并建议向上游报告。是否放行 M1 vendoring 由维护者确认。

## 9. 实现偏差与已知限制

未实现，无偏差。开放问题批复记录（维护者 2026-11-21）：

- **Q1（依赖批准）——已批**："批准"。ADR 010 已接受、ADR 002 修订已批（MCP 子系统豁免登记 cxxmcp/cpp-httplib/OpenSSL）。引入仍受两道门约束：M0 spike 通过 + 引入前逐仓库许可证核实（ADR 010 前提 3）。
- **Q2（OAuth 裁剪存续）——延续，保留拓展空间**："延续，但是未来可能会拓展"。auth 目标保持 OFF；未来启用 OAuth/DPoP 须另行人审设计（新 ADR 或本设计增补），不在本次授权内。
- **Q3（协议基线）——按建议**："ok"。M1 行为保持期钉 2025-06-18（回退 2025-03-26 语义按 cxxmcp 协商实测确认），M2 再上移基线。
- **Q4（裁剪入库形态）——按建议**："ok"。client 子集裁剪 vendoring；具体裁剪清单在 M0 确认依赖闭包后于 third_party/cxxmcp/README 记录。

已知限制：

- **上游析构挂起缺陷（M0 发现，gdb 实证）**：draft 2026-07-28 stateless wire 下，auth 拒绝类场景（`auth/resource-mismatch`、`auth/iss-supported-missing`、`auth/iss-wrong-issuer`、`auth/iss-unexpected`、`auth/iss-normalized`、`auth/metadata-issuer-mismatch` 共 6 个）中客户端进程无法退出。根因链：`Client::request_async`（sdk/client/src/client.cpp:1822）对 `in_flight_requests_` 的递减写在 worker lambda 内、**非 RAII 保护**；conformance harness 的 auth 回调在 transport 内部调用 `[[noreturn]] fail()` 抛异常，越过递减点；`RequestHandle::spawn` 的 catch 把异常折叠为错误结果，但计数残留为 1；异常展开触发 `Client::~Client()`（client.cpp:2164）无限等待 `in_flight_requests_==0`。同场景在 2025-11-25 wire 下走同步 `initialize()` 主线程路径，<1s 干净退出且拒绝行为正确（server 侧 check `resource-mismatch-rejected: SUCCESS` 已捕获）；6 场景单独重跑均复现挂起（exit 124），非负载偶发。gdb 全线程回溯存证（`.references/conf-diag2/client-destructor-hang-gdb-bt.txt`）。**对 M1 无影响**：触发需 auth ON + draft stateless wire，两者均不在 M1 授权内（Q2/Q3）；M2 上移版本基线前须确认上游修复或本地规避（如 RAII 计数守卫），建议向上游报告。
- conformance 复跑口径漂移：本地 runner v0.2.0-alpha.12 场景数（48）多于上游 2026-09-15 自报快照，`--suite all` 总数不可直接对照 501/501；已用 `--spec-version 2025-11-25` 同口径精确复现 247/0 作为对照锚（§8）。runner 需 Node ≥22（本仓库环境为本地 `.references/node22/`）。
- runner 超时 kill 走 `shell:true` 的包装 shell，超时后 client 进程可能残留（孤儿），长跑套件需人工核对进程清理。
- 双 HTTP/TLS 栈为接受的长期代价，统一栈不在本设计射程。

## 最终核对

- [x] 接口、配置、路径与当前代码一致（自写侧）；cxxmcp 侧标注为外部核对事实。
- [x] 已实现与规划内容清楚区分（正文仍为规划；M0 spike 已在 `.references/` 内执行并落证据，仓库代码零改动）。
- [x] 外部来源、版本和实际采用内容可追溯（SHA + 核对层级如实标注）。
- [x] 验证结果有证据，未执行事项如实标明。
- [ ] 索引与架构总览按需更新（索引已登记；architecture.md 待接受后随实现更新）。