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

注：`project.json` 的 `language` 已按迁移流程接入 cpp profile（2026-10-31，在临时生成后合并骨架，未重跑 `init_project.py`），`quality_commands` 为 CMake configure → build → CTest。

## 成功标准

可验证的验收标准（草案，数值待维护者确认后进入设计文档）：

1. 冷启动：构建产物在无交互模式下从进程启动到就绪（打印首个提示符或接受首个请求）不超过 100 ms（不含网络往返）；可用脚本化计时测量。
2. CLI：单次调用成功时输出模型响应、失败时以非零退出码报告错误，可被 shell 脚本组合使用；有对应自动化测试。
3. TUI：进入交互界面后至少完成一轮"输入 → 响应 → 再输入"的对话；异常退出后会话数据不损坏。
4. 多 session：同时运行 ≥ 2 个会话，各自对话历史互不可见；一个会话崩溃不影响其他会话。
5. 构建与质量：`python3 scripts/check.py --strict-project` 通过，覆盖 CMake configure → build → CTest；Linux GCC 与 Clang 至少各验证一种 preset。

第 1、5 条可在 CI 自动验证；第 3、4 条需要人工或脚本化终端测试并在交付说明中给出证据。

## 项目状态

2026-10-31：维护者补充用户与场景、约束和初步目标；"范围与非目标"的不做清单与"成功标准"数值为 AI 依据描述整理的假设，待维护者确认。同日已接入 cpp profile（`project.json` 的 `language` 改为 `cpp`，`quality_commands` 为 C++ configure/build/CTest；骨架为 starter 示例，非业务代码），GCC Debug 与 Clang Debug preset 均通过 `--strict-project`。第一版技术方案见 [design/007-agent-v1.md](design/007-agent-v1.md)（草案），会话形态、HTTP/JSON 依赖、TUI 选型三条 ADR 见 [decisions/](decisions/)（均提议）；2026-11-01 第一步开发完成：chat/session/provider/loop/CLI 实现并通过 GCC/Clang 双 preset、clang-tidy -Werror 与本地 mock SSE 端到端冒烟；自绘 TUI（ADR 003，5.6 帧模型与交互契约）已实现并经真实 pty 冒烟通过（run 在非 TTY 自动回退 line-mode）；Pi 式 streaming 排队 steering 已提前落地；免环境变量配置流（/login /model /provider）已实现并经 pty 全链路验证；独立日志文件（spdlog）默认开启写 /tmp/mini-agent-<uid>.log（0600，uid 隔离；MINI_AGENT_LOG 或 TUI /log 可改路径/off），日志保证不含密钥与正文；TUI 助手回复支持 Markdown 子集渲染（md4c 解析 + 自写 SGR writer，ADR 004，含 GFM 表格/任务清单）；TUI 与 line-mode 均支持 /exit 退出会话（保存+释放锁+恢复提示）。其余开放问题仍以人工评审结论为准。2026-11-05：v2 全部实现合入后，维护者提出下一阶段目标"能否进行代码修改、项目开发"，范围含改码改文档、git 能力、diff 显示修订；盘点结论为"能改但缺文件编辑工具/工作区/system prompt/git 工具面"。因维护者离场委托务实默认，AI 按 design-first 产出 [009 v3 设计草案](design/009-coding-v3.md)（未实现）与 [ADR 007](decisions/007-file-tools-workspace.md)、[ADR 008](decisions/008-git-tool-policy.md)（均提议），git 策略默认取"只读自由 + add/commit 恒逐次审批含 diff 预览 + 永不 push"；009 第 9 节开放问题中 Q2/Q3/Q4 已获同日批复（"git 提交可以自动，push 等人工操作"，ADR 008 升为已接受；AGENTS.md 红线条款同步改为"提交可自动、推送仍人工"），Q1/Q4b/Q5 仍开放；实现启动仍待维护者确认（未获"开始实现"指示）。
