# 008：agent 产品内置 git 工具取"commit 可自动（随审批档位）+ 永不提供 push + 远端操作留人工"

- 状态：已接受（核心策略获维护者 2026-11-05 对话批复："git 提交可以自动，push 等人工操作"；细则解释见"决策"节标注的 AI 假设）。
- 日期与关联任务：2026-11-05；[009：mini-agent v3 技术方案](../design/009-coding-v3.md)。
- 决策人或审批依据：维护者 2026-11-05 对话批复（针对 009 开放问题 Q2/Q4 的回答）。原稿为 AI 按 AGENTS.md 红线起草的从严默认（写侧恒逐次审批），批复将其放宽为"commit 可自动、push 永远人工/不提供"。

## 背景

维护者 2026-11-05 将"git 能力"列入 v3 范围。两层约束交织：

1. AGENTS.md"不自动提交、推送"约束的是 **AI 助手在本仓库的开发流程**；mini-agent 作为产品是否内置 git 工具属另一问题（009 Q4 待确认）。
2. 产品自身无沙箱，git 写操作（commit）产生持久历史副作用，`push` 更将未审内容送出机器——后者无论哪层约束都不可接受。

`run_bash` 已能跑任意 `git` 命令（走审批），"git 工具集"的价值在于：审批块的结构化可读性（文件清单 + staged diff 预览而非命令串）、参数 fail-closed（message 不进 shell）、以及 allowlist 档下只读操作的自动化空间。

## 备选方案

- 方案 A（只读三件套 status/diff/log + 写侧 add/commit 恒逐次审批含 diff 预览 + 不提供 push/remote）：git 工作流闭环（改码→验证→提交）能在 TUI 内完成，push 留给人；审批摩擦集中在唯一有远端前风险的步骤之前。
- 方案 B（纯只读）：最保守；用户明确要"git 能力"，commit 也做不了等于半个功能，且模型仍可经 `run_bash` 审批路径 `git commit`——规避不了的能力只会绕道出现，不如正面做成可审计形态。
- 方案 C（commit 与 push 都提供，走审批）：push 审批难以展示"远端将收到什么"（网络核对成本与 OAuth 裁剪冲突），且送出未审内容风险不可回滚，违背 AGENTS.md 精神。
- 方案 D（不做专门 git 工具，只靠 run_bash）：零实现；放弃结构化审批块与 allowlist 安全档，与"diff 显示修订"诉求背道而驰。

## 决策

按维护者批复（"commit 可以自动，push 等人工"）采用方案 A′：

1. `GitRuntime` 五工具：`git_status`/`git_diff`/`git_log`（只读，无副作用）+ `git_add`/`git_commit`（写侧）；
2. **写侧审批粒度（批复落定）**：`git_commit`/`git_add` 与文件工具同等对待，走 `tool_approval` 统一策略——`ask` 逐次确认（默认）、`allowlist` 可按工具名命中免问、`off` 全自动；`AllowSession`（`a` 键）对写侧 git 工具生效（同普通工具）。原稿"恒需逐次审批"的从严默认被批复放宽，此为 AI 对批复范围的解释：批复明示 commit 可自动，未区分档位；
3. 自动 commit 的补偿性可见性：commit 无论走哪档审批，执行后历史区 `[tool]` 行常显 `git_commit` 摘要，且审批块默认视图 = 受影响文件清单 + staged diff 前 N 行 + commit message 全文（复用 009 §5.3 渲染）——免问模式下用户仍能在历史里回看每次提交内容；
4. **永不提供** push/pull/fetch/remote/reset/rebase/clean/checkout 类工具；模型请求时 unknown tool 如实回灌。"push 等人工操作"由此落实为工具面缺席 + `run_bash` 审批兜底（off 档下 `run_bash "git push"` 仍可达远端——见"后果·风险"）；
5. 执行 = fork/exec `git -C <workspace> --no-pager` 固定 argv（模型文本只进 message 参数字位，绝不经 shell）；非 git 目录时整个 GitRuntime `alive()=false` 缺席注册表；
6. **开发流程红线同步放宽**：维护者批复同时作用于 AGENTS.md"不自动提交、推送"条款——本仓库内 AI 助手现可自主 `git commit`（含本次批复文档的提交），但 **push 永远等人工**。AGENTS.md 该条已按批复改写为"不自动推送或创建付费资源；提交须经批准或维护者预授权"。

## 后果

- 收益：agent 完成"改码 → 跑测试 → 本地提交"全自动化闭环（off/allowlist 档），TUI 默认仍有 ask 摩擦保护；提交行为始终有文件清单 + diff + message 三要素可回看；参数注入面关闭（不经 shell）。
- 代价：又一 runtime 与测试面（临时 `git init` 仓库内跑，无网络）；AGENTS.md 红线文本需同步修订（已做）。
- 风险：① 免问档下自动 commit 污染历史——缓解：不提供 reset/rebase，收拾权留给人；commit message 与 diff 入历史区可回看；② **off 档下 `run_bash` 仍可 `git push`**——批复"push 人工"在 off 档只能靠警示行与文档约定，工具层无法拦截（与 006 对 off 的既有风险声明同级）；若需硬保证，push 需列入 `run_bash` 命令黑名单，该增强未获批复、暂不实现；③ 本仓库 AI 自主 commit 后误提交垃圾——缓解：commit 前 `git status` 复核纪律 + push 人工兜底。
- 后续验证：009 第 8 节 `test_git_tools`（注入、非仓库、push 缺席、allowlist/off 免问路径）；pty smoke commit 审批场景。

## 关联
[009](../design/009-coding-v3.md) 第 5.2/6 节；[ADR 006](006-tool-approval-policy.md)（审批框架）；[ADR 007](007-file-tools-workspace.md)（同一工作区边界校验复用）。
