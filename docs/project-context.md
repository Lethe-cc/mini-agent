# 项目背景

## 初始化信息

以下三个占位符仅由初始化脚本精确替换一次。其他内容由项目维护者补充。

- 项目名称：mini-agent
- 一句话目标：a mini ai agent
- 技术栈：C++

## 用户与场景

希望编写一个轻量级 AI Agent，目标是能快速启动，支持 CLI 和 TUI 两种交互方式。

使用场景：开发者在 Linux 终端启动 mini-agent，通过命令行单次执行任务（CLI 入口），或进入交互式终端界面（TUI）与 Agent 多轮对话；需要同时运行多个会话（多开 session），会话之间互不干扰。

初步目标：

1. 启动快速，优先 C++。
2. 提供 CLI 入口和 TUI 界面。
3. 能多开 session。

## 范围与非目标

本期必须交付（依据维护者 2026-10-31 描述，细节待设计阶段确认）：

- C++ 实现的 Agent 内核：与 LLM 服务交互的最小会话循环（提示 → 模型响应 → 下一轮）。
- CLI 入口：非交互方式启动，适合脚本调用与单次任务。
- TUI 界面：终端交互界面，支持多轮对话。
- 多 session：可同时开启多个会话，会话状态相互隔离。
- Linux 上的 CMake 构建与测试（CTest）。

明确不做（AI 整理的假设，需维护者确认）：

- 不支持 Windows 与 macOS（模板本身也不提供 Windows 支持）。
- 不做 IDE 插件、Web 界面或远程服务部署形态。
- 不做模型训练或本地推理引擎；LLM 能力通过外部 API 提供。
- 不做插件市场、多用户权限体系等企业级能力。
- 不追求功能完整的编码 Agent（如自动仓库级重构）；对标设计参考见 [references/](./references/) 的 agent 条目。

开放问题（2026-11-02 全部已定案，记录于 ADR 与设计文档）：

- "多开 session" 形态：一进程一会话、多开 = 多进程、文件锁防同 id 并发（[ADR 001](decisions/001-session-model.md)）。
- LLM 服务方与协议：OpenAI 兼容 Chat Completions（HTTPS + SSE）；密钥 env 优先 + TUI `/login` 持久化 0600 配置文件，不入仓库（[ADR 002](decisions/002-http-json-dependency.md)）。
- TUI 实现选型：自绘最小 TUI（POSIX termios + ANSI，零依赖）（[ADR 003](decisions/003-tui-approach.md)）；Markdown 渲染用 vendored md4c（[ADR 004](decisions/004-md4c-parser.md)）。
- CLI 与 TUI 共用同一内核入口：是，`loop::run_turn(Chat&, Provider&)` 参数注入（[007 §5](design/007-agent-v1.md)）。

## 约束

- 运行平台：Linux。
- 构建：CMake。
- 语言优先级：C++（快速启动是首要体验目标，冷启动应尽量减少运行时依赖与初始化开销）。
- 依赖策略：沿用模板规则——小步修改、不无故新增依赖；新增第三方库前必须核实许可证、署名及分发义务，并记录 ADR。
- 密钥策略：遵循 AGENTS.md——不读取真实密钥进仓库，不在文档或 URL 中放 token。
- git提交策略：使用英文commit, 推送到远端需要确认

注：`project.json` 的 `language` 已按迁移流程接入 cpp profile（2026-10-31，在临时生成后合并骨架，未重跑 `init_project.py`），`quality_commands` 为 CMake configure → build → CTest。

## 成功标准

可验证的验收标准（草案，数值待维护者确认后进入设计文档）：

1. 冷启动：构建产物在无交互模式下从进程启动到就绪（打印首个提示符或接受首个请求）不超过 100 ms（不含网络往返）；可用脚本化计时测量。（2026-11-14 已落地：`tools/cold_start_check.py`，CI `smoke` job 门禁；本地实测中位数 ~5ms，见 architecture.md 验证入口）
2. CLI：单次调用成功时输出模型响应、失败时以非零退出码报告错误，可被 shell 脚本组合使用；有对应自动化测试。
3. TUI：进入交互界面后至少完成一轮"输入 → 响应 → 再输入"的对话；异常退出后会话数据不损坏。
4. 多 session：同时运行 ≥ 2 个会话，各自对话历史互不可见；一个会话崩溃不影响其他会话。
5. 构建与质量：`python3 scripts/check.py --strict-project` 通过，覆盖 CMake configure → build → CTest；Linux GCC 与 Clang 至少各验证一种 preset。

第 1、5 条可在 CI 自动验证；第 3、4 条需要人工或脚本化终端测试并在交付说明中给出证据。

## 项目状态

2026-10-31：维护者补充用户与场景、约束和初步目标；"范围与非目标"的不做清单与"成功标准"数值为 AI 依据描述整理的假设，待维护者确认。同日已接入 cpp profile（`project.json` 的 `language` 改为 `cpp`，`quality_commands` 为 C++ configure/build/CTest；骨架为 starter 示例，非业务代码），GCC Debug 与 Clang Debug preset 均通过 `--strict-project`。第一版技术方案见 [design/007-agent-v1.md](design/007-agent-v1.md)（草案），会话形态、HTTP/JSON 依赖、TUI 选型三条 ADR 见 [decisions/](decisions/)（均提议）；2026-11-01 第一步开发完成：chat/session/provider/loop/CLI 实现并通过 GCC/Clang 双 preset、clang-tidy -Werror 与本地 mock SSE 端到端冒烟；自绘 TUI（ADR 003，5.6 帧模型与交互契约）已实现并经真实 pty 冒烟通过（run 在非 TTY 自动回退 line-mode）；Pi 式 streaming 排队 steering 已提前落地；免环境变量配置流（/login /model /provider）已实现并经 pty 全链路验证；独立日志文件（spdlog）默认开启写 /tmp/mini-agent-<uid>.log（0600，uid 隔离；MINI_AGENT_LOG 或 TUI /log 可改路径/off），日志保证不含密钥与正文；TUI 助手回复支持 Markdown 子集渲染（md4c 解析 + 自写 SGR writer，ADR 004，含 GFM 表格/任务清单）；TUI 与 line-mode 均支持 /exit 退出会话（保存+释放锁+恢复提示）。其余开放问题仍以人工评审结论为准。2026-11-05：v2 全部实现合入后，维护者提出下一阶段目标"能否进行代码修改、项目开发"，范围含改码改文档、git 能力、diff 显示修订；盘点结论为"能改但缺文件编辑工具/工作区/system prompt/git 工具面"。因维护者离场委托务实默认，AI 按 design-first 产出 [009 v3 设计草案](design/009-coding-v3.md)（未实现）与 [ADR 007](decisions/007-file-tools-workspace.md)、[ADR 008](decisions/008-git-tool-policy.md)（均提议），git 策略默认取"只读自由 + add/commit 恒逐次审批含 diff 预览 + 永不 push"；009 第 9 节开放问题中 Q2/Q3/Q4 已获同日批复（"git 提交可以自动，push 等人工操作"，ADR 008 升为已接受；AGENTS.md 红线条款同步改为"提交可自动、推送仍人工"），Q1/Q4b/Q5 仍开放；实现启动仍待维护者确认（未获"开始实现"指示）。同日维护者指示"开始实现"后，v3 M1–M4 全部落地：`src/tools/diff.cpp`（行级 LCS unified diff）、FileRuntime 五工具 + 软性 workspace 边界（`resolve_within_workspace`）、GitRuntime 五工具（惰性探测、无 push 面，ADR 008 已接受）、`ToolRuntime::preview/disarm` 审批竞态守卫、TUI diff 审批块（+绿/-红/@@ magenta）与 `/cwd`、默认 system prompt（仅新 session 注入，`Chat::update_system_workspace` 随行更新）、CLI `--cwd` 与 `--tools allowlist` 档、`run_subprocess` 进程管理去重。config/session 以可选字段前后兼容（未 bump schema，偏差 D6）。验证：GCC/Clang debug + tidy -Werror + ASan、agent_test 新增 5 个用例（diff/文件工具/v3 配置与 prompt/审批行渲染/git 工具）、pty smoke 75 项全绿（含 v3 审批 diff 场景与日志红线扩展）、mock CODEFIX 多轮编码流 E2E（auto 与 allowlist 两档，toy 仓库文件实际修改并 commit）。真机 LLM 编码冒烟已由维护者 2026-11-05 实测通过（qwen3.8-flash TUI：run_bash 取 git 输出、正确区分 edit_file 审批基线与 git commit 基线的双 diff 语义，记录见 009 §9）。ADR 007 状态"已实现待评审"。
2026-11-14：v7（provider 强制命名与配置结构统一）实现合入：维护者提议"不允许不加 name 就添加 provider、顶层 base_url/api_key/model 用统一 current 结构表示、不考虑旧配置兼容"，AI 按 design-first 产出 [013](design/013-named-providers.md) 与 [ADR 009](decisions/009-named-providers-schema-v2.md)，维护者批复 Q1–Q4 全部推荐默认并指示实现。config schema 升 2：顶层 LLM 五字段废弃，`providers`+`active_provider` 为唯一持久化面；运行时 `current` 单向派生（活动档案 + env 叠加，env 恒赢，顺带修复 012 env 覆盖下切换档案丢 env 值的缺陷）；`sync_active_provider()` 删除；无档案时持久化命令自动建 `default` 档案；v1 文件一次性加载迁移（未命名顶层折 default，名冲突 default-2）；rm 活动档案改为切换剩余首个档案。验证：四 preset ctest 3/3 + tidy + ASan/UBSan、pty smoke 156 PASS（3 项既有失败与本期无关）、CLI smoke 全绿、双 provider mock E2E 含 rm 后续聊。旧二进制读 v2 文件被拒绝（破坏性，有意；升级前备份 config.json）。待人工评审。
2026-11-14（同日第二批）：维护者批准优化清单并指示"逐项实现+补文档+提交"，当日合入 8 个提交：(1) 修复 pty smoke 三项既有失败（根因：v4.16 合并 footer 补到恰好 cols 触发终端 deferred-autowrap 破坏行记账 + 状态词被裁剪；行宽不变量收紧为 ≤cols-1、状态词前置，007 v7.1 修订）；(2) [014](design/014-provider-retry.md) 流前重试（网络/408/429/5xx，指数退避 + Retry-After，流已开始门禁，`MINI_AGENT_RETRIES`，TUI 经 TurnBridge 队列落 provider_retry 日志）；(3) [015](design/015-context-compact.md) 溢出错误体提取 + 窄模式提示 + 手动 /compact（独立 worker、有界转录、失败/中断不动历史）；(4) CI 新增 smoke（pty 两套件 + 冷启动）与 quality（tidy+ASan）job；(5) 冷启动预算脚本化（tools/cold_start_check.py，实测中位数 ~5ms，成功标准 #1 落地）；(6) 审批 `a` 键 scope 显式化（图例 + 按下回显警示，语义不变）；(7) README 同步 v6/v7/014/015 + 新增 docs/usage.md 日常使用指南；(8) [016](design/016-session-lifecycle.md) session export/prune；(9) tui.cpp 行为保持拆分（tui_render.cpp 纯函数 ~940 行）。另核实：MCP stdio 崩溃自动重启早已实现并有测试（v4.17，原分析误判，未改动）。最终验证：四 preset ctest 3/3、tidy+ASan/UBSan 退出 0、pty TUI 171 PASS / 0 FAIL、CLI ALL PASS、check.py 全绿。远端 CI 待推送（人工，ADR 008）；014–016 与 v2–v5 同样待人工评审；ADR 005/006 批准仍待维护者。
2026-11-15：版本 0.8.0（CMake 嵌入 commit/构建时间；CLI `version` 子命令 + TUI `/version` + 启动横幅带版本）；007 v7.2 修订修复 CI 独有的两项 smoke 失败（根因：footer 状态预算随机器 hostname/cwd 宽度漂移，长主机名挤掉状态词；修复：状态保底 24 列、sysinfo 先裁，本地以 USER 加宽精确复现后转绿）；六家上下文管理调研入库（[ctx-compaction-survey](references/context-compaction-survey.md)：Codewhale/pi/Codex/OpenCode 源码层 + Claude Code CHANGELOG 层 + Zed 源码层，PlantUML 流程图规范确立）；[017](design/017-context-management.md) 实现合入：锚点+尾部单一读数、/context breakdown、max(15%,16k) 压力提示（只提示不自动）、/compact 升级（0600 备份→回合边界保留尾部→机械剪枝→结构化迭代摘要+NUDGE）、溢出排除集 + Exceeded 合成、session rm/prune 清理备份。验证：test_context_017 28 断言、四 preset ctest 3/3、pty TUI 180 PASS 双环境、CLI ALL PASS、check.py 全绿。017 待人工评审；Q1–Q3 采用设计默认（备份仅留最新/无阈值旋钮/摘要播种后续）。
2026-11-21：019 P4 收官（键位分发 → `tui_keys.hpp`，LoopCtl 方案，提交 `c1a57f3`；tui.cpp 1694→1268，P1–P4 全部完成，四 preset + pty TUI 195/0 双环境 + CLI + strict-project 全绿）。同日维护者批复 **[ADR 006](decisions/006-tool-approval-policy.md) 方案 A**（默认 ask 逐次人审、allowlist 按工具名、off 显式配置 + auto-tools 警示），状态升为已接受——实现自 11-04 起即按此运行，本次为正式追认；009 Q4b（off 档 run_bash push 黑名单）不随本批复定案，仍开放。同日维护者批复 **[ADR 007](decisions/007-file-tools-workspace.md)"使用建议的方案"**（A：edit_file 精确唯一替换五工具 + D：软性 workspace 边界 + session 级 workspace 归属），状态升为已接受——实现自 11-05 起按此运行，本次为正式追认；design 009 整体仍待评审。ADR 005 与各设计文档的人工评审、009 Q1/Q4b/Q5、017 Q1–Q3 默认追认、远端推送/CI 首跑仍待维护者。
2026-11-21（同日第二批）：维护者确认 MCP 方向"未来要 resources/prompts/sampling/elicitation/tasks/追新版本"，推翻 ADR 005 的 tools-only 裁剪与版本钉死前提。AI 完成四个 C++ SDK 候选的深化核对（cxxmcp/mcp-cpp/mcp-cpp-sdk/gopher-mcp，元数据+许可证+构建层，cxxmcp 另读 client 头文件；源码未精读、conformance 系自报），按 design-first 产出 [design 021](design/021-mcp-full-client.md)（cxxmcp 迁移：M0 spike 否决门 → M1 双轨底座替换 → M2 能力分期 → M3 收尾）与 [ADR 010](decisions/010-mcp-full-client-cxxmcp.md)（提议取代 005、修订 002：vendored cxxmcp+cpp-httplib+系统 OpenSSL，LLM 仍 libcurl 双栈共存）——**均待评审，未获批、未引入任何第三方代码、spike 未执行**；开放问题 Q1 依赖批准/Q2 OAuth 裁剪存续/Q3 迁移期版本基线/Q4 vendoring 形态。ADR 005 标注"拟被 010 取代"（未批前仍是现行实现事实）；008/references/agent-protocols 同步更新核对记录。另修正两处过期索引（design/README 019 行、019 文档状态头：P1–P4 已全部落地）。ADR 010/021 评审、009 Q1/Q4b/Q5、017 Q1–Q3、远端推送/CI 首跑仍待维护者。
2026-11-21（同日第三批）：维护者批复 021 开放问题——**Q1"批准"**（[ADR 010](decisions/010-mcp-full-client-cxxmcp.md) 升为已接受，[ADR 002](decisions/002-http-json-dependency.md) 修订获批：MCP 子系统豁免零新增依赖，登记 vendored cxxmcp+cpp-httplib+系统 OpenSSL）；**Q2"延续，但是未来可能会拓展"**（OAuth 裁剪延续，auth 目标保持 OFF，未来启用须另行人审设计）；**Q3/Q4"ok"**（M1 期钉 2025-06-18、client 子集裁剪 vendoring）。[ADR 005](decisions/005-mcp-transport-abstraction.md) 状态改为"已被 010 取代"（021 M3 前自写三层仍是现行实现，git 历史作回退底座）。[design 021](design/021-mcp-full-client.md) 进入 M0 spike：`.references/` 内 clone 固定 SHA、四 preset 编译、mock/deepwiki 重放、conformance 独立复跑、并发桥接验证——spike 不达标即回自写扩展路线；引入 `third_party/` 仍受 spike 通过 + 逐仓库许可证核实双门约束。
2026-11-21（同日第四批）：[design 021](design/021-mcp-full-client.md) **M0 spike 执行完毕并判定通过**（证据表落其 §8）。cxxmcp 固定 SHA `3c25a8afef0ae77031bcb7a3142bcca67d4cd3d6`（`.references/`，gitignored）：四 preset（GCC 14.2/Clang 19.1 × Debug/Release）client-only+HTTP+OpenSSL 编译全过；stdio mock 8/8、HTTP mock（含 ping 传播/DELETE 恢复）、deepwiki 实测（3 tools）、4 线程×25 并发 100/100 重放全绿（协商均 2025-06-18）；ASan+LSan、TSan 三场景各干净。官方 conformance runner（v0.2.0-alpha.12，本地 Node 22）独立复跑：**`--spec-version 2025-11-25` 同口径 247 passed/0 failed，与上游自报 247/0 精确一致**；最新 wire `--suite all` 42/48 场景 463 SUCCESS/0 FAILURE（4 SKIP 为设计内）。6 个 draft-auth 拒绝场景挂起，gdb 根因定位为上游 `Client::~Client()` 非 RAII in-flight 计数在异常展开时残留导致析构死等（021 §9 已知限制；触发条件 auth ON + draft stateless wire 均在 M1 授权外，Q2/Q3）；客户端拒绝行为本身正确。许可证核实完成（cxxmcp/cpp-httplib/nlohmann MIT、tl CC0），[ADR 010](decisions/010-mcp-full-client-cxxmcp.md) 前提 3 更新为"核实完成、归档待 M1"。**未向 third_party/ 引入任何代码、仓库行为零变化；M1 vendoring 放行待维护者确认**。009/017 开放问题、设计文档逐节人审、远端推送/CI 首跑仍待维护者。
2026-11-22：维护者"M1 放行"后，[design 021](design/021-mcp-full-client.md) **M1 五阶段（A–E）全部落地并全门禁验收，默认后端已翻转 cxxmcp**（提交链 `9f344c3` vendoring → `aa3838e` 接缝+适配层 → `32bb3a6` 开关勘误 → `90e3233` 双后端回放 → `cdd4b8a` 门禁修复 → `90a30b7` 默认翻转，另加本批文档）。A：vendored client-only 56 文件（固定 SHA `3c25a8af`，逐字节上游一致，LICENSE/裁剪清单归档 `third_party/cxxmcp/README.md`，[ADR 010](decisions/010-mcp-full-client-cxxmcp.md) 前提 3 关闭）；B：`McpSession` 接缝 + `mcp_cxxmcp.cpp`（唯一 include SDK 的 TU）+ env 开关，测试注入非空 factory 仍强制 legacy；C：`tools/mcp_parity_check.py` 10 场景双后端逐字回放——7 项一致 + 3 项已接受偏差（404 中途恢复更快、非标准错误码丢前缀、无 result 回包 fail-closed，021 §9）；D：四 preset × debug/release × -Werror × 双 env CTest、pty TUI/CLI 冒烟（cxxmcp 默认路径）、冷启动 cxxmcp 3.9ms/legacy 3.8ms（预算 100ms）、clang-tidy（vendored 经 SYSTEM include 豁免，自写 0 告警）、ASan/UBSan 干净、deepwiki 双后端一致、`--strict-project` 通过；E：021 §8 证据表 + §9 as-built 偏差、architecture/handbook/索引/ADR 010 状态同步。**回滚开关 `MINI_AGENT_MCP_BACKEND=legacy` 保留至 M3**；M2（resources/prompts/sampling/elicitation/tasks；引入 handler 前须确认上游析构缺陷修复）与 M3（删 legacy）未启动。021/010 整体人工评审、009/017 开放问题、远端推送/CI 首跑仍待维护者。
