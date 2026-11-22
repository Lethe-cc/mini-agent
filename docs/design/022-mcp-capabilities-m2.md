# mini-agent 技术方案 022：MCP M2 能力扩展（resources/prompts/sampling/elicitation/tasks + 版本上移）

## 文档状态

- 状态：**草案 / 待人工评审**（2026-11-22）。本文是 [021](021-mcp-full-client.md) §5.1 M2 的细化设计；未获批前不进入实现。M2b（sampling/elicitation）含审批与 UI 行为变化，须过 ADR 006 同款人审，且预计需要一篇新 ADR（编号届时顺延，下文暂称"ADR-M2b"）。
- 关联任务：[ADR 010](../decisions/010-mcp-full-client-cxxmcp.md)（已接受）；021 §5.1"M2 能力扩展"；维护者 Q3 批复"M1 期钉 2025-06-18，M2 再上移"。
- 核对日期、适用版本或代码修订：2026-11-22，基线 main `6a2bd79`（M1 已翻转默认后端）。SDK 事实按 vendored cxxmcp `3c25a8afef0ae77031bcb7a3142bcca67d4cd3d6`（VERSION 1.3.0）头文件/源码实读核对，行号见 §3；全库仍未精读。
- 人工评审记录：无（本文档为草案，§4 取舍与 §5.4 待裁决问题需维护者批复）。

## 1. 背景与目标

M1 已把 MCP 底座替换为 vendored cxxmcp（默认后端），但客户端能力仍钉在 tools 面：握手发送空 capabilities（tools-only），入站服务端请求一律回 MethodNotFound，版本白名单 `2025-06-18 / 2025-03-26 / 2024-11-05`。SDK 侧 resources/prompts/sampling/elicitation/tasks 的完整 API 已随 vendoring 入库（§3），M2 的任务是把这些能力接入 mini-agent 的注册表、审批、UI 与配置层。

成功标准：
- M2a：模型可发现并读取 server resources、获取 prompts；tools 既有行为零变化。
- M2b：server 可发起 sampling/elicitation，经人审设计的审批与 UI 处理；默认关闭，显式开启才生效；非交互环境默认拒绝。
- M2c：长任务（tasks）可创建、查询、取消、取结果，状态机落档 [020](020-state-machines.md)。
- 版本基线上移（目标 2025-11-25），协商白名单随之扩展；deepwiki 与双 mock 重放全绿。
- ADR 006 既有工具审批语义零变化；新增审批面单独批复。

## 2. 范围与非目标

范围：M2a/M2b/M2c 三期能力接入 + 版本白名单上移 + mock 升级 + 配置 schema 增字段。

非目标：
- M3（删 legacy 三层）不在本文；仍按 021 §7 观察一个版本周期后另行执行。
- 不启用 OAuth/auth（Q2 批复延续 OFF；启用须另行人审设计）。
- 不做 roots 能力面（SDK 支持，但 mini-agent 无"向 server 声明工作区根"的需求输入；留待需求出现再设计）。
- 不做 resource subscriptions 的实时 UI 推送（M2a 只做 list_changed 缓存失效；subscribe/updated 推送留 M2c 之后评估）。
- 不动 TUI/CLI 并发形态（单线程 poll 循环 + TurnBridge worker join 窗口）。
- 不统一 HTTP/TLS 栈（021 已接受双栈共存）。

## 3. 约束与参考资料

项目约束：C++17；ADR 002 vendoring 纪律（M2 原则上零新增 vendored 文件，见 §5.5 裁剪清单核对）；冷启动预算不变（能力发现全部懒加载，不新增启动期 RPC）；ADR 006 审批语义；021 §5.3 线程契约（SDK handler 运行在 transport 接收线程，绝不触碰 UI）。

SDK 事实（vendored 树实读，2026-11-22；路径前缀 `third_party/cxxmcp/sdk/`）：
- 能力方法（client/include/cxxmcp/client/client.hpp）：`list_prompts()`/`_page(cursor)`/`get_prompt`（:386/:390）、`list_resources()`/`_page`/`read_resource`（:405/:409）/`list_resource_templates`、`list_all_*` 分页内建；`call_tool_task`→`CreateTaskResult`（:441）；`complete` 家族（:450–476）；出站 `create_message`（:481）/`create_elicitation`（:490）；`list_tasks`/`get_task`/`cancel_task`/`task_result`（:498–525）；`subscribe`/`unsubscribe`（:535/:538）；`server_capabilities()`（:348）；`set_capabilities(protocol::ClientCapabilities)`（:713）；`set_handler(ClientHandler)`（:789）。全部返回 `core::Result<T>`。
- Handler 类型（client.hpp:259–290 + handler.hpp）：`SamplingRequestHandler = function<Result<CreateMessageResult>(const CreateMessageParams&)>`（另有 CancellationToken 变体）；`ElicitationRequestHandler` 同构；通知 handler 含 `on_task_status(Task)`、list_changed 家族、`ResourceUpdatedHandler(uri)`。
- **handler 线程**（源码实证）：`Client::ensure_transport_started`（client.cpp:295）把 `handle_request`/`handle_notification` 交给 `transport_->start()`；stdio 在 reader 线程同步调用（stdio_transport.cpp:161），HTTP 在 per-request worker 线程调用（http_transport.cpp:1405）。**handler 阻塞 = 该 transport 的入站处理阻塞**；stdio 下还会阻塞后续消息读取（单 reader 循环），这是 §5.3 审批桥设计的硬约束。
- 入站缺省行为（client.cpp:1710–1777）：sampling 无 handler → MethodNotFound；elicitation 无 handler → **Decline**（不是 MethodNotFound，M1 as-built 描述的"一律 MethodNotFound"对 elicitation 不成立——M1 发送空 capabilities，server 依约不应发起，故实测未触发差异）；未知方法 → MethodNotFound；handler 抛异常被捕获转 error response（client.cpp:1795–1802）。
- 版本协商（client.cpp:562–660）：`initialize` 接受 `RequestOptions.protocol_version` 覆盖首选项；server 拒绝并在 error.data 给出 supportedVersion/supported 数组时自动带新版本重试一次；成功版本记入 `record_server_capabilities` 并同步 HTTP 头。serialization.hpp:30–40：支持 `2024-11-05/2025-03-26/2025-06-18/2025-11-25/2026-07-28/DRAFT-2026-v1`，SDK 默认 2025-11-25。
- DTO：`CreateMessageParams`（sampling.hpp:231，messages/model_preferences/system_prompt/include_context/temperature/max_tokens/stop_sequences/tools/tool_choice）；`CreateElicitationRequestParam`（elicitation.hpp:335，**mode=Form|Url**；Form 携 `ElicitationSchema`＝扁平 primitive 属性表 + required 列表，:144；Url 携 elicitation_id+url，经 `notifications/elicitation/complete` 异步完成）；`ElicitationAction{Accept,Decline,Cancel}`；`TaskStatus{Working,InputRequired,Completed,Failed,Cancelled}`（task.hpp:28）；`Task{task_id,status,status_message,created_at,ttl,poll_interval,...}`（:144）；`TaskCapabilities{list,cancel,tools_call,sampling_create_message,elicitation_create}`（capabilities.hpp:125）；`ClientCapabilities{roots,sampling,elicitation,optional tasks,...}`（:296）+ Builder。
- 资源读取结果含 `ttl_ms` 缓存提示（resource.hpp:293，SEP-2549）；`PromptsGetResult` 有 MRTR `input_required` 判别（prompt.hpp:220，SEP-2322，属新版本协议面）。

项目接缝（实读）：
- [mcp.hpp](../../include/mini_agent/mcp.hpp)：`McpSession` 缝保持 SDK-free + nlohmann-free；`McpClient` 门面单所有者线程契约（CLI turn 线程 / TUI worker join 窗口）。M2 能力方法必须沿此缝扩展。
- [mcp_rpc.cpp](../../src/mcp/mcp_rpc.cpp)：`handshake()`（:211）发送空 capabilities + `kPreferredVersion="2025-06-18"`，白名单 `negotiable_version`（:16）；`build_tool_registry`（:486）。
- [tui_bridge.hpp](../../include/mini_agent/tui_bridge.hpp)：审批固定槽协议（publish_approval→seq→answer_approval(1/2/3)，20ms 轮询 + 120s 超时 + abort 钩子），M2b 服务端请求审批必须复用此形态改为队列。
- [provider.hpp](../../include/mini_agent/provider.hpp)：`complete_chat(chat, tools_json, on_delta, should_abort)`（:166）→ `CompletionResult{text,tool_calls,...}`（:128）。sampling 执行体将复用此缝。
- [config.hpp](../../include/mini_agent/config.hpp)：`McpServerConfig`（:17）现有 name/transport/command/args/env/url/bearer_token/allow_insecure_http；schema_version 上限 2。
- [session.cpp](../../src/core/session.cpp)：`build_system_prompt`（:427）是资源/prompt 提示注入缝。
- mock：tests/mock_mcp_server.py 与 mock_mcp_http_server.py 仅实现 initialize/tools 面，M2 各期需同步升级（§8）。

## 4. 方案与关键取舍

| # | 取舍 | 选定方案 | 主要备选 | 理由 | ADR |
| --- | --- | --- | --- | --- | --- |
| T1 | resources/prompts 暴露形态 | **合成工具**：每 server 注册 `mcp_resources`（list/read 子动作）与 `mcp_prompts`（list/get 子动作）两个虚拟工具，走既有 ToolRuntime/审批/注册表管线 | 自动注入 system prompt；新增独立 slash-only 面 | 零新审批面（复用 ADR 006 tool_approval）；模型按需拉取不炸上下文；slash 面（/mcp 详情）作为人类入口另行小改 | — |
| T2 | 资源内容缓存 | M2a 不做内容缓存，只做元数据列表缓存 + list_changed 失效 | ttl_ms 驱动的内容缓存 | 首版从简；读取走 server 现值，语义最安全；缓存留观察后评估 | — |
| T3 | prompts 呈现 | `/mcp prompts <server>` 列出；`/prompt <server>/<name> [k=v...]` 获取并把 messages 注入当前会话（user 角色文本块；非文本块降级为描述占位） | 仅作为模型工具 | prompts 的设计意图就是用户主动触发模板；工具面（T1）同时保留给模型 | — |
| T4 | sampling/elicitation 开关 | **per-server 配置字段，默认 OFF**；OFF 时不声明对应 capabilities，server 依约不发起 | 全局开关；默认 ON+审批兜底 | 021 §6 已定"默认关"；per-server 粒度与 mcp_servers 配置形态一致；不声明能力是最干净的拒绝 | ADR-M2b（待写） |
| T5 | 服务端请求审批 | 新增 **ServerRequestBridge 队列**（TurnBridge 旁挂），UI 在 poll 循环消费；sampling 逐次审批（含内容预览），elicitation Form 逐字段填写即审批 | 复用工具审批固定槽；无审批直接执行 | 固定槽会被工具审批占用产生交叉死锁；队列天然支持 turn 内多请求；elicitation 本身就是问用户，填写=同意 | ADR-M2b |
| T6 | 非交互/超时语义 | 无 UI 消费方（CLI ask、TUI worker 未接管）或超时（沿用 120s）→ sampling 返回 error、elicitation 返回 Decline；不挂起 | 阻塞等待 | SDK handler 在 transport 线程同步执行（§3），挂起会冻结整个连接；CLI 无审批 UI，与 ADR 006 ask 模式 --tools off 的保守取向一致 | ADR-M2b |
| T7 | tasks 触发形态 | M2c 仅当 server 声明 `tasks.tools_call` 时，`call_tool` 超时策略升级为"可选 task 化"：合成工具 `mcp_tasks`（list/get/result/cancel）+ 工具调用侧 `task=true` 参数透传 | 自动把所有慢工具 task 化 | 显式可控；状态机/UI 增量最小；InputRequired 态对接 T5 队列 | — |
| T8 | 版本上移 | 白名单扩为 `2025-11-25/2025-06-18/2025-03-26/2024-11-05`，首选 2025-11-25；**硬前置：上游析构缺陷修复确认**（§6） | 直升 2026-07-28；维持 2025-06-18 | 2026-07-28/DRAFT 面太新且 conformance 覆盖未复核；2025-11-25 是 SDK 默认且 M0 conformance 已覆盖；Q3 批复"M2 再上移" | — |
| T9 | 配置 schema | **加字段不升 schema_version**（新字段全部可选、缺省=旧行为，v2 读端向后兼容） | bump 到 3 | 现有读端对未知字段宽容、写端 only-additive；无破坏性变化不必制造版本墙（若评审认为要显式版本墙则改 bump，见 Q3） | — |

## 5. 详细设计

### 5.1 M2a：resources + prompts（纯出站，先行）

**缝扩展**（mcp.hpp，保持 SDK-free/nlohmann-free——JSON 字符串进出）：

```cpp
// McpClient 新增（全部懒加载，失败折叠为诊断尾，不 throw）
std::string list_resources(std::string* error);   // 聚合 JSON 数组（list_all_resources 分页内建）
std::string read_resource(const std::string& uri, std::string* error); // contents 原样 JSON
std::string list_prompts(std::string* error);
std::string get_prompt(const std::string& name, const std::string& args_json, std::string* error);
bool has_resources() const; bool has_prompts() const;  // 握手 capabilities 探测结果
```

实现落 [mcp_cxxmcp.cpp](../../src/mcp/mcp_cxxmcp.cpp)（唯一 SDK TU）：经既有 `request()` 通路走 typed API（`list_all_resources`/`read_resource`/`list_all_prompts`/`get_prompt`），`Result<T>` 错误折叠沿用 M1 规则。capabilities 探测：`handshake()` 解析 `capabilities.resources/prompts` 存旗标（现只存 tools_capability_）。

**合成工具**（build_tool_registry 内，仅当对应旗标真时注册）：
- 名称 `server/mcp_resources`、`server/mcp_prompts`（沿用现命名空间与净化规则）；schema：`{"action":"list|read","uri":...}` / `{"action":"list|get","name":...,"arguments":{...}}`。
- 走 ToolRuntime 正常调用 ⇒ ADR 006 审批、预览、错误折叠全部免费复用；read/get 结果按现有工具输出截断规则处理。
- list_changed 通知：经 §5.3 桥投递为缓存失效事件，门面丢弃对应列表缓存（M2a 只缓存列表元数据，T2）。

**UI**：`/mcp` 详情行加 resources/prompts 计数；新增 `/prompt <server>/<name> [k=v...]`（T3）：get_prompt 的 messages 转成会话注入（text 块直入；image/audio/resource 块 M2a 降级为 `[unsupported content: type]` 占位并在回复中说明）。`build_system_prompt` 增一行能力提示（仅当存在带 resources/prompts 的 server）。

**MRTR 边界**：`PromptsGetResult` 的 `input_required`（SEP-2322）仅在协商到 2025-11-25+ 才可能出现；M2a 若先于版本上移落地则不会遇到；上移后遇到时按"prompt 需要追加输入"错误折叠返回，不做多轮 MRTR 交互（留待后续需求）。

### 5.2 M2b：sampling + elicitation（入站，人审门）

**配置**（McpServerConfig 增可选字段，T9）：

```jsonc
{ "mcp_servers": [{ "name": "x", ...,
    "sampling": true,          // 默认 false；true 才声明 sampling capability
    "elicitation": true        // 默认 false；true 才声明 elicitation capability（仅 form）
}] }
```

握手 capabilities 由空对象改为按配置构造（经 `set_capabilities`）：sampling 开启 ⇒ `sampling{}`（不声明 tools/context 子能力，M2b 不支持 sampling 内工具调用）；elicitation 开启 ⇒ `elicitation{form:{}}`（**不声明 url**——URL 模式引外部浏览器流，超出终端 UI 能力面，收到 Url 模式请求回 Decline）。

**sampling 执行体**：handler 收到 `CreateMessageParams` → 经 §5.3 桥发布审批（预览=server 名 + messages 摘要 + max_tokens）→ 用户批准后，在 **worker/turn 侧**用 `Provider::complete_chat` 执行（system_prompt/messages 转 Chat；temperature/stop_sequences M2b 忽略并在审批预览注明；params.tools 非空 ⇒ 拒绝，因未声明该子能力）→ 结果回填 `CreateMessageResult{role:"assistant",content:text,model:实际模型名}`。拒绝/超时 ⇒ error response（T6）。**执行线程**：不得在 transport 线程调 LLM（长阻塞冻结连接）——桥协议为"transport 线程发布请求并等待带超时的应答"，LLM 调用发生在 UI 侧消费线程；等待上限 = 审批 120s + LLM 调用超时（沿用 provider 现值），超限回 error。
**elicitation 执行体**：Form 模式 → 桥发布表单（message + schema 字段表）→ TUI 渲染逐字段输入（string/number/boolean/enum primitive），提交=Accept+content，Esc=Decline，超时=Decline（T6）；SDK 侧 form_schema_validation 不开启（校验交给 UI 输入约束 + server）。Url 模式 ⇒ Decline。`notifications/elicitation/complete` 忽略（未声明 url）。

**审批语义与 ADR 006 的关系**：工具审批固定槽协议不动；ServerRequestBridge 是独立队列，UI 渲染为独立面板行（样式沿用工具审批行）。`ask` 非交互子命令：即使配置开了 sampling/elicitation，无 UI 消费方一律按 T6 拒绝（capabilities 照常声明——server 请求得到明确拒绝而非静默）。此语义整体进 ADR-M2b 人审。

### 5.3 线程桥：ServerRequestBridge（M2b 核心，M2a 通知复用）

021 §5.3 契约的落地形态：

```
transport 接收线程                    UI poll 线程（TUI main / CLI 无）
  SDK handler(params)
    ├─ bridge.publish(req) → id       ├─ poll: bridge.try_take() → 渲染审批/表单
    ├─ bridge.wait(id, timeout,       ├─ 用户操作 → bridge.answer(id, payload)
    │        abort_fn)  ←─ 20ms 轮询  └─ turn 结束 reset：drain 未答请求 → 全部拒绝
    └─ 返回 Result<T>（超时→拒绝）
```

- 数据结构：mutex 保护的 `deque<PendingRequest>` + per-id 应答槽（condition_variable 或 20ms 轮询，沿 TurnBridge 现风格取轮询以便 abort 钩子）；id 单调递增防串答（同 TurnBridge seq 语义）。
- **所有权**：bridge 由会话层持有，寿命覆盖 McpClient；handler lambda 只捕获 bridge 裸指针 + server 名，McpClient 析构前必须先 `bridge.shutdown()`（置关闭旗标，所有等待立即返回拒绝），再停 client——顺序写进 mcp.hpp 注释契约。
- TUI worker join 窗口：worker 不消费 bridge；join 期间到达的服务端请求排队至 main 恢复 poll（120s 超时兜底）。CLI turn 线程同样不消费 ⇒ T6 拒绝路径。
- 通知（list_changed/task_status/resource_updated）：同一桥的 fire-and-forget 通道，UI 侧消费为缓存失效/状态行刷新，无应答。
- 死锁审计要点：transport 线程 wait 期间不持任何 UI 锁；bridge.answer 只写字段不回调 SDK；`close()` 从 UI/worker 侧发起时先 shutdown bridge 再 join transport 线程（否则 stdio reader 卡在 handler wait 里 join 不动）。

### 5.4 M2c：tasks（长任务状态机）

- 前置：server 声明 `capabilities.tasks`（按 TaskCapabilities 子旗标裁剪行为）；协商版本 ≥2025-11-25（tasks 属新版本面）。
- 工具面：`call_tool` 增可选 task 化参数（仅 `tasks.tools_call` 真时透传 `TaskRequestParameters`）；`CreateTaskResult` 折叠为工具输出"任务已创建 task_id=...（用 mcp_tasks 查询）"。
- 合成工具 `server/mcp_tasks`：`{"action":"list|get|result|cancel","task_id":...}`，映射 `list_tasks/get_task/task_result/cancel_task`；`result` 在任务未终态时返回当前状态而非阻塞。
- 状态机（增补 [020](020-state-machines.md) 新节，PlantUML）：`Working → Completed|Failed|Cancelled`；`Working ↔ InputRequired`（InputRequired 时经 §5.3 桥发 elicitation 式输入请求；M2c 仅当 server 用 tasks.elicitation_create 发起）。`on_task_status` 通知驱动 UI 状态行；UI 不主动轮询（get 由模型/用户显式触发）。
- 会话结束/断连：任务留在 server 侧（TTL 由 server 管），客户端不追不取消；重连后 `mcp_tasks list` 可续查（task_id 不跨会话持久化，M2c 从简）。

### 5.5 版本上移与 vendoring 核对

- `kPreferredVersion` → `"2025-11-25"`；`negotiable_version` 白名单 +2025-11-25（保留旧三值；2026-07-28/DRAFT 不入白名单，T8）。SDK `initialize` 的 error.data 重协商路径（§3）在门面 `handshake()` 之下自动生效，门面白名单校验逻辑不变。
- **硬前置（021 §9）**：上游 `request_async` 的 `in_flight_requests_` 非 RAII 递减缺陷在本 vendored SHA 仍存在（client.cpp:1822 `++in_flight_requests_` 后裸 lambda 递减；~Client :2164 无超时等待）——auth 关闭时该路径不可达，M1 已验证；版本上移本身不触发 auth，但 2025-11-25 conformance 面更广，上移 PR 必须附带该缺陷的可达性复核（或上游修复 bump vendored SHA，或本地 RAII guard 补丁并登记偏差）。
- vendoring 裁剪清单核对：M2 用到的 handler 分发/DTO 代码是否都在 client-only 闭包内——`handle_request` 的 sampling/elicitation 分支在 client.cpp（已入库），sampling.hpp/elicitation.hpp/task.hpp 已入库；实施 M2a 首 PR 时按 `third_party/cxxmcp/README.md` 清单逐项核对，缺文件则按 vendoring 纪律补录（固定同 SHA），**不得**顺手引入 auth/server 目标。

### 5.6 待裁决问题（评审时批复）

- **Q1（T1/T3）**：resources/prompts 走合成工具 + `/prompt` 注入的形态是否认可？备选是自动上下文注入（不推荐，炸 token）。
- **Q2（T4–T6）**：sampling/elicitation per-server 默认 OFF、独立审批队列、非交互一律拒绝——是否按此写 ADR-M2b 送审？sampling 逐次审批是否要加"本会话允许"档位（工具审批有 allow-session，服务端请求面建议首版不给，防 server 连续采样刷配额）？
- **Q3（T9）**：配置只加字段不 bump schema_version，是否接受？
- **Q4（T8）**：版本目标 2025-11-25（非 2026-07-28），且以上游析构缺陷复核为前置，是否认可？
- **Q5**：M2a/M2b/M2c 严格串行（每期独立验收+人审）还是 M2a 先行、M2b/M2c 设计并行？建议严格串行。

## 6. 异常与安全

- 全部新能力错误折叠为诊断尾/Tool error，不 throw 出缝；bearer token 不入任何新日志/预览（沿 M1 红线）。
- sampling 是**把本机 LLM 配额借给外部 server**：审批预览必须展示 messages 摘要与 max_tokens；拒绝路径零 LLM 调用；params.tools/tool_choice 非空一律拒绝（未声明子能力）。elicitation 表单内容仅回给发起 server，不入会话历史（除非用户随后主动引用）。
- 恶意 server 高频入站请求：桥队列设上限（建议 8），溢出直接拒绝（sampling error / elicitation Decline），防 UI 刷屏与内存增长。
- 超时矩阵：审批 120s（沿 TurnBridge）；sampling 总等待 = 审批 + LLM 超时；elicitation 表单 120s；tasks 无客户端阻塞等待。全部超时路径回明确拒绝，绝不悬挂 transport 线程。
- 上游析构缺陷（§5.5）：M2 各期 PR 均需确认新增路径不会让 `in_flight_requests_` 泄漏（出站 typed API 走同一 request_async 路径——超时/取消分支的递减行为在 M2a 实施时以 ASan+压力测试复核）。

## 7. 兼容性与发布

- 默认行为零变化：新配置字段缺省 OFF/无；未配置时 capabilities 仍为空对象、白名单上移只影响与新版 server 的协商结果（旧 server 协商回旧版本，行为同 M1）。
- 回滚：M2a 合成工具可用配置级禁用（server 不声明能力即不注册）；M2b 配置 OFF 即回到 M1 语义；版本上移单独 PR，可独立 revert。legacy 回滚开关在 M3 前继续有效，但 **legacy 路径不实现 M2 能力**（自写三层无入站请求入口）——开启 M2 能力的 server 在 legacy 后端下按"能力不可用"降级并在 /mcp 详情注明。
- 发布节奏：M2a → 验收+人审 → M2b（先 ADR-M2b 获批）→ 验收+人审 → 版本上移 PR → M2c → 验收+人审。每期独立提交链，门禁全绿才进下一期。

## 8. 验证情况

| 项 | 命令/方法 | 结果 |
| --- | --- | --- |
| M2a：mock 升级（resources/prompts 面 + list_changed） | tests/mock_mcp_server.py / mock_mcp_http_server.py 扩展；双 mock E2E 新场景 | 未执行（计划） |
| M2a：合成工具审批/截断/错误折叠回归 | 现有 mcp 单测 + 新增 resources/prompts 场景；四 preset 双环境 CTest | 未执行（计划） |
| M2b：桥死锁/超时/溢出/非交互拒绝 | 新增单测（fake transport 注入入站请求）+ ASan/UBSan + pty TUI 审批流 | 未执行（计划） |
| M2b：sampling 真实 LLM 冒烟 | 本地 provider 对拍（mock LLM 优先，真实端点人工） | 未执行（计划） |
| M2c：tasks 状态机全迁移路径 | mock tasks 面 + 020 迁移表逐行对照测试 | 未执行（计划） |
| 版本上移：协商矩阵（新/旧/拒绝重试） | mock 版本矩阵 + deepwiki 实测重放 + conformance client suite 复跑 | 未执行（计划） |
| 全量门禁 | `python scripts/check.py --strict-project` + pty/CLI + 冷启动预算 | 未执行（计划） |
| 上游析构缺陷可达性复核 | 代码审读 + 压力/超时注入测试（§5.5） | 未执行（计划；M1 期 gdb 证据已归档 .references/conf-diag2/） |

## 9. 实现偏差与已知限制

（草案期无实现。以下为设计期已识别限制，实施后按 as-built 更新。）

- SDK elicitation 无 handler 时回 Decline 而非 MethodNotFound（client.cpp:1737），与 021 §9 M1 as-built 的概括描述有出入；M1 发空 capabilities 故实际不可达，无行为影响，021 勘误随 M2a 首 PR 顺带登记。
- sampling 的 temperature/stop_sequences/include_context 在 M2b 忽略（provider 缝现不支持逐请求覆盖）；审批预览注明。
- elicitation Url 模式、roots、resource subscribe 实时推送、MRTR 多轮、tasks 跨会话持久化均为明确的非目标/后续项（§2、§5.1、§5.4）。
- legacy 后端不支持 M2 能力（§7），双轨期开启新能力的 server 在 legacy 下降级。

## 最终核对

- [x] 模板结构完整；状态为草案，未自称人审通过。
- [x] SDK/项目事实按 vendored SHA 与 main `6a2bd79` 实读核对（§3 行号）。
- [x] 验证计划全部标注未执行；无计划冒充结果。
- [ ] 维护者批复 §5.6 Q1–Q5；ADR-M2b 立项与送审。
- [ ] 获批后更新 docs/design/README.md 状态列并进入 implement-feature 工作流。