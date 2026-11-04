# cli-as-tools：CLI-as-tools / Code Mode（全量注入 schema 的替代路线）

## 来源与版本
- 类型：官方工程博客 + 第三方仓库 + npm 包（背景论证 + 实现样本混合登记）。
- 主题：把 MCP/外部工具以 **CLI 命令或可执行代码** 的形态暴露给模型，替代"所有工具 schema 一次性注入 context"的传统 MCP 消费方式。本项目 v3 候选项（tool discovery / progressive disclosure，见 [008 §2 非目标](../design/008-mcp-tools.md)）的对标来源。
- URL 或本地路径：
  - 背景论证（必读）：
    - Anthropic Engineering — Code execution with MCP：https://www.anthropic.com/engineering/code-execution-with-mcp
    - Cloudflare Blog — Code Mode：https://blog.cloudflare.com/code-mode/
  - 路线 A：MCP → CLI 转换器（schema 不进 context，discover-then-call 走 shell）：
    - https://github.com/knowsuchagency/mcp2cli （运行时把 MCP/OpenAPI/GraphQL server 变 CLI，零代码生成；该路线代表作）
    - https://github.com/weibaohui/mcp2cli （Go；按需加载 schema、发现+调用合并为单次调用——与本项目 v3 候选形态最接近）
    - https://github.com/makabakaxy/mcp2cli （Python；MCP → CLI + 同步生成 SKILL.md）
    - https://github.com/rodaddy/mcp2cli 、https://github.com/vanja-emichi/a0-mcp2cli 、https://github.com/samikh-git/skiff （Rust 移植；同类小样本）
  - 路线 B：Code Mode / 代码执行沙箱（模型写代码调工具 API）：
    - npm `@cloudflare/codemode` （Cloudflare 官方实现，Workers 容器沙箱；核对版本 0.5.6）
    - https://github.com/universal-tool-calling-protocol/code-mode （TS 即插即用库，MCP/UTCP 工具经代码执行调用）
    - https://github.com/arek-e/mcpx （自托管网关：多 server 聚合为 2 个元工具 search/execute，V8 isolate 执行）
    - https://github.com/voidmind-io/voidmcp （Go，独立 Code Mode server，WASM 沙箱 JS）
    - https://github.com/buildagenta/adk-code-mode （Google ADK 执行器）
    - https://github.com/Parthkomalwad/codemode-deno-executor 、https://github.com/edmaker89/code-mode-poc （脱离 Workers 的执行器/PoC）
    - https://github.com/tychenjiajun/pi-codemode-guard （Pi coding agent 的 Code Mode 扩展；Pi 已登记于 [coding-agents.md](coding-agents.md)，本条是该来源生态的延伸线索）
  - 路线 C：Agent Skills（SKILL.md 开放标准，按需读知识+脚本，替代常驻 schema）：
    - https://github.com/anthropics/skills （官方标准仓库）
    - https://github.com/initializ/forge （Go 运行时：SKILL.md 编译为可移植 agent）
  - 对照（纯 CLI 宿主 / 双前端融合形态）：
    - https://github.com/mark3labs/mcphost （Go CLI 宿主；**已归档**，仅作历史形态参考）
    - https://github.com/thedotmack/mcp-client-cli （任意 MCP server 的 CLI）
    - 融合形态样本（同一能力同时发布 CLI 与 MCP server）：https://github.com/taylorwilsdon/google_workspace_mcp 、https://github.com/getsentry/MobileBuildMCP 、https://github.com/CodeGraphContext/CodeGraphContext 、https://github.com/OpenOSINT/OpenOSINT
- 固定 commit SHA / tag / 产品版本：未固定（多为活跃仓库）。`@cloudflare/codemode` 按 npm latest 0.5.6 核对；MCP 规范侧仍按 008 基线（2025-06-18，回退 2025-03-26）；引用 star 数/活跃度以 2026-11-05 为准，重新引用需再核对。
- 最近核对日期与阅读状态：**2026-11-05 登记**。Anthropic 博客全文实读；全部仓库经 GitHub API 实读元数据+许可证（stars/archived/license）；`@cloudflare/codemode` 经 npm registry 实读；Cloudflare 博客与仓库 README/源码**未精读**。
- 许可证及核实依据（当日 API 实读 spdx）：knowsuchagency/mcp2cli=**MIT**、weibaohui/mcp2cli=**MIT**、makabakaxy/mcp2cli=**MIT**、universal-tool-calling-protocol/code-mode=**MPL-2.0**（文件级 copyleft，复制代码前须逐文件评估）、@cloudflare/codemode=**MIT**、voidmcp=**MIT**、adk-code-mode=**Apache-2.0**、pi-codemode-guard=**MIT**、codemode-deno-executor=**MIT**、mcp-client-cli=**MIT**、mcphost=**MIT**（已归档）、initializ/forge=**Apache-2.0**、workers-sdk=**Apache-2.0**、anthropics/skills=**未标注**（无 license 字段，内容型仓库，引用条款前先读仓库说明）。arek-e/mcpx=**NOASSERTION（未核实）**；rodaddy、vanja-emichi、samikh-git、edmaker89 等小样本=**未核实**。核实≠授权复制：任何复制/改写前仍按 AGENTS.md 逐仓库读 LICENSE 原文并记 ADR。

## 参考目的与范围
- 借鉴什么：
  - 问题定义与量化：工具定义全量注入与中间结果回灌的 token 成本（Anthropic 博客示例场景 150k→2k tokens，-98.7%；数字属该文假设场景，不是普适基准）。
  - 两种可落地的发现机制：**按需加载 schema**（weibaohui/mcp2cli：默认不注入工具定义，`search`/`describe` 后才进 context）与**聚合为 2 个元工具**（mcpx：`search_tools` + `execute`，注册表常驻只占两个槽位）——即 008 非目标排除的 progressive disclosure 的两种成熟形态，是本文件对 v3 最直接的价值。
  - 融合形态证据：CLI 与 MCP 不是替代关系；协议留在传输/发现层，消费方式（schema 注入 / shell / 代码执行）按 context 经济学选择。
- 重点文件或章节：待精读。候选起点（按名登记，未读）：weibaohui/mcp2cli 的 schema 缓存与单命令 discover-then-call 实现、arek-e/mcpx 的元工具聚合层与 isolate 生命周期、@cloudflare/codemode 的 API 生成器（`mcp-to-typescript`）。
- 不参考什么：各仓库的宿主框架胶水（ADK/Workers/Agent Zero 集成层）；沙箱基础设施（V8 isolate/WASM/容器——与本项目"不新增第三方依赖 + 无代码执行环境"约束冲突，Code Mode 整体不属 v3 射程）；Skills 的 npm 包分发生态（同 Pi 生态不参考的既有结论）。
- 何时阅读：设计 v3 tool discovery / `search_tools` 元工具 / 懒注册时；评审 008"非目标"裁剪是否需松动时；维护者问"CLI 是否取代 MCP"类问题时的事实来源。
- 在任务中的角色：设计参考。趋势表述以本文件登记来源为准，不凭模型记忆断言；"某项目支持 X"类结论引用当天重核。

## 访问方式
在线读取（GitHub API 元数据 + LICENSE、npm registry、博客页面）。无法访问时说明影响，不凭记忆补写。离线精读仓库时 clone 到 `.references/` 并记录 commit SHA。

## 本地差异
- mini-agent v2 现状：`tools/list` 全量进注册表并注入请求体——正是本路线批评的模式；但当前规模（单/双 server、`server__tool` 前缀净化、上限 8 轮）下 token 成本远低于实现复杂度，008 裁剪仍成立。**无任何 C++ 同类实现**：两条路线都要手写，只能取模式，符合"手写最小实现"取向。
- 本项目已具备路线 A 的一半：`run_bash` + 逐次审批即 CLI 消费形态；缺的只是 server 工具的按需发现。
- Code Mode（路线 B）需要安全执行环境（Anthropic 博客自己列出的沙箱/限额/监控前置），与项目约束冲突，明确不借鉴实现、只记概念。
- 无 MCP 配置时 v1 逐字节兼容承诺（008 §1 成功判据）与任何懒注册改动必须共存：默认路径行为不变。

## 实际采用情况
- 2026-11-05：趋势讨论的量化依据取 Anthropic《Code execution with MCP》（当日全文实读）；Cloudflare《Code Mode》（URL 已登记，正文未读，两文互证关系来自博客文中引用，标注为待核对）。
- 2026-11-05：GitHub 搜索 + API 完成仓库清单与许可证核对（见上）；结论"生态无 C++ 实现"基于当日检索覆盖，属快照判断。
- 设计文档采用：**尚无**。008 未引用本文件；若 v3 立项 tool discovery，需在本节回写采用的具体形态（按需加载 vs 元工具聚合）与偏差记录。
- 已知待核对项：mcpx 许可证与实现细节；各仓库 star 数会漂移；anthropics/skills 的使用条款。
