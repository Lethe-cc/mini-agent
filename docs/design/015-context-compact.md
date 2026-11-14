# mini-agent 技术方案 015：上下文溢出提示与手动 /compact

## 文档状态

- 状态：已实现（2026-11-14；验证见 §6）；待人工评审。
- 关联任务：2026-11-14 维护者批准的优化清单第 (b) 项——长编码会话撑爆上下文窗口时用户只看到裸 400 错误，且无任何历史压缩手段（007 §5.5 预留的压缩扩展点首次落地）。
- 核对日期：2026-11-14，基于 main `5111b1d`。
- 人工评审记录：维护者 2026-11-14 指示"一个一个解决"优化清单（含本项）；非逐条书面评审。

## 1. 背景与目标

两个缺口：

1. 网关以 400 拒绝超长上下文时，`ProviderError` 只带 "HTTP status 400"（错误体被丢弃），用户无法区分"上下文爆了"与"请求参数错了"，也没有下一步指引。
2. 历史只增不减：长会话（文件读入 + 工具输出）必然撞窗口，撞了只能 `/session new` 丢掉全部上下文。

目标：溢出错误**可识别、有指引**；提供**手动** `/compact` 把历史浓缩为一条摘要消息，保留工作连续性。自动压缩仍为非目标。

## 2. 范围与非目标

范围：

- provider 非 2xx 错误体提取（`error.message`，有界捕获 + 截断 + 净化）；纯函数 `context_overflow_message()` 窄模式识别。
- TUI 回合失败路径与 CLI 错误输出的溢出 `[hint]`。
- TUI `/compact`：独立短命 worker（probe 同款纪律）请求摘要 → 主线程替换历史 → 保存；`Chat::replace()` 与纯函数 `build_compact_request()`。
- 单测 + pty TUI smoke 新场景（OVERFLOW 400 提示、compact 全流程）。

非目标：

- 自动压缩/阈值触发（007 非目标维持；手动先行）。
- CLI line-mode 的 /compact（一次性脚本场景无长会话压力；TUI 专属，文档注明）。
- 摘要质量调优、多轮迭代压缩、压缩前备份文件（session 原子写 + git 已是兜底；摘要失败历史不动）。
- `list_models`/probe 的错误体提取（只改 chat 路径）。

## 3. 约束与参考资料

- 项目约束：007 §5.6 事件/渲染分离（网络不得在主线程；compact 走 worker）；008 §5.2 失败不污染历史（compact 失败历史原样）；密钥/正文日志红线（compact 日志仅元数据：前后消息数、摘要字符数）。
- 外部事实：OpenAI 兼容网关溢出错误文案不稳定（OpenAI `context_length_exceeded`/"maximum context length"、vLLM 同族、DashScope "Range of input length"），与 010 §13 probe 解析同一处理哲学：**窄模式集 + 未识别不猜**。模式集复用 probe 已核对的文案家族，不新增外部来源。

## 4. 方案与关键取舍

**A. 错误体提取**：`CurlWriteCtx` 增加有界原始缓冲（前 4 KiB）；非 2xx 时解析 JSON `error.message`（兼容顶层 `message`），截断 200 字符、`sanitize` 控制字符后拼入错误文本：`HTTP status 400: <message>`。前缀不变，既有 `in` 断言不回归。SSE 流内错误载荷路径不变。取舍：不提取非 JSON 体（网关 HTML 错误页无信息量，避免噪音）。

**B. 溢出识别**：`context_overflow_message(text)` 大小写不敏感子串窄模式集：`context_length_exceeded`、`maximum context length`、`context length`、`reduce the length`、`range of input length`、`too many tokens`。命中且 HTTP 400 → TUI `[hint] context window exceeded · /compact summarizes history into one message, or /session new starts fresh`；CLI stderr 同款。400 但无命中 → 不提示（不猜）。

**C. /compact 流程**（TUI）：

1. 门禁：streaming/审批待决/compact 进行中 → 拒绝；非 System 消息 < 2 → `[info] nothing to compact`。
2. `build_compact_request(history)`（纯函数）：保留首条 System；单条 User = 固定指令 + 转录。转录逐条 `role: content`（工具消息含名字），**每条截断 1500 字符**；总量超 **60000 字符**时保头保尾、中间以 `[... middle omitted ...]` 占位——摘要请求自身不能再次撞窗口。
3. 短命 `compact_worker`（probe 同款 join-then-read）：`provider.complete()` 无工具、`should_abort` 接 `abort_requested`；状态行显示 `compacting...`；Ctrl-C/Esc 可中断（中断键路径把 compacting 视同 streaming）。
4. 主线程完成时：成功 → `session.chat.replace({System?, User("[conversation summary]\n" + summary)})`、保存（temp 跳过）、usage 读数重置（`reset_usage_readings`）、`[ok] compacted N messages into a summary`；失败/中断 → 历史不动、`[error]/[interrupted]` 行。日志 `compact status=ok|error|aborted before=N after=N summary_chars=N`（零正文）。
5. 摘要消息用 **User** 角色 + 显式标记：不伪造 Assistant 发言，模型下轮自然把它当背景材料；无工具数据 → session schema 不升级。

取舍：

- 否决"压缩走 run_turn"：摘要不是对话回合，不应进工具循环/审批/usage 语义；独立 worker 更简单且不污染回合记账。
- 否决"保留最近 K 轮原文 + 摘要其余"：实现与测试面翻倍；首版全量摘要，保留策略留作后续（§7）。
- 否决自动触发：阈值判断依赖 usage 可靠性（网关差异大），误触发会静默丢上下文；手动命令把控制权交给用户。

## 5. 接口与数据变化

```cpp
// chat.hpp
class Chat { void replace(std::vector<Message> messages); }; // 整体替换（compact 专用）
// 纯函数：构造摘要请求（System 保留 + 指令 + 有界转录），test-visible
Chat build_compact_request(const Chat& history);

// provider.hpp
bool context_overflow_message(std::string_view text); // 窄模式集，大小写不敏感
```

- `ProviderError(HttpStatus)` 文本追加网关错误消息（截断/净化）；`http_status()` 语义不变。
- TUI：`/compact` 进命令表与补全；状态行新增 `compacting...` 状态词；中断键覆盖 compacting。
- 无 config/session schema 变化；请求体形状不变（摘要就是一次普通 complete）。

## 6. 验证计划与结果

| 项 | 方法 | 结果 |
| --- | --- | --- |
| 溢出模式矩阵 | agent_test：六类命中 + 大小写 + 非溢出 400 文案不命中 | 通过（2026-11-14） |
| build_compact_request | agent_test：System 保留、逐条截断、总量保头保尾、空历史 | 通过（2026-11-14） |
| Chat::replace | agent_test：替换后消息数/内容、JSON 往返 | 通过（2026-11-14） |
| OVERFLOW 提示 E2E | pty TUI smoke：mock 400 + context_length_exceeded → `[hint] context window exceeded` | 通过（2026-11-14） |
| compact 全流程 E2E | pty TUI smoke：两轮对话 → /compact → `[ok] compacted`、session 文件含摘要且消息数收缩、下一轮正常 | 通过（2026-11-14） |
| 既有契约不回归 | agent_test/mcp_test 全量、pty TUI/CLI smoke、GCC/Clang Debug | 通过（2026-11-14） |

未验证：真实网关的溢出文案覆盖（模式集外不提示，属有意保守）；摘要质量（模型行为，非本设计契约）；Release preset 与 tidy/ASan 随 CI 门禁。

验证补充（2026-11-14）：TUI smoke 167 PASS / 0 FAIL（新增 8 断言：compact 空历史 no-op、摘要替换、持久化形态=系统提示+标记摘要、后续回合可用、OVERFLOW 提示与网关消息透出、compact 元数据日志红线）；CLI smoke ALL PASS；GCC/Clang Debug CTest 3/3。E2E 核对确认：默认 system prompt 在 compact 后保留（§4C 第 5 条设计如此）；日志断言须在进程退出后读取（spdlog 退出时刷盘）。

## 7. 兼容性与限制

- 错误文本变化：`HTTP status N` 后可能追加网关消息——按前缀匹配的既有断言不受影响；精确全文匹配的消费者需知晓（仓库内无此类断言，核对见验证）。
- /compact 有损：细节（精确代码片段、长工具输出）可能丢失；摘要请求自身截断（1500/条、60000 总量）进一步限制保真。命令输出明示"summarizes history"，用户知情。
- 摘要请求本身可能失败（网络/鉴权/窗口仍超）：历史不动，错误可见；可重试或 /session new。
- 后续候选：保留最近 K 轮原文、自动阈值建议（只提示不执行）、压缩前导出备份。