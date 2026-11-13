# 009：provider 档案强制命名与配置 schema v2（废弃顶层 LLM 字段）

- 状态：已接受（2026-11-14 维护者批复"全部默认，开始实现"，随 design 013 Q1–Q4 一并确认）。
- 日期与关联任务：2026-11-14 维护者提议"不允许不加 name 就添加 provider；把配置最外层的 base_url/api_key/model 用统一的 current 结构表示；不用考虑兼容旧配置"。
- 决策人或审批依据：维护者 2026-11-14 通过批复表单接受 design 013 Q1–Q4 推荐默认并指示开始实现；非逐条书面评审。

## 背景

[012](../design/012-multi-provider.md)（v6）为兼容旧单档案配置保留未命名态：顶层 LLM 五字段与命名档案表并存，`active_provider` 空 = 未命名。代价是双向同步不变量（activate 展开 / sync 折回）、散布 TUI/config 的未命名分支，以及一处 env 覆盖下切换档案丢失 env 值的语义缺陷（013 §1）。012 §4 曾否决"顶层字段废弃、只留 active 指针"，否决理由是破坏 env 覆盖与旧文件兼容。

维护者 2026-11-14 明确撤回兼容前提（"不用考虑兼容旧配置"）；env 覆盖经运行时叠加可完整保留。原否决理由不再成立。

## 备选方案

1. **保持现状**：双模式继续存在，每个新编辑命令都要维护 sync 不变量；否决理由不成立但复杂度持续计息。
2. **仅禁止未命名态落盘**（顶层展开保留）：改动最小，但双表示与双向 sync 原样保留，未达成统一结构目标。
3. **强制命名 + schema v2 + 运行时 current 单向派生 + 一次性加载迁移**（选定）：磁盘单一事实来源，sync 消失，env 恒赢修严格；代价是破坏性变更与约 63 处调用点迁移。
4. **同 3 但直接拒绝旧文件**：维护者已允许；省约 30 行迁移代码，但已保存密钥读不出、须重新 /login，不值得。

## 决策

采用方案 3：

- provider 档案强制命名；磁盘 LLM 配置只有 `providers` + `active_provider`，`schema_version` 升 2。
- 运行时生效值统一为 `AppConfig::current`（活动档案 + 非空 env 叠加，单向派生、不落盘）；`sync_active_provider()` 删除。
- 无档案时 TUI 持久化命令自动创建 `default` 档案，首启体验不变。
- v1 文件加载时一次性迁移（未命名顶层折成 default 档案），首次保存落 v2；迁移代码标注短命，后续版本可移除。

细节见 [design 013](../design/013-named-providers.md)。本 ADR 推翻 012 §4 对"顶层字段废弃"方案的否决结论（仅限兼容性理由部分；012 的命令面、聚合菜单、限定名语法全部继承）。

## 后果

- 收益：单一事实来源；双向同步不变量与未命名分支整体删除；env 覆盖语义修严格（切换档案不再丢 env 值）；后续 provider 功能不再背双模式包袱。
- 代价（破坏性，有意）：旧二进制读 v2 文件被 schema 门禁拒绝；升级后首次保存前原文件仍可读，回滚靠备份。
- 行为变更：`/provider rm` 删活动档案不再保留顶层值，改为激活剩余首个档案（无剩余则空 setup 态）；被删档案凭据随档案删除。
- 迁移成本：config 层重构 + 约 63 处消费端引用（tui.cpp 占 42）+ agent_test config 组与 pty smoke `multi:` 段重写。
- 红线不变：密钥 0600 文件、零日志/零回显、fail-closed 校验、无 push 面（ADR 002/008 不受影响）。
- 后续验证：013 §8 验证计划（四 preset + tidy + ASan + pty smoke + 双 provider mock E2E），实现后逐项记录。

## 关联

- [design 013：provider 强制命名与配置结构统一](../design/013-named-providers.md)（本决策的实现设计）。
- [design 012：多 provider 登录与统一模型切换](../design/012-multi-provider.md)（§4 兼容性取舍被本 ADR 推翻；其余继承）。
- [ADR 002：HTTP 与 JSON 依赖](002-http-json-dependency.md)（0600 配置持久化取舍不变）。