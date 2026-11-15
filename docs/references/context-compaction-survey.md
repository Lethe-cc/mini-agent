# Context 计量与压缩机制调研：Codewhale / Pi / Codex CLI / OpenCode / Claude Code

## 文档状态

- 性质：**设计参考**（017 上下文管理增强的输入），非本项目规范。采纳结论须进 `docs/design/017-*.md`，本文只保原始记录。
- 调研日期：2026-11-15。
- 方法：本机 checkout 源码实读（Codewhale/pi/codex/opencode）；**claude-code 公开仓库不含实现源码**（仅 docs/plugins/examples/CHANGELOG，实现为闭源 bundled npm 包，本机未安装），其结论仅基于 CHANGELOG 文档化行为，证据层单独标注。未运行任何被调研项目。
- 动因：维护者 2026-11-15 指示逐家分析五款终端 Agent 的上下文管理（起于 Codewhale 的 Context 面板截图），汇总为本报告，供 015（手动 /compact）之后的增强设计取舍。
- 分工：窗口大小判定链与 reasoning 档位见 [context-reasoning-survey.md](context-reasoning-survey.md)（2026-11-08）；TUI 布局/状态栏见 [tui-agent-ui-survey.md](tui-agent-ui-survey.md)（2026-11-06）。**本文不重复窗口判定**，只覆盖：用量计量、压缩触发、溢出检测与恢复、压缩策略、历史保全、摘要 prompt 纪律、上下文显示。
- 许可证：借鉴设计不复制代码。Codewhale/pi=MIT、codex=Apache-2.0（[coding-agents.md](coding-agents.md) 2026-11-05 登记）；opencode 许可证本轮未重核，沿用既有调研标注；claude-code 闭源，仅行为参考。

### 被调研项目修订表

| 项目 | 本地路径 | 修订 | 证据层 |
| --- | --- | --- | --- |
| Codewhale | /home/marvin/code/repos/AI/Codewhale | `284520c17`（2026-11-14） | 源码 |
| pi | /home/marvin/code/repos/AI/pi | `c3cc91632`（2026-11-15） | 源码 |
| Codex CLI | /home/marvin/code/repos/AI/codex | `b016cd1847`（2026-11-15） | 源码 |
| OpenCode | /home/marvin/code/repos/AI/opencode | `d432a06b9c`（2026-11-14） | 源码 |
| Claude Code | /home/marvin/code/repos/AI/claude-code | `c159118`（2026-11-15） | 仅 CHANGELOG |

行号对应该修订；引用格式 `文件:行`。

## 1. 各家发现

### 1.1 Codewhale：预算数学 + 显示/gate 单一事实来源

**显示**（Context 页，`crates/tui/src/tui/work_surface/views.rs:177-295`）：预算行 `used of max · %` + 24 格 gauge（`:298`）+ 阈值行 + 三行 breakdown（system+tools / conversation / tool output）+ 可选中的 `compact now` 行（Enter 执行 `/compact`）。breakdown **渲染路径零重扫**：逐消息读 token 缓存 ×1.5（`message_split`，`:316-344`）；消息的所有 block 均为 tool result → 计 tool output，否则计 conversation。

**计量**（`crates/engine/src/compaction.rs`）：双估算器——
- `estimate_input_tokens_conservative`（`:680-690`）：bytes/4 与 chars/3 取大（CJK 防低估，`:652-660`）× **1.5 膨胀** + 每消息 12 framing + 48，**只用于溢出保护**；
- `estimate_input_tokens_for_pressure`（`:702-712`）：不膨胀，用于触发与显示，优先 provider 账单 prompt tokens；`calibrate_token_estimate`（`:725-730`）用账单/估算比率校准。
- 显示快照 `context_usage_snapshot`（`crates/tui/src/tui/ui/frame.rs:2504-2564`）**优先本地估算**：reported 来自 `turn.add_usage` 的多轮累加，多工具回合后百分比跳变（#115，`:2539-2547`）；估算对历史增长单调。billed 高于估算时向上抬（`:2534`）。

**触发**：纯函数预算模块 `crates/runtime/src/context_budget.rs:34-75`：window − output 预留（≤window/4 且 ≤64K）− 1024 headroom = 可花费上限；trigger = 百分比 × window 再 clamp；`PressureLevel` 40/75/90%（`:82-135`）。配置默认 `auto_compact=false`、阈值 80%（`crates/runtime/src/settings.rs:626-628`，设阈值即视为开启）。发送前 preflight（`core/engine/turn_loop/preparation.rs:312-329`）。

**守卫**：`has_compactable_history`（`compaction.rs:979-982`）——1–2 条消息的超预算源于固定前缀或最新消息，压缩只浪费一次模型调用 → 拒绝；另有"压完降不到 trigger 以下就跳过"。

**压缩策略**：先**机械剪枝旧 tool output**（`prune_tool_results_until`，`:1089`：保护最近 N 条逐字节不动；重复结果留最新全文、旧副本一行摘要），再 LLM handoff 摘要；最近用户消息按 `retained_user_message_tokens` 逐字保留；摘要请求复用冻结 system+tools 前缀与相同 reasoning effort（缓存友好）；支持 `/compact <focus>`、operator `summary_instructions`、请求过大 size ladder。

**踩坑回归**：#6297 显示用 1.5× 而 gate 用不膨胀 → "ctx 82%" 但 auto 不动；测试名即纪律 `context_meter_and_compaction_gate_share_one_estimator`（`crates/tui/src/tui/ui/tests.rs:19103-19109`）。

### 1.2 pi：usage 锚点 + 尾部估算；不可变会话 + 投影压缩

**计量**（`packages/ai/src/utils/estimate.ts`）：chars/3.5（`:15`）、图片=4800 chars（`:16`）。`estimateContextTokens`（`:97-112`）= **最后一个有效 assistant usage（totalTokens，含 cacheRead+cacheWrite）+ 其后消息的启发式估算**。锚点纪律（`:71-95`）：跳过 aborted/error/全零；**时间戳失效**——锚点之后插入更新前缀（如压缩摘要）则旧 usage 作废。锚点处账单级精确、尾部估算、整体单调——同时治"纯估算不准"与"reported 多轮跳变"。

**触发**（`packages/coding-agent/src/core/compaction/compaction.ts:267-270`）：`tokens > contextWindow − reserveTokens`，**绝对预留**；默认 `enabled=true, reserveTokens=16384, keepRecentTokens=20000`（`:126-130`）。发送前 preflight（`core/agent-session.ts:830-837`）+ 回合后溢出恢复。

**溢出检测**（`packages/ai/src/utils/overflow.ts`）：26+ 条按厂商注释的正则（`:39-69`，含 DashScope `range of input length`）；**NON_OVERFLOW 排除集**（`:82-86`：Bedrock 限流文案 "Too many tokens" 会误中，先排除 rate-limit/throttling）；**静默溢出**（z.ai：stop 但 `usage.input+cacheRead > window`，`:160-165`）；**length-stop 溢出**（MiMo：截断输入后 `length` 且 output=0、input≥99% 窗口，`:170-175`）；`isRecoverableLength`（`:186-188`）。**溢出 ≠ 可重试**（`agent-session.ts:3885-3889`）→ 一次有界 compact-and-retry（`:3171-3183`）。

**压缩**：切点 `findCutPoint`（`compaction.ts:446-479`）从新到旧累加到 keepRecentTokens；**只切 user/assistant 边界，绝不切 toolResult**；切在回合中间时该回合前缀单独摘要（split-turn，`:931-942`）。摘要为结构化 checkpoint（`SUMMARIZATION_PROMPT`，`:507-538`：Goal/Constraints/Progress(Done·In Progress·Blocked)/Key Decisions/Next Steps/Critical Context，保留精确路径/函数名/错误串）；二次压缩迭代合并旧摘要（`UPDATE_SUMMARIZATION_PROMPT`，`:540-575`）；**文件操作硬事实**（readFiles/modifiedFiles 存 `CompactionEntry.details` 跨压缩累积，`:60-88`）。

**历史保全**：压缩不删历史——append-only entry log 追加 `CompactionEntry{summary, firstKeptEntryId}`，**投影** = 摘要 + 保留尾部；磁盘完整转录永在，可审计可分支（branch-summarization）。

**显示**（`modes/interactive/components/footer.ts:199-212`）：常驻 footer `12.3%/200k (auto)`，>70% 警告色、>90% 错误色。

### 1.3 Codex CLI：目录驱动限额 + 窗口基线 + 三种压缩形态

**计量/触发**：`auto_compact_token_limit` 随模型 catalog 服务端下发、config 可覆盖（`codex-rs/models-manager/src/model_info.rs:29-31`）；`effective_context_window_percent: 95`（`:133`）。**窗口级基线**（`core/src/state/auto_compact_window.rs:41-48,107-132`）：每压缩窗口记 `prefill_input_tokens`，**ServerObserved 永远替换 Estimated**；增长 = 当前 usage − 基线（`BodyAfterPrefix` scope）——pi 锚点思想的窗口化、服务端优先版。

**三种压缩形态**（同一 lifecycle，pre/post-compact hooks 统一观察）：
1. 本地摘要（`core/src/compact.rs`）：摘要 + **从新到旧保留 ≤20k token 的用户消息**（`COMPACT_USER_MESSAGE_MAX_TOKENS`，`:69`；`build_compacted_history_with_limit`，`:748-778`）+ always-retained 项；摘要以 `SUMMARY_PREFIX` 包装为"另一模型已开始解题的交接摘要"注入（`:408`；模板 `prompts/templates/compact/prompt.md`、`summary_prefix.md`）；`compact_prompt` 可配置覆盖（`tasks/compact.rs:85-95`）。
2. remote/server-side v2（`compact_remote_v2.rs`）：ChatGPT 后端服务端摘要，客户端安装替换历史。
3. token-budget（`compact_token_budget.rs:16-27`）：**不摘要**，直接开新上下文窗口。
- 另有 async compaction 后台预摘要（`session/async_compaction.rs`，`async_compact_threshold_percent`）。rollout append-only，压缩记为 `CompactedItem`；compact 测试套件 6740 行（`core/tests/suite/compact.rs`）。

### 1.4 OpenCode：measured+estimated、缓存感知预计算、最强摘要纪律

**计量**（`packages/core/src/session/compaction.ts:56-57`）：`Estimate = { measured, estimated }`——measured = 最近一次 provider 报告的 prompt 大小，estimated = 其后文本字符估算。**与 pi 锚点+尾部同构**（独立收敛）。

**触发**（`:42-50` + `packages/schema/src/config/compaction.ts`）：`buffer` 默认"**留 15% 且至少 16k**"（百分比+绝对混合）；`keep` 默认 15k 逐字尾部（`:269`）；`auto` 默认开，**auto 关时溢出也不自动压**（`:305-306`）。

**预计算**（`:111-171`）：接近阈值前后台预摘要（提前量固定 60k，`PRECOMPUTE_LEAD`）；**缓存感知**——只在 prompt cache 5min TTL 剩 ≥30s 时启动（`CACHE_LIFETIME/CACHE_MARGIN`）；换入时尾部超预算 40% 作废重摘（`PRECOMPUTED_TAIL_SHARE`）；连败 2 次停（`PRECOMPUTE_MAX_FAILURES`）。

**溢出恢复**（`:316-320,543-544` + `runner/llm.ts:319-325`）：被拒后按 `SHRINK_STEPS` 瞄准被拒请求大小的比例重试，且历史**改纯文本发送**（原样必再拒）。**native compaction**：provider 侧压缩，替换窗口存 `native.messages` 回放（schema `session-message.ts:272-276`）。

**摘要 prompt 纪律**（`:173-248`，五家最强）：
- 模板 Objective/Requirements/Decisions/Work State/Next Move/Files/Important Context；
- **不复述 AGENTS.md/环境设置**——"下一个 agent 单独收到这些的当前版本"（`:216`）；
- **未回答的用户问题逐字携带**，已解决不重复（`:210`）；保留工作流状态 uncommitted/committed/pushed/under review/merged（`:211`）；保留精确路径/符号/命令/错误串/URL（`:209`）；"不要提及压缩这件事"（`:212`）；
- 模型没填模板 → **NUDGE 提醒重试一次**才计失败（`:247-248`）；旧格式 checkpoint 一次性降级重写防摘要无限膨胀（`:230-239`）。

### 1.5 Claude Code（仅 CHANGELOG 证据，行号为 CHANGELOG.md）

- **per-model 自动压缩窗口**：`/autocompact` 按模型保存（L2599）；subagent frontmatter `autoCompactWindow` 可提前压缩（L1907）；未知模型假设 200K 并警告（L920）。
- **同款计量 bug**：context meter 把 advisor-tool 回合按约 2 倍计 → auto-compact 在真实窗口一半处 firing（L3860）。
- **同款守卫缺口**："no complete earlier exchange to summarize" 时会话永久卡死 "Prompt is too long" 的修复（L4055）。
- 溢出先**剥旧图片**再压缩（Bedrock，L959）；`/compact` 后转录折叠 "Compacted (ctrl+o to see full summary)"（L994）——摘要默认隐藏可展开。
- 边界：以上为发布说明记载的行为与修复，**无源码佐证**，不引用其内部实现细节。

## 2. 横向对比

| 维度 | Codewhale | pi | Codex | OpenCode | Claude Code | mini-agent 现状 |
| --- | --- | --- | --- | --- | --- | --- |
| 计量 | 估算优先+billed 抬升+校准 | usage 锚点+尾部 | 窗口基线（server 优先） | measured+estimated | 文档层：meter 曾 2× | reported 优先+纯估算 fallback |
| 触发表达 | %×预算数学（默认 80%） | 绝对 window−16k | catalog 限额+95% | max(15%,16k) 混合 | per-model 窗口 | 无自动；仅 400 后提示 |
| 默认自动 | off（设阈值即开） | on | on | on（off 时溢出也不压） | on | off（015 决策） |
| 溢出检测 | 模式集+size ladder | 目录+排除集+静默/length-stop | catalog/服务端 | 拒绝→shrink 阶梯+纯文本重发 | 剥图片+压缩 | 窄模式集（含 DashScope），仅 400 |
| 保留尾部 | 近期用户消息逐字 | 20k 全消息、回合边界 | ≤20k 用户消息 | 15k | 文档层未详 | 无（全量替换） |
| 先机械剪枝 | ✅ tool output | ❌ | （retained tool outputs 另策） | ❌ | 剥图片 | ❌ |
| 摘要迭代 | ✅ | ✅ +文件清单 | ✅ | ✅ +NUDGE+legacy 重写 | 文档层未详 | ❌（每次全摘） |
| 历史保全 | 内存替换 | entry-log 投影 | rollout append-only | DB | 文档层：history 仍保存 | **磁盘有损** |
| 后台预摘要 | ❌ | ❌ | ✅ async | ✅ precompute（缓存感知） | 文档层未详 | ❌ |
| 显示 | Context 页 breakdown+gauge | footer %/window 变色 | /status | TUI 消息流 | meter+ctrl+o | status 尾部 ctx + /context |

## 3. 行业收敛与共同坑

**收敛点**（≥3 家独立采用，可视为行业共识）：
1. **锚点+尾部计量**（pi/opencode 同构，codex 窗口基线佐证，Codewhale 以"估算优先+billed 抬升"逼近）——纯估算与纯 reported 都被实测否定。
2. **留输出余量再触发**（绝对/百分比/混合/目录限额四种表达，语义同一）。
3. **保留近期尾部逐字** + 结构化交接摘要 + 迭代更新。
4. **溢出→压缩而非重试**（pi/opencode/codex/claude；与 mini-agent 014 只重试瞬时错误的分工一致）。
5. **"压不动就别压"守卫**（Codewhale/claude）。
6. **磁盘历史不可变/可恢复**（pi/codex/opencode）。

**共同坑家族**（设计时必须设回归断言）：
- **显示/gate/真实前缀口径不一致**：Codewhale #6297（1.5× 显示 vs 不膨胀 gate）、pi #115（多轮累加 usage 跳变）、claude L3860（advisor 回合 2×）。三者同病：**计量口径分叉**。纪律：显示、触发、恢复共用同一读数函数。
- **环境依赖的显示宽度**：本项目 v7.2（007 修订）CI 长主机名挤掉状态词——与上述同属"显示契约隐含环境假设"家族。
- **摘要膨胀**：opencode legacy 重写（preserve-first 更新 prompt 使旧摘要细节无限携带）——迭代摘要必须允许"删除已失效信息"。

## 4. 对照 mini-agent 基线（main `7436714`，0.8.0）

已有：窗口三级链 config→gateway caps→models.dev（`src/tui.cpp:662-678`，详见 ctx-reasoning-survey §5）；reported 优先+`estimate_context_tokens` fallback（`:683-691`）；014 流前重试（瞬时错误）；015 溢出窄模式集（`src/provider_openai.cpp:480-497`，含 `range of input length`）+ 手动 `/compact`（有界转录、全量替换、失败不动历史）；v7.2 footer 状态保底。

缺口（按价值排序）：
1. 计量口径：reported 优先在多轮累加 usage 下有 pi #115 同款跳变风险 → 锚点+尾部。
2. `/compact` 全量替换：continuity 差、**磁盘有损**（原始转录被覆盖）。
3. 无 breakdown、无主动压力提示（只有撞墙后 400 提示）。
4. 摘要 prompt 无结构纪律、无迭代、无 NUDGE。
5. 溢出模式集无排除集（目前靠"仅 400"门禁兜底）。

## 5. 结论：017 建议范围（待设计文档立项）

**采纳**（括号内来源）：
1. 锚点+尾部计量，服务端 usage 优先、时间戳/前缀变更失效（pi/opencode/codex）。
2. `/context` 三行 breakdown：system / conversation / tool output（Codewhale；数据已在 chat，纯函数）。
3. 混合余量压力提示 `max(15%, 16k)`，**只提示不自动**（opencode 表达 + 015 非目标决策）。
4. `/compact` 升级：先机械剪旧 tool output（Codewhale）→ codex 式保留近期 user 消息 ≤20k + 最后一轮完整问答（切点不破坏 tool 配对，pi 硬约束）→ 结构化迭代摘要。
5. 摘要 prompt 移植 opencode 纪律：不复述系统提示/AGENTS.md、未答问题逐字携带、工作流状态、精确路径/错误串、不提压缩；NUDGE 一次重试；迭代时允许删除失效信息。
6. 压缩前原始转录备份（`<id>.pre-compact.json` 轻量中间态，不动 schema；pi/codex/opencode 不可变精神的降级版）。
7. 溢出模式集加 NON_OVERFLOW 排除（pi）。

**非目标**（规模产物或既有决策）：自动压缩本体、remote/native 压缩、async precompute、hooks、entry-tree/分支摘要、cache-zone 计费机器（Codewhale）、1.5× 双估算器（单一读数函数纪律即可）。

**风险与边界**：启发式非 tokenizer，CJK 仍可能低估（Codewhale `compaction.rs:652-660` 自认）；claude-code 结论仅文档层；opencode 许可证未重核——**只借设计不抄代码**；所有采纳项须按 017 设计走验证（单测+pty smoke 双环境）。

## 6. 使用注意

- 本文为原始记录；017 设计只引用实际采纳项并回链本文，不粘贴整份。
- 行号对修订表 SHA；被调研项目更新后涉及结论须重核，不把旧摘要当最新原文。
- 与 ctx-reasoning-survey 的窗口判定结论冲突时，以各自修订表日期较新者为准并说明。