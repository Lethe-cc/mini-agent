# 004：Markdown 解析层改用 vendored md4c

- 状态：已接受（2026-11-01，见决策人栏）。
- 日期与关联任务：2026-11-01；[007 第 5.6 节 Markdown 渲染行](../design/007-agent-v1.md)。
- 决策人或审批依据：维护者 2026-11-01 明确询问"能否基于这些开源库实现 markdown 渲染"，随后指示推进（"那能否…？"→ 本轮实施）；未逐条批复，按 ADR 002 既有 vendoring 模式执行。

## 背景

自研 `md::render` 子集缺表格、嵌套列表、任务清单等 CommonMark/GFM 语法，透传影响可读性。[references/md-renderers.md](../references/md-renderers.md) 已核对候选库许可证。约束：Linux/CMake、依赖须 vendored + 许可记录、终端几何（ANSI 零宽/CJK/emoji 双宽、硬换行）问题库不负责、`md::render` 是唯一替换点（TUI 不动）。

## 备选方案

- 方案 A：继续自研子集，需要时逐个补表格等（工作量随语法增长，合规性永远不完整）。
- 方案 B：md4c（MIT，v0.6.0，单 .h/.c 零依赖，CommonMark 严格合规）做解析 + 自写 MD_RENDERER 回调输出 SGR（本 ADR 选定）。
- 方案 C：cmark（许可需读原文）AST 遍历。AST 需求（/compact 结构分析）未出现，且体积更大，暂不选。
- 方案 D：glamour 思路（Go，不可链接）。样式架构借鉴，实现不可用。

## 决策

选方案 B：`third_party/md4c/`（md4c.h/md4c.c/LICENSE.md + 记录，模式同 ADR 002 的 nlohmann）。`src/md.cpp` 的 `render()` 重写为 md4c `MD_RENDERER` 回调式 SGR writer：语义 token → ANSI；`display_width/wrap/clip` 保持自研。md4c-html/entity 不引入。

## 后果

- 收益：CommonMark 全语法（含表格、嵌套列表、任务清单需 MD_TABLES flag）解析正确性外包；自研面收缩为"SGR writer"。
- 代价：agent_core 多编一个 ~260KB C 文件；third_party 审查面+1。
- 风险：回调 token 组合复杂（已用单测 + pty E2E 覆盖主要构造）；MD_FLAG_ 选错子句会退化透传——测试兜底。
- 验证：ctest md 用例升级（表格/嵌套出现即渲染）、pty 虚拟屏 0 超宽/0 重复行不变量保持、双 preset + tidy。

## 关联
[007](../design/007-agent-v1.md) §5.6/§8/§9；[md-renderers.md](../references/md-renderers.md)。
