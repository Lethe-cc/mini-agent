# mini-agent v7 技术方案（provider 强制命名与配置结构统一）

## 文档状态

- 状态：已实现（2026-11-14 M1–M3 完成并通过验证门禁，见 §8；Q1–Q4 维护者 2026-11-14 批复推荐默认；待人工评审）。
- 关联任务：维护者 2026-11-14 提议"不允许不加 name 就添加 provider；把配置最外层的 base_url/api_key/model 用统一的 current 结构表示；不用考虑兼容旧配置"。
- 核对日期：2026-11-14，基于 main `0e4f2b1`。
- 人工评审记录：2026-11-14 维护者批复"全部默认，开始实现"（Q1–Q4 推荐默认值，未提出修改）；非逐条书面评审。

## 1. 背景与目标

[012](012-multi-provider.md)（v6，已实现）为兼容旧单档案配置保留了**未命名态**：顶层 `base_url/api_key/model/context_window/reasoning_effort` 与命名档案表并存，`active_provider` 空 = 未命名。核对自代码的双模式成本：

- **双向同步不变量**：`activate_provider()` 把档案展开到顶层，`sync_active_provider()` 把顶层编辑折回档案（[config.cpp:218-250](../../src/config.cpp#L218)）；TUI 每个编辑命令（/provider /login /model /context /reasoning）都必须记得调 sync（tui.cpp 6 处调用点），漏调即档案与生效值脱节。
- **未命名分支散布**：`activate_provider("")` 回退、save 时"无档案则两键不写盘"特判、`auth_probes` map 空名键、`/models` 聚合的未命名路由（[tui.cpp:2062-2073](../../src/tui.cpp#L2062)）、`"(unnamed)"` 显示 3 处、`/provider rm` 活动档案回退未命名（[tui.cpp:2501](../../src/tui.cpp#L2501)）、`/provider` 列表未命名额外行。
- **已发现的语义缺陷**：env 覆盖生效时切换档案，`activate_provider()` 直接覆写顶层三元组，load 时套用的 env 值被丢弃；与 `[warn]` 文案"this process keeps the env endpoint/model"（[tui.cpp:2264-2267](../../src/tui.cpp#L2264)）及 012 §5.1"覆盖生效期间档案切换只写盘不生效"的文档意图不符。

前提变化：维护者 2026-11-14 明确"不用考虑兼容旧配置"。012 §4 否决"顶层字段废弃、只留 active 指针"的理由（破坏 env 覆盖与旧文件兼容）中，兼容部分不再成立；env 覆盖可用运行时叠加保留。取舍推翻记录于 [ADR 009](../decisions/009-named-providers-schema-v2.md)。

目标：

1. provider 档案强制命名，磁盘上不再存在未命名持久态。
2. 单一事实来源：LLM 配置只存 `providers` + `active_provider`，顶层五字段废弃。
3. 运行时生效值统一为 `current`（单向派生：活动档案 + env 叠加），双向同步消失。
4. 首启零摩擦保留：无档案时持久化命令自动创建 `default` 档案。
5. 一次性加载迁移保住旧文件里已保存的密钥（迁移代码短命，但避免用户重新 /login）。

## 2. 范围与非目标

范围：

- config schema v2：废弃顶层 LLM 五字段；`providers`/`active_provider` 成为唯一 LLM 持久化面；加载时一次性迁移 v1 文件。
- `AppConfig` 接口重构：顶层五字段成员移除，新增运行时 `current`；`sync_active_provider()` 删除。
- TUI/CLI 消费端迁移（约 63 处直接引用，见 §5.7）；`/provider rm` 活动档案语义变更；CLI `--provider/--model` 明确为会话级叠加。
- 测试重写与扩展（agent_test config 组、pty smoke `multi:` 段与单档案 setup 流）、文档同步。

非目标（本期明确不做）：

- env 优先语义变更（三个 LLM env 仍最高优先；本设计反而把"env 恒赢"修严格，见 §5.3）。
- session schema 变更（`meta.provider` 可选字段现状保留，旧会话不迁移）。
- 密钥库/加密存储、跨进程配置热同步、模型列表缓存（沿用 012 非目标）。
- 新第三方依赖；ProviderConfig/OpenAiProvider 接口不动。

## 3. 约束与参考资料

- 项目约束：AGENTS.md（密钥不入仓库/日志；小步修改；破坏性变更先确认）；[ADR 002](../decisions/002-http-json-dependency.md)（0600 配置持久化取舍不变）。
- 既有设计：[012](012-multi-provider.md)（本设计推翻其 §4 兼容性取舍，命令面/聚合菜单/限定名语法全部继承）；[010](010-model-metadata.md)（配置权威、window 解析链、AuthProbe 兼探针）；[008 §5.3](008-mcp-tools.md)（fail-closed 校验与 forward-read 惯例）。
- 本设计为纯内部重构，无外部 API 事实变化，不引入新参考来源。

## 4. 方案与关键取舍

**选定：强制命名 + 磁盘单一事实来源 + 运行时 `current` 单向派生 + 自动 `default` 档案 + 一次性加载迁移。**

- 磁盘：`providers`（名字 → 五字段）+ `active_provider`，`schema_version=2`。
- 运行时：`AppConfig::current`（`ProviderProfile` 类型）= 活动档案值 + 非空 env 叠加；**只从档案派生，永不写回、永不落盘**。TUI 编辑命令改活动档案后重派生；CLI 旗标只改 `current`（会话级）。
- 无档案时任何 TUI 持久化命令自动创建并激活 `default` 档案，首启体验与现状一致。
- v1 文件加载时一次性迁移（§5.2），下次保存写 v2。

备选与否决理由：

| 备选 | 否决理由 |
| --- | --- |
| 保持顶层展开，仅禁止未命名态落盘 | 改动最小，但双表示与双向 sync 不变量原样保留，未达成"统一结构"目标 |
| 不设 `current` 成员，`effective()` 每次现算 | 63 处调用点产生临时对象，`complete()/missing()` 等接口别扭；派生结果需要稳定快照供回合使用 |
| 直接拒绝旧格式文件（维护者已允许） | 已保存密钥"丢失"（文件在但读不出），用户须重新 /login；迁移仅约 30 行，不值得省 |
| 首启强制先 `/provider add <name>` | 比现状多一步；自动 `default` 把未命名态"转正"，UX 不变而模型统一 |

重大取舍（破坏兼容、废弃顶层字段）记录于 [ADR 009](../decisions/009-named-providers-schema-v2.md)，不在此重复。

## 5. 详细设计

以下全部为**规划内容，未实现**。

### 5.1 磁盘 schema v2

```jsonc
{
  "schema_version": 2,
  "active_provider": "a",               // providers 非空时必填且必须命中
  "providers": {                        // 名字 [A-Za-z0-9_-]+，max 32（沿用 012）
    "a": { "base_url": "https://api.a.com/v1", "api_key": "...",
           "model": "model-x", "context_window": 200000, "reasoning_effort": "high" },
    "b": { "base_url": "https://api.b.com/v1", "api_key": "..." }
  },
  "log_file": "...", "workspace": "...", "tool_approval": "...", "mcp_servers": { }
  // 其余非 LLM 字段与现状完全一致
}
```

读写规则：

- 顶层 `base_url/api_key/model/context_window/reasoning_effort` 不再写入；v2 文件中若出现，按未知字段忽略（forward-read 惯例，Q4）。
- `providers` 非空 ⇒ `active_provider` 必填且命中，否则读档报错（fail-closed，防静默选端点，沿用 012）。
- `providers` 空（全新/env-only）⇒ `active_provider` 必须为空。
- 档案条目校验沿用 `ProviderProfile::valid()`（名字字符集、URL scheme、context_window 正整数）；部分档案（缺 URL/key）合法，到首次使用才要求齐备。
- `config_from_json` 的 schema 门禁改为 max 2；`save_config_file` 原子写/0600/0700 不变。

### 5.2 一次性加载迁移

`schema_version` 缺席或 =1 的文件在 `config_from_json` 内迁移为 v2 内存态：

| v1 文件形态 | 迁移动作 |
| --- | --- |
| `active_provider` 命中档案 | 顶层五字段即该档案展开，直接丢弃顶层，档案表原样保留 |
| 未命名（active 空）且顶层有值 | 顶层五字段折成 `default` 档案并激活；已有其他档案保留；`default` 名冲突时确定性后缀 `default-2`、`default-3`… |
| 未命名且顶层全空 | 无迁移内容，空配置 |

- 迁移只发生在加载；**加载不写盘**，下次保存才落 v2（在此之前旧二进制仍可读原文件，§7）。
- 日志记 `config_migrate` 元数据事件（`profiles=N` 等计数，零 key、零 URL 正文）。
- 迁移逻辑集中在 config.cpp 一处，标注短命代码；后续版本可整体移除。

### 5.3 AppConfig 接口与派生规则

```cpp
struct AppConfig {
    int schema_version = 2;
    std::vector<ProviderProfile> providers;   // 唯一 LLM 持久化面
    std::string active_provider;              // providers 非空时必命中
    ProviderProfile current;                  // 运行时生效值，不落盘；
                                              // name="" 仅存在于无档案/env-only 态
    bool env_override_active = false;         // 语义不变（UI 标注用）
    // base_url/api_key/model/context_window/reasoning_effort 顶层成员删除
    // log_file/workspace/system_prompt/tool_*/mcp_servers 不变

    bool activate_provider(std::string_view name); // 空名拒绝；切换后 refresh_current()
    void refresh_current();   // current = 活动档案(若有) + 非空 env 叠加
    // sync_active_provider() 删除
    bool complete() const;    // 读 current
    std::vector<std::string> missing() const; // 读 current
    ProviderConfig to_provider() const;       // 读 current
};
```

- `refresh_current()` 叠加 `MINI_AGENT_BASE_URL/_API_KEY/_MODEL/_CONTEXT_WINDOW/_REASONING_EFFORT`（非空才覆盖）；`_MODEL` 限定名 `provider/model` 先选档案再叠加（沿用 012 解析）。env 进程内不变，重读 `getenv` 无副作用。调用点：`load_config_merged` 末尾、`activate_provider`、TUI 每次档案编辑后。
- **编辑路径单向化**：TUI 持久化命令（/provider /login /model /context /reasoning）改活动档案 → `refresh_current()`；CLI `--provider/--model` 只改 `current`（会话级，从不触碰档案、从不写盘——现状 sync 也只改内存，净效果一致，语义就此明确）。
- **语义修正**：env 覆盖生效时切换档案，`current` 重新派生仍被 env 覆盖，生效端点/模型真正不变（修复 §1 所述缺陷，与 012 文档意图对齐）；`[warn]` 文案保留。
- 无档案态：`current` 仅由 env 填充（或全空 = setup 流）；`current.name` 为空。该态**不可持久化**——任何 TUI 持久化命令先自动建 `default` 档案（§5.4）。

### 5.4 TUI 命令语义

| 命令 | v7 语义（未列出的沿用 012） |
| --- | --- |
| `/provider` / `list` | 只列档案（名字 + URL + key 掩码 + 活动标记）；未命名额外行删除；无档案时提示 `/provider add` 或 `/login`（自动 default） |
| `/provider <url>` | 改活动档案 URL；无档案时自动创建并激活 `default` 再写入 |
| `/provider add <name> [url]` | name 必填（现状已如此）；**首个档案自动激活**（避免"有档案无 active"的非法态）；后续 add 不自动激活（沿用 012） |
| `/provider rm <name>` | 删档案；删活动档案 → **激活剩余首个档案**；无剩余 → 空 setup 态（`current` 清空，提示 `/provider add`）。被删档案的 URL/key 随档案删除，不再保留顶层值（行为变更，Q2） |
| `/login [key]` | 写活动档案；无档案时自动 `default`；`/login <name> [key]` 定向写不变 |
| `/models` | 只聚合档案；无档案（env-only）时以 `current` 值单路拉取，菜单无前缀（沿用现状交互契约） |
| `/model …` | 裸名/限定名/自由文本语义不变；写入活动档案后 refresh |
| `/config` | `active=` 行恒为档案名或 `(no provider)`；`"(unnamed)"` 措辞全部移除 |

- 自动 `default` 规则统一为一条：**任何需要持久化的 TUI 命令，在 `providers` 为空时先创建并激活 `default`**（Q1）。用户随后可 add 其他档案或 rm default。
- `config_next_hint()` 读 `current`，三步提示（config N/3 · next）不变；编号菜单数字选择/Esc/自由输入穿透三条交互契约不得回归。

### 5.5 AuthProbe 与日志

- `auth_probes` key = 档案名；env-only 态运行时用 `""` 键（仅内存，不落盘）。提示符与 `/config` 的 login 状态取 `current.name` 对应 verdict。
- 聊天回合结束折叠进 `current.name` 对应探针（source=chat 语义不变）。
- 日志 `provider=` 字段的 `"(unnamed)"` 改为 `"(none)"`；`provider_add/provider_rm/provider_activate/models_list/model_select/login` 事件沿用；新增 `config_migrate`。零 key、零正文红线不变。

### 5.6 session

`meta.provider` 可选字段不变：新会话在 `current.name` 非空时恒记录；旧会话缺席 = 历史事实，不迁移、不 reinterpret。恢复会话不自动改当前配置（现状语义）。

### 5.7 改动面清单（核对自 `0e4f2b1`）

| 文件 | 改动 |
| --- | --- |
| include/mini_agent/config.hpp | 顶层五字段成员删除、`current`/`refresh_current` 新增、sync 删除 |
| src/config.cpp | schema v2 读写 + 迁移 + 派生（约 11 处顶层引用重构） |
| src/tui.cpp | 约 42 处 `config.base_url/api_key/model` → `config.current.*`；未命名分支删除（聚合路由/rm 回退/显示措辞）；编辑命令改"档案 → refresh"路径 |
| src/cli_main.cpp | 约 5 处引用迁移；`--provider/--model` 改为 current 叠加（不再 activate+sync） |
| src/provider_openai.cpp | **不受影响**（消费 ProviderConfig，非 AppConfig） |
| tests/cpp/agent_test.cpp | `test_provider_profiles` 重写（legacy 字节不变断言、未命名 rm-active 断言作废）；`test_config_round_trip_and_precedence`/`test_config_batched_hint`/`test_connection_prompt` 随接口更新；新增迁移四分支/env 派生/auto default 用例 |
| tests/pty_tui_smoke.py | `multi:` 段与单档案 setup 流断言更新（`"(unnamed)"`、rm-active 行为）；新增迁移场景 |
| docs | parameters.md、architecture.md、.env.example、mcp-usage.md 涉及处同步 |

## 6. 异常与安全设计

- 迁移与派生路径零 key 日志；`config_migrate` 仅计数元数据。
- dangling `active_provider` fail-closed（沿用 012：防把已存 key 发给错误端点）；`/model` 裸名歧义 fail-closed 不变。
- `default` 名冲突后缀确定性生成，不依赖随机数；名字字符集校验不变（防 ANSI/换行注入）。
- **rm-active 行为变更的安全面**：被删档案凭据随档案立即删除（旧行为保留顶层值）；这是用户显式删除的预期结果，剩余档案不受影响；回合执行中改档案沿用 worker join 快照纪律，进行中回合不受影响。
- 密钥面不变：N 把 key 同在 0600 config.json，掩码/零输出红线全部继承。

## 7. 兼容性与发布

- **破坏性变更（有意）**：旧二进制读 v2 文件被 schema 门禁拒绝（报错提示升级或恢复备份）。维护者 2026-11-14 明确不需要旧配置兼容。
- 迁移只在加载时发生于内存；**首次保存前原文件不变**，旧二进制仍可读。升级后第一次 /login、/model 等任一写盘动作落 v2。
- 回滚：恢复升级前 config.json 备份（文档注明升级前备份建议）；代码回滚即 revert。
- env/CI 脚本流：三个 LLM env 语义不变且更严格（恒赢）；限定名语法不变。
- 里程碑：M1 config 层（schema v2 + 迁移 + 派生 + 单测）→ M2 TUI/CLI 消费端 → M3 pty smoke 重写 + 双 provider mock E2E + 文档同步。

## 8. 验证情况

2026-11-14 实际执行结果：

| 验收项 | 测试位置或证据 | 实际执行命令 | 结果 | 限制 |
| --- | --- | --- | --- | --- |
| schema v2 往返/校验/fail-closed | agent_test `test_provider_profiles`（重写：v2 无顶层键、dangling/空表选择拒绝、v2 顶层字段忽略） | `python3 scripts/cpp_check.py --preset linux-gcc-debug/linux-clang-debug/linux-gcc-release/linux-clang-release` | 通过（四 preset ctest 各 3/3） | — |
| 迁移四分支（未命名折 default/命中展开丢弃/名冲突 default-2/空文件） | agent_test `test_provider_profiles` 迁移用例 + `test_v4_config_fields`（v1 输入迁移） | 同上 | 通过 | — |
| env 派生恒赢（含 env 下切换档案、限定名钉档案、session_model 叠加） | agent_test 派生用例 + pty smoke 主场景（全程带三个 LLM env：菜单选择只写盘、`[warn]`、verdict 不被 profile-only 编辑改变） | 同上 + `python3 tests/pty_tui_smoke.py ./build/linux-gcc-debug/mini-agent` | 通过 | 012 遗留的 env pty 空缺本期补上 |
| auto default / rm-active 新语义 / CLI 会话级不写盘 | agent_test + pty smoke `batched:`（三进程分批录入落 default 档案）与 `multi:`（rm 活动档案切剩余首个） | 同上 | 通过 | — |
| 聚合菜单交互契约不回归 | tests/pty_tui_smoke.py `multi:` 段（重写）+ 既有单档案段 + 新增无 env 拒绝态会话（REJECTED/红 marker//config login=rejected/好 key 恢复） | `python3 tests/pty_tui_smoke.py ./build/linux-gcc-debug/mini-agent` | 通过（156 PASS） | 3 项既有失败与本期无关（状态行队列深度、80 列超限、auto-tools 常驻标记；012 §8 已记录改动前即失败，宽度检查窗口仅覆盖 long-reply 流式段） |
| 双 provider 登录→聚合→切换→rm→剩余档案聊天 E2E | pty smoke `multi:` 段：双 mock 端口（8939/8941）各自 key，rm beta 后 alpha 补模型并聊天、session meta 记 alpha | 同上 | 通过 | mock 为 loopback HTTP，未覆盖真实公网鉴权 |
| 密钥红线（迁移/菜单/日志/掩码） | pty smoke 既有断言 + 拒绝态会话（wrong-key 不入屏/不入日志）+ multi 段双 key 断言 | 同上 | 通过 | — |
| CLI 契约（ask/line-mode/中断） | tests/pty_cli_smoke.py | `python3 tests/pty_cli_smoke.py ./build/linux-gcc-debug/mini-agent` | 通过（ALL PASS） | — |
| GCC/Clang 四 preset + tidy + ASan | scripts/cpp_check.py | 四 preset + `--tidy` + `--sanitizers` | 通过（ctest 各 3/3；tidy exit 0；ASan/UBSan 3/3） | tidy 默认检查集存在全仓库既有风格类告警（swappable-parameters 等），非本次引入的新类别；mock server 在 Esc 中断场景有既有 BrokenPipe 噪音（不影响断言） |

## 9. 实现偏差与已知限制

实现偏差（相对 §5 规划，均为增量）：

- D1 CLI `--model` 落地为 `AppConfig::session_model` 会话叠加成员，在 `refresh_current()` 最后应用（env 之后）：与"只改 current"语义等价，且保证后续 TUI 编辑触发重派生时 CLI 覆盖不丢失。
- D2 §5.4 原写"只聚合档案"；实现保留 **env 覆盖态的生效路由**额外拉取（以 `current.name` 命名，鉴权结论落生效档案）。否则 env + 部分档案场景（pty 主场景）无任何可拉取目标，模型发现不可用。
- D3 `/provider <url>` 现在经 `ProviderProfile::valid()` 校验 URL（旧顶层写入不校验）：fail-closed 增强，防止非法 URL 落盘后下次加载整份拒绝。
- D4 `select_model` 的 env `[warn]` 从"仅跨档案切换"扩为"env_override_active 恒提示"：同档案内的模型编辑同样不生效，提示需一致。
- D5 `/context probe` 结果写入**生效档案**（`current.name`）；env-only 无档案时会话级保留、不落盘（不自动建档案）。
- D6 `config_migrate` 日志事件经 `AppConfig::migrated_from_v1` 运行时标志在 CLI/TUI 各自 logger 就绪后记录（config 层无 logger 依赖）。

已知限制：

- 继承 012：多开进程配置 last-writer-wins；聚合拉取耗时随档案数线性增长；env 覆盖需 unset + 重启才解除。
- 新增（有意）：旧二进制不可读 v2 文件；`default` 自动档案名不可配置（可 rm 后自建命名档案）。
- 迁移代码为短命兼容层，后续大版本可移除（届时 v1 文件直接拒绝）。
- pty smoke 3 项既有失败（012 §8 记录在案）未在本期修复，属 TUI 渲染/状态行既有问题。

## 开放问题（已批复，2026-11-14）

维护者通过批复表单接受全部推荐默认（"全部默认，开始实现"），未提出修改。逐项记录：

- Q1 无档案时持久化命令自动创建 `default` 档案：推荐**是**（首启零摩擦；备选：强制先 `/provider add`）。
- Q2 `/provider rm` 删活动档案后激活剩余首个档案：推荐**是**（备选：进入空 setup 态，更保守但多一步操作）。
- Q3 v1 文件一次性加载迁移：推荐**是**（约 30 行保住已存密钥；维护者已允许直接拒绝，若确定仅本人使用且愿重新 /login 可改为拒绝）。
- Q4 v2 文件中出现顶层 LLM 字段按未知字段忽略：推荐**忽略**（forward-read 惯例；备选：fail-closed 拒绝，更严格但无实际收益）。

## 最终核对

- [x] 接口、配置、路径与当前代码一致（§1/§5.7 核对自 `0e4f2b1`；实现后差异见 §9）。
- [x] 已实现与规划内容清楚区分（已实现，增量偏差见 §9）。
- [x] 外部来源、版本和实际采用内容可追溯（纯内部重构，无新外部来源）。
- [x] 验证结果有证据，未执行事项如实标明（§8 已全部执行，2026-11-14）。
- [x] 索引与架构总览按需更新（design/README.md、decisions/README.md 已登记；architecture.md 配置/模型发现/登录状态三段已更新；parameters.md §1.2/§2.2/§3.1/§4/§5.1 已更新）。