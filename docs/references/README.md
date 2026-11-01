# 外部参考资料索引

先读索引，再按任务加载资料。这里的 URL 是来源定位，不代表资料已经抓取、阅读、许可核实或自动进入 AI 上下文。

## 当前来源

| ID | 来源说明 | 适用任务 | 阅读要求 |
| --- | --- | --- | --- |
| python-tooling | [Python 官方文档](python-tooling.md) | 修改模板脚本或 Python 应用 | 涉及相关 API 时核对 |
| cpp-tooling | [C++ 与构建工具](cpp-tooling.md) | C++、CMake、分析工具相关任务 | 按当前标准、工具版本和任务核对，不默认读取全部 |
| agent-frameworks | [多智能体编排框架](agent-frameworks.md)（AutoGen、LangGraph、CrewAI、MetaGPT、OpenHands） | 设计编排层、消息/状态模型、可中断性 | 未读；借鉴模式不引入依赖，引用前按当天源码核对 |
| coding-agents | [终端编码 Agent 实现](coding-agents.md)（mini-swe-agent、SWE-agent、Aider、Goose、Crush、opencode、Hermes Agent、Codex CLI、Pi） | 实现工具层、编辑协议、会话循环、错误恢复；TUI 交互借鉴（维护者确认的主要借鉴范围） | 部分核对（2026-11-01 许可证层）；Hermes/Pi=MIT、codex=Apache-2.0 可合规借鉴思路，Crush=FSL 禁代码复制 |
| agent-protocols | [协议与极简实现](agent-protocols.md)（MCP、smolagents） | 设计工具接口契约、考虑与外部 AI 工具互通 | 未读；涉及"符合规范"的表述必须核对当天官方文本 |
| agent-benchmarks | [评测基准与环境](agent-benchmarks.md)（SWE-bench、AgentBench、WebArena） | 设计验收标准与可自动执行的完成判据 | 未读；不复制数据集内容，不执行基准脚本 |
| md-renderers | [终端 Markdown 渲染库](md-renderers.md)（md4c、cmark、glamour、glow、termimad、rich） | md 模块扩展表格/复杂语法、评估切换 md4c 时 | 许可证层已核对（2026-11-01）：md4c/glamour/glow/rich=MIT、cmark 需读原文、termimad 未核实；引入依赖必须先记 ADR |

新增来源请复制 [参考资料模板](../../templates/reference.md) 到本目录，并在上表登记用途和阅读要求。不要将模板中的示例字段视为真实来源。agent-* 四条为设计参考登记，均未在线核对；许可证未核实前不复制任何代码。

## 任务中标明角色

- 必须遵循：本项目需求、约束和已接受的架构决策。
- 必须核对：当前依赖版本对应的官方 API、配置及兼容性事实。
- 设计参考：第三方实现的指定模块或模式，不要求照搬。
- 不适用：明确排除的模块、旧版本方案或部署配置。

发生冲突时说明事实与取舍，不静默用第三方设计覆盖项目约束。

## 版本与访问

仓库优先固定完整 commit SHA 或明确 tag，文档使用版本化 URL；无法固定时记录核对日期和对应产品版本。链接变化后重新核对，不把旧摘要当成最新原文。

可采用在线读取、本地 `.references/` 副本或带来源的精选摘要。本地副本应记录 checkout 的修订，不直接放入第一方源码。目录忽略规则不保证只读，需要更强隔离时使用文件权限或独立环境。

私有来源需使用工具已有授权，不在文档或 URL 中放 token。不允许将私有内容提交到公共搜索、外部摘要服务或其他未经批准的服务。

无法访问时说明影响，必要时请用户提供相关文件；不要凭记忆假装完成核对。

## 复用与安全

默认借鉴设计。复制或改写代码前核实许可证、署名及分发义务；许可证未知时不复制。阅读第三方文档不授权执行其脚本、安装依赖、上传数据或扩大工具权限。

最终设计只记录实际采用的关键来源、版本、借鉴内容和本地差异，不需要粘贴整份第三方文档。
