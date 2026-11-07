# mini-agent v4 技术方案（模型元数据：token 用量采集 + 上下文窗口 + 思考级别 + 底部状态栏）

## 文档状态
- 状态：已实现（2026-11-07，验证见第 8 节；待人工评审）。
- 关联任务：维护者 2026-11-06 提问"agent 能否获取模型上下文长度/使用量/思考级别"，确认三者现状均不可得后，2026-11-07 批复"开始补全这些功能，显示位置就在底部 bar"。承接 [009](009-coding-v3.md)（v3 编码能力，已实现）。
- 核对日期与适用版本：2026-11-07 实读本仓库 `src/provider_openai.cpp`、`src/loop.cpp`、`src/config.cpp`、`src/tui.cpp` 与 [TUI 界面调研](../references/tui-agent-ui-survey.md)（状态栏专题第 4 节，四家共识 model·ctx%·cost 底线）。
- 人工评审记录：无。

## 1. 背景与目标

v3 之后 TUI 状态行只有 model/session/连通/auth 信息，没有任何用量数据：请求体不发 `stream_options.include_usage`（usage chunk 被 SSE 解析器的 choices 空数组守卫直接丢弃）、`/models` 元数据只取 id、无 reasoning 通道。调研结论（references 调研文档 §4.5）：四家参考 agent 的底部状态栏共识底线是 `model · ctx% · cost`，本项目缺后两者数据源。

成功判据（可验证）：

- provider 回 usage 时，状态栏显示真实 `ctx 已用/最大`（prompt_tokens ÷ 配置窗口决定着色阈值）与 `out` 累计；无 usage 时回退字符类估算并带 `~` 标记。
- `context_window`、`reasoning_effort` 两个配置项：文件/env/TUI 三入口一致，配置即权威来源（不探测、不被服务端应答覆盖）。
- 未配置任何 v4 项时：请求体、状态行与 v3 字节一致（回归钩子，007/008/009 惯例）。

## 2. 范围与非目标

本期交付：

- **usage 采集**：请求体 `stream_options.include_usage`；SSE 解析 usage chunk（含 `input_tokens/output_tokens` 兼容命名折叠）；`CompletionResult::usage`；`TurnOptions::usage_out` 跨轮汇总（prompt=末轮、completion/total 累加；仅成功 turn 提交，与历史同一提交边界）。
- **估算兜底**：纯函数 `estimate_context_tokens`（ASCII≈4 字符/token、CJK≈1/codepoint、其余≈3；粗估，UI 标 `~`）。
- **配置**：`AppConfig.context_window`（正整数，非法值整份配置失败关闭）与 `reasoning_effort`（字符串原样透传）；env `MINI_AGENT_CONTEXT_WINDOW`/`MINI_AGENT_REASONING_EFFORT`；`ProviderConfig.reasoning_effort` 经 `to_provider()` 贯通。
- **TUI**：底部状态栏用量段 `status_usage_segments`（`ctx 已用/最大`，按百分比 70% 黄/90% 红着色、`out`、`reasoning=`）；`/context <tokens>|off`、`/reasoning <value>|off` 命令；`/config` 展示两字段；模型切换复位读数；400 + reasoning 已设时提示 `/reasoning off`。

非目标（明确排除）：

- 费用（cost）显示：需要 per-model 单价表，本期无数据源，留 v5。
- cache 命中率（Pi 的 `CH%`）：依赖网关回 `cache_read_input_tokens` 类字段，兼容面太窄。
- context 压缩（/compact）：ctx% 高时仅变色提示，不动历史。
- 服务端元数据探测（/models 的 context_length 等）：按维护者决策"配置即权威"，不实现探测优先级。
- CLI `ask` 输出用量：维护者指定显示位置为 TUI 底部 bar；CLI 保持脚本契约不变。

## 3. 约束与参考资料

- 零新增第三方依赖（各版本硬约束）：全部用现有 libcurl + nlohmann。
- 借鉴模式（不复制代码，MIT/Apache-2.0 许可合规，见 [coding-agents](../references/coding-agents.md) 与 [TUI 调研](../references/tui-agent-ui-survey.md)）：Pi footer 的 ctx 阈值配色与 `~` 估算标记；Codewhale `estimated_context_tokens` 的"usage 优先、估算兜底"次序；Codewhale 状态栏"model 与 ctx% 永不丢弃"的底线（本实现暂用既有 md::clip 截断，未做字段级 shed——记为已知限制）。
- OpenAI `stream_options.include_usage` 语义（末个 chunk、choices 为空数组）为公开协议事实；实现按"last-seen wins"容错逐 chunk 上报的网关。

## 4. 方案与关键取舍

- **usage 汇总语义**：prompt_tokens 取末轮（= 实时上下文占用，状态栏 ctx 的正确读数），completion/total 跨轮累加（= 本 turn 工作量）。备选"全部累加"会把多轮工具 turn 的 ctx 虚高数倍，弃。
- **估算只在完全无 usage 时启用**：一旦某 turn 拿到真实 prompt_tokens 就永久优先，直到模型切换复位。避免真实/估算值交替抖动。
- **`stream_options` 不进 preview 钩子默认路径**：`preview_chat_request_body` 新参数默认关闭，v1 字节一致回归测试原样通过；真实请求路径显式开启。
- **reasoning 失败不静默剔除**：400 时提示 `/reasoning off` 但保留配置值——静默改写会让用户误以为参数已生效（配置权威原则的另一面）。
- **context_window 非法值整份失败**：与 tool_approval 的宽松容错不同，窗口值参与显示计算，静默丢弃会让 ctx% 消失得像 bug；fail-closed 更早暴露拼写错误。

## 5. 详细设计

- `include/mini_agent/provider.hpp`：`TokenUsage{prompt,completion,total}` + `CompletionResult::usage`；`ProviderConfig::reasoning_effort`；`estimate_context_tokens` 声明。
- `src/provider_openai.cpp`：`request_body` 增两参数（默认值保持旧字节形状）；SSE `on_data_` 在 choices 守卫**之前**读 `usage` 对象（含 input/output_tokens 折叠）；`estimate_context_tokens` 实现（UTF-8 首字节分类）。
- `include/mini_agent/loop.hpp` / `src/loop.cpp`：`TurnOptions::usage_out` 指针；每轮 `ask_provider` 后合并进 `turn_usage`；提交段写回（仅 `usage_seen`）。
- `include/mini_agent/config.hpp` / `src/config.cpp`：两字段 + JSON 读写 + env 覆盖 + `to_provider()` 透传 reasoning。
- `include/mini_agent/tui.hpp` / `src/tui.cpp`：`StatusInputs` + `status_usage_segments`（纯函数可测）；`TurnResult::usage`；run_loop 读数状态（`context_reading`/`reset_usage_readings` lambda，估算带 `estimate_dirty` 惰性重算）；状态行在 auto-tools 之后、瞬时态之前拼接；`/context`、`/reasoning` 命令；`/config` 两行展示；模型选择两处复位；400 hint。
- `tests/mock_openai_server.py`：仅当请求带 `stream_options.include_usage` 时追加 usage chunk（123/45/168），对齐真实网关语义。

## 6. 异常与安全

- usage chunk 字段缺失/非整数 → 计 0，不报错（显示降级为估算路径）。
- 日志零新增敏感面：usage 是纯计数，logger `turn` 事件本期未加 token 字段（避免与消息正文混淆），估算与显示全部本地。
- `reasoning_effort` 值原样透传：来自用户配置，不来自模型输出，无注入面；错误提示仅回显字段名不回显值。

## 7. 兼容性

- 无 v4 配置时请求体/状态行与 v3 字节一致（回归钩子）。
- session 文件无变化（usage 不持久化，进程内读数）；config schema_version 保持 1（新字段可选、旧构建忽略即回退默认，无破坏）。
- 旧网关忽略 `stream_options`：无 usage chunk，状态栏走估算路径，功能不失效。

## 8. 验证情况（2026-11-07 实现轮，已执行）

| 项 | 命令 | 结果 |
| --- | --- | --- |
| 单元 | `agent_test`（新增 `test_v4_usage_collection`/`test_v4_config_fields`/`test_v4_status_segments`，含多轮 usage 累加 FakeUsageProvider） | 全部通过 |
| 回归 | `test_v1_request_shape_regresssion`（默认参数路径） | 通过：无 v4 配置请求体不变 |
| CTest | `ctest --test-dir build/linux-gcc-debug` | 3/3 通过 |
| 质量门禁 | `python3 scripts/check.py --strict-project`（gcc-debug configure+build+CTest） | PASS，无告警（-Wall -Wextra -Wpedantic） |
| 格式 | `python3 scripts/cpp_format.py` | 通过 |
| mock E2E | `mini-agent ask` 对 mock（usage chunk 条件发送） | 回复正常，协议兼容 |
| TUI 实屏 | 临时 PTY 驱动（`run` + env 配置 + mock）：断言状态栏出现 `ctx`/`out`/`reasoning=low` 段与 `/context` 读数 | 4/4 PASS |

未验证：真实 OpenAI/DeepSeek 网关的 usage chunk 时序（mock 对齐公开协议，真机待维护者冒烟）；Release/clang preset 组合（本轮 gcc-debug；CI 四组合覆盖）。

## 9. 实现偏差与已知限制

- D1：状态栏字段级 shed 优先级（Codewhale 式"窄终端按序丢弃"）未实现，沿用既有 `md::clip` 右缘截断；窄终端可能截掉 reasoning 段。留待状态栏重构时统一处理。
- D2：估算器为粗粒度字符类计数，CJK 判定用 UTF-8 首字节范围近似（E3..EF 三字节段），kana/部分汉字外字符可能归类偏差——设计上接受（估算带 `~`，不承诺精度）。
- D3：`/context off` 与 `/reasoning off` 语义为"清除并落盘"，与 `/log off` 的哨兵值模式不同（那两个字段无"显式禁用"与"未设置"的三态需求）。
- D4：usage 不写入 session 文件——resume 后状态栏从估算重新开始，直到下一 turn 拿到真实 usage。持久化留待与 cost 表一起设计。
- D5（v4.2）：reasoning_effort 回显位置只覆盖顶层与 choices[0] 两种已见形态；真实 OpenRouter 用顶层，长尾兼容网关若有第三种位置需按 008 R2 逐网关核实。

## 10. v4.1 修订（2026-11-07 维护者反馈）

维护者实机看到 `ctx ~1.2k`（未设 context_window 的降级形态）并反馈两点：

- **ctx 改为"已占用/最大"格式**：`ctx 45.2k/128.0k`，比裸百分比信息量高；着色阈值仍按 `used/max` 百分比（70% 黄、90% 红）。窗口未配置时保持 `ctx ~1.2k` 单值形态。
- **思考级别不显示是设计行为**：`reasoning=` 段仅在配置了 `reasoning_effort` 时出现（未设 = 请求不带该字段 = 无可显示值）。该会话未设置此项，属预期；设置入口 `/reasoning low|medium|high` 或 env `MINI_AGENT_REASONING_EFFORT`。

## 11. v4.2 修订（2026-11-07 维护者需求：候选级别实时获取 + 实际级别从报文提取）

维护者提出两点：① `/reasoning` 的候选级别应实时从 provider 获取；② 状态栏的 reasoning 应显示**实际生效**级别（从回答报文提取），而非配置回显。

实现（provider → loop → TUI 三层）：

- **能力元数据**：`parse_models_meta_json`（与 `parse_models_json` 同一 GET /models 体、同一 fail-closed 守卫）保留每模型 `context_length` 与 OpenRouter 形态的 `reasoning{mandatory, supported_efforts[], default_effort}`（兼容裸 bool 形态）；`Provider::list_model_capabilities()` 虚函数 + `OpenAiProvider` 实现，共享 `fetch_models_body` 传输段（api_key 只进 header 的约束不变）。标准 OpenAI 无这些字段 → caps 为空，行为回落到 v4 形态。
- **TUI 缓存与选择器**：models worker 一次请求同时产 ids+caps（HTTP 次数不增加）；`/reasoning` 无参时若当前模型在 caps 中有 `reasoning_efforts`，输出编号菜单（`reasoning_menu_rows`，标 `<- default`/`<- current`，mandatory 提示），选择复用 `model_choice`；Esc 可关菜单。
- **实际级别三路证据**：SSE 解析顶层或 choices[0] 的 `reasoning_effort` 回显串（记 level）、`delta.reasoning_content`/`delta.reasoning` 非空串（记 seen，思考正文不进回复流不落存储）、`usage.completion_tokens_details.reasoning_tokens>0`（记 seen+tokens 标志）。`ActualReasoning` 经 `CompletionResult.reasoning` → `TurnOptions::reasoning_out`（与 usage 同提交边界：仅成功 turn）→ `TurnResult.reasoning` → 主线程状态。
- **状态栏优先级**：`reasoning=` 显示实测 level > 实测 seen 无 level 显示 `on` > 无证据回显配置值；模型切换复位证据（`reset_usage_readings`）。sticky 语义：seen 一旦为真保持到换模型——后续普通 turn 不再显示旧配置值误导，也不谎称 off。

取舍记录：

- 思考**正文**本期仍不显示/不存储（维护者需求是"级别"不是"内容"）；三路证据即 §2 非目标里 reasoning 内容渲染的接入点，需要时加 `on_reasoning` 回调即可。
- caps 是提示性数据：`/reasoning <任意值>` 字面量路径不校验、不拦截（配置权威原则不变）；菜单只是把网关声明的档位摆出来。
- 实测中 mock 暴露回显位置分歧（顶层 vs choices[0]），解析器两处都查——真实 OpenRouter 用顶层，兼容网关形态未穷举（记 §9 D5）。

验证（2026-11-07）：`test_v42_capabilities_and_menu`（解析三形态/失败关闭/菜单行/编号选择/状态优先级）通过；agent_test 全量、CTest 3/3、`check.py --strict-project`、`cpp_format.py` 全过；宽 PTY（200 列）E2E：`/models`→`/reasoning` 菜单出 3 档带 default 标记、编号 2 选中 medium、REASONTEST turn 后状态行 `ctx 123/131.1k · out 45 · reasoning=high`（实测 high 压过配置 medium）——全 PASS。

## 12. v4.2.1 修订（2026-11-07 维护者实机反馈：`reasoning=on` 与 `ctx 3.1k` 显示不符预期）

维护者真机（qwen3.8-flash）看到 `ctx 3.1k · out 34 · reasoning=on`，认为不对。诊断：qwen 网关有思考流（delta.reasoning_content）但**不回显档位字符串**，v4.2 规则"seen 无 level → on"于是压过了配置值并 sticky 常驻；`ctx` 无 `/最大` 因未配 context_window。三点修正：

- **证据 + 配置值 → 显示配置值**（证据确认了它，显示 `on` 反而是降级）；仅"有证据且任何地方都不知道档位"才显示 `on`；无证据回显配置值（intent, unconfirmed）。显示优先级：echo > 被证据确认的配置值 > `on` > 配置回显。
- **去 sticky**：读数只反映**最近一次成功 turn** 的证据，本轮无证据即清空（后续普通回答不再挂着旧的 `on`）。
- **context_window 兜底链**：config 显式值 > 缓存 caps 的 `context_length`（网关声明）> 无（单值形态）。qwen3.8-flash 这类未配置场景在 OpenRouter 形态网关上自动获得 `/最大`。

验证（2026-11-07）：agent_test 更新语义断言（seen+config→config 值、裸 seen→on）全过；宽 PTY E2E 三断言 PASS——caps 兜底出 `ctx 123/131.1k`、REASONTEST 后 `reasoning=high`、其后普通 turn 读数清空不残留。

## 13. v4.3 修订（2026-11-08 维护者需求：让 agent 能获取 context 窗口大小）

维护者实测（qwen3.8-flash 真机自述探测）验证了"错误消息即权威边界"的路子后要求接入。取舍与形态：

- **只做廉价探测（方法 2）**：一次非流式小请求带 `max_tokens=1e9`，从网关拒绝消息里读数字。超长 prompt 探测输入上限（方法 3）明确不做——成本随窗口大小线性涨且有逻辑死结（须先超限才看得到边界）。
- **三种消息形态白名单**（`parse_context_probe_error`，纯函数）：`maximum context length is N tokens`（vLLM/OpenAI，即总窗口）、`Range of <param> should be [1, N]`（DashScope 取括号内最后一个合法数字）、`max_tokens: X > N`（OpenAI 输出上限，小 prompt 下≈实用窗口）。合理域 1k..200M 之外的数字一律拒绝；JSON 错误体先抽 `error.message` 文本再匹配，请求回显值永不误读。
- **静默 clamp 显式承认拿不到**：网关接受超大 max_tokens（2xx）时返回 0 → TUI 提示手工 `/context <tokens>`。不解析成功体。
- **仅显式触发**：`/context probe` 一条命令，后台 worker（30s curl 上限、join-then-read、退出 join），不自动跑——探测计费、错误文本非契约，何时花这个钱由用户决定。结果 >0 时写入 config（此后它就是权威值，可再 `/context <tokens>` 覆盖或 `/context off` 清除）并落日志（redact_url，零密钥）。
- **兜底链最终形态**：`config.context_window` > 缓存 caps `context_length`（v4.2，免费）> `/context probe`（一次请求，缓存进 config）> 单值形态。

验证（2026-11-08）：`test_v43_probe_parse`（三形态 + 括号取上界 + 回显值/越界/空体拒绝）通过；mock 新增探测拒绝路径后宽 PTY E2E 三断言 PASS——`/context probe` 出 `probed context window: 131072`、config 持久化 131072、下一 turn 状态栏变 `ctx N/131.1k`。agent_test 全量、CTest 3/3、strict-project、cpp_format 全过。

已知限制（并入 §9 语义）：D6——真实 DashScope/OpenRouter/vLLM 线上消息形态未逐一实测（mock 按维护者真机自述构造），首次真机探测若得 `[info] ... silent clamp` 或解析失败，按提示手工配置即可，无副作用。

## 14. v4.3.1 修订（2026-11-08 维护者追问：probe 拿到的到底是输入还是输出？）

追问戳中真 bug：网关的**第一个校验器先开火**，同一个超大 max_tokens 请求在不同网关报出的边界不是同一个东西——vLLM/OpenAI 报 `maximum context length`（总窗口），DashScope 却先报 `Range of max_tokens`（**输出上限**）。v4.3 不加区分地存进 `context_window`，qwen3.8-flash 会把输出上限 131072 当成窗口存下来（分母小 7.5 倍，ctx% 虚高）。

修正：`parse_context_probe_error` 返回 `ProbeOutcome{kind, value}`，kind ∈ Window / InputCap / OutputCap / Unknown：

- **Window / InputCap**：可作 ctx 分母，写入 config（提示语区分 "context window" / "input limit"）。
- **OutputCap**：只展示（`gateway reported its OUTPUT cap: 131.1k · not the context window`），**绝不写入** context_window。
- **Unknown**（含 `Range of <含糊参数名>`）：不猜，维持静默 clamp 提示。
- 顺带：所有用户可见的大 token 数统一 `format_token_count` 阶梯（<1k 原值、k、M、G），`/context` 与 `/config` 读数用"简写 (精确值)"双显（`983.6k (983616)`），config 语义要求可核对精确数。

验证（2026-11-08）：`test_v43_probe_parse` 重写为 kind 断言（三形态分类 + 含糊参数拒绝 + 回显值/越界/空体拒绝）；mock 的 DashScope max_tokens 拒绝路径 E2E 三断言 PASS——OutputCap 正确分类显示 `131.1k`、未持久化为窗口、`/context 983616` 后读数 `983.6k (983616)`。全量 agent_test/CTest/strict-project/cpp_format 通过。

## 最终核对

代码、测试、文档（src/README、development.md、.env.example）同批交付；本文件为实现轮记录，待人工评审后按流程更新状态。
