# mini-agent 技术方案 014：Provider 流前重试与退避

## 文档状态

- 状态：已实现（2026-11-14；验证见 §6，偏差 D1 见 §8）；待人工评审。
- 关联任务：2026-11-14 维护者批准的优化清单第 (a) 项——"provider 瞬时失败（429/5xx/网络抖动）一次即整轮失败，多工具回合丢弃全部已完成工作"。
- 核对日期：2026-11-14，基于 main `adcc4ff`。
- 人工评审记录：维护者 2026-11-14 指示"一个一个解决"优化清单（含本项）；非逐条书面评审。

## 1. 背景与目标

`OpenAiProvider::complete_chat` 目前对任何失败直接抛 `ProviderError`：一次 429 限流、502 网关抖动或连接重置就使整轮失败。`run_turn` 的 staging 语义（008 §5.2）意味着多工具回合中已执行的工具工作全部丢弃，用户必须重发。OpenAI 兼容网关的 429/5xx 大多是瞬时的，业界客户端普遍做有限重试。

目标：在**不改变流式语义、中断语义与失败轮回滚**的前提下，对"尚未向调用方交付任何内容"的瞬时失败做有限指数退避重试，并尊重 `Retry-After`。

## 2. 范围与非目标

范围：

- `complete_chat`（含 `complete` 委托路径）的重试循环；`ProviderConfig` 新增 `max_retries` 与 `on_retry` 通知钩子。
- env 旋钮 `MINI_AGENT_RETRIES`（0–5，默认 2；沿用 `MINI_AGENT_TIMEOUT_SECONDS` 的 env-only 模式，不动 config schema）。
- 纯函数判定/退避助手（可单测）；TUI/CLI 把重试记入元数据日志 `provider_retry`。
- 单测 + CLI pty smoke 的 flaky mock 场景（429×2 后成功、503×1 后成功、流中断不重试、`MINI_AGENT_RETRIES=0` 关闭）。

非目标：

- `list_models` / `probe_context_window` 不重试（ probes 已有独立超时与 TUI 侧流程；模型发现失败不丢工作）。
- 流开始后的断线重连/续传（SSE 无续传契约；保持失败轮回滚）。
- `Retry-After` HTTP-date 形式（只认整数秒；date 形式按缺省退避处理）。
- config.json 新字段、session schema 变化、新依赖。

## 3. 约束与参考资料

- 项目约束：AGENTS.md（小步修改、密钥不入日志）；007 §5.6 事件/渲染分离（重试通知不得经 `on_delta` 污染回复正文）；008 §5.2 失败轮回滚不变。
- 外部事实：HTTP 429/503 可携带 `Retry-After`（RFC 9110 §10.2.3，整数秒或 HTTP-date）；OpenAI 兼容网关普遍对 429/5xx 建议客户端退避重试。本设计只采用状态码语义这一稳定事实，不依赖任何网关私有错误文案。
- 无新参考来源登记（状态码语义属协议常识，按当前 libcurl 8.x 行为实现）。

## 4. 方案与关键取舍

**选定：单尝试逻辑不变 + 外层尝试循环 + "流已开始"门禁 + 指数退避（Retry-After 优先）。**

- **重试门禁（全部满足才重试）**：
  1. 错误可重试：`Network` 类（除取消外的所有 curl 错误），或 `HttpStatus` 且状态码 ∈ {408, 429, 500, 502, 503, 504}；`Protocol`/`Cancelled` 永不重试。
  2. `stream_started == false`：SSE `data:` 载荷一旦进入解析（无论内容/工具/错误载荷）即置位，此后任何失败不重试——调用方可能已看到部分输出，重试会造成重复正文。非 2xx 的 JSON 错误体不是 `data:` 行，不置位，因此 429/5xx 响应天然可重试。
  3. 还有剩余尝试次数（默认最多 3 次尝试 = 1 + 2 重试）。
  4. `should_abort()` 为假。
- **退避**：第 n 次重试（0 起）基础等待 `1s << n`（1s、2s、…）；响应带整数秒 `Retry-After` 时取 `max(基础, Retry-After)`；`Retry-After > 30s` 视为服务端明确拒绝短等待，**放弃重试**直接抛错。等待以 100 ms 分片睡眠并轮询 `should_abort`，中断即时抛 `Cancelled`。
- **取舍**：
  - 否决"流中断后重放整轮"：SSE 无续传契约，重放会重复已显示正文，违反 007 事件/渲染分离；保持失败轮回滚。
  - 否决"重试经 on_delta 提示"：会污染回复文本与 session 历史；通知走可选 `on_retry` 钩子 → 仅元数据日志（TUI/CLI 各自接线），UI 无新行（streaming 状态词已覆盖等待期）。
  - 否决 config.json 新字段：013 刚收敛配置面；重试次数属运行时调优，env-only 与 `MINI_AGENT_TIMEOUT_SECONDS` 同模式。
  - `max_retries` 钳制 0–5：防止误设大值造成长时间挂起；非法 env 值忽略（保持默认 2）。

## 5. 接口与数据变化

```cpp
// provider.hpp
struct ProviderConfig {
    // ...既有字段不变...
    int max_retries = 2;   // 0 关闭；实现钳制 0..5
    // 可选通知：第 attempt 次重试（1 起）、触发状态码（网络错误为 0）、等待毫秒。
    // 在调用线程（worker/CLI 主线程）同步触发；不得抛异常。
    std::function<void(int attempt, int http_status, int wait_ms)> on_retry;
};
// 纯函数，test-visible：
bool retryable_provider_error(ProviderErrorKind kind, int http_status);
// 返回等待毫秒；-1 = 放弃（Retry-After 超上限）。retry_after_s <= 0 视为缺省。
int retry_wait_ms(int retry_index, int retry_after_s);
```

- `complete_chat` 内部：请求体/endpoint 构建一次；每次尝试重建 headers 与解析状态（`full`/`tool_calls`/`usage`/`reasoning`/`SseLines`/`stream_started`/`retry_after`），新增 HEADERFUNCTION 捕获 `Retry-After`（大小写不敏感，仅整数秒）。curl 句柄复用不变（连接缓存跨尝试保留）。
- `AppConfig::to_provider()`：读 `MINI_AGENT_RETRIES`（0–5 合法，其余忽略）。
- TUI worker / CLI ask / CLI line-mode：`on_retry` → `provider_retry` 元数据日志（attempt/status/wait_ms）。TUI 侧经 `TurnBridge` 新增 retry 事件队列由主线程落日志（D1）；CLI 侧 logger 进程内稳定，直接引用捕获。零密钥、零正文红线不变。
- 无 config/session schema 变化；无请求体变化（重试重发同一 body）。

## 6. 验证计划与结果

| 项 | 方法 | 结果 |
| --- | --- | --- |
| 判定/退避纯函数 | agent_test 新增 `test_provider_retry_helpers`：可重试矩阵（Network/各状态码/Protocol/Cancelled）、退避序列、Retry-After 取大、>30s 放弃 | 通过（2026-11-14） |
| 429×2 后成功 + Retry-After | pty_cli_smoke `FLAKY429`：mock 前两次 429（Retry-After: 1），第三次成功；断言 exit 0、回复完整、服务端计数=3、日志含 2 条 provider_retry | 通过（2026-11-14） |
| 503×1 后成功 | pty_cli_smoke `FLAKY503`：一次 503 后成功 | 通过（2026-11-14） |
| 流开始后不重试 | pty_cli_smoke `MIDRST`：首个 content chunk 后 RST 断连；断言 exit 1、服务端计数=1（无重试） | 通过（2026-11-14） |
| 关闭开关 | `MINI_AGENT_RETRIES=0` + FLAKY429：exit 1、计数=1 | 通过（2026-11-14） |
| 既有契约不回归 | agent_test 全量、mcp_test、pty_tui_smoke、pty_cli_smoke 既有段、GCC/Clang Debug preset | 通过（2026-11-14） |

未验证：真实公网网关的 429 行为（mock 为 loopback HTTP）；Release preset 与 tidy/ASan 随 CI 门禁。

## 8. 实现偏差

- **D1（TUI 通知通道）**：§5 原写"on_retry 直接接 logger"。实现期核对发现 `/log off|default|<path>` 会在**回合进行中**重建 `logger`（unique_ptr，主线程独占），worker 捕获 logger 指针/引用会悬空。改为 `TurnBridge::push_retry_event/drain_retry_events`（与 tool events 同锁），主线程在既有 `drain_tool_events` 钩子内落 `provider_retry` 日志——跨线程只传数据不传 logger，符合 5.6 规则 1 的既有纪律；`reset_turn` 一并清空。单测覆盖队列 push/drain/reset。

## 7. 兼容性与限制

- 默认行为变化：瞬时失败从"立即报错"变为"最多 3 次尝试、最长约 3s+ 退避后报错"。脚本侧最坏时延增加有界（≤ 2 次退避 + 各次连接/总超时）；`MINI_AGENT_RETRIES=0` 恢复旧语义。
- 401/403 不重试（密钥错误重试无意义，AuthProbe 语义不变）；400 不重试（请求本身错误）。
- `Retry-After` HTTP-date 不解析（按缺省退避）；超过 30s 的 Retry-After 直接失败而非长等待。
- 重试对 usage 采集无副作用：每次尝试的 usage 状态独立，成功尝试的 usage 为准。