# mini-agent v6 技术方案（多 provider 登录与统一模型切换）

## 文档状态

- 状态：草案（未实现；待维护者确认开放问题后开工）。
- 关联任务：维护者 2026-11-12 提问"只考虑 OpenAI 兼容 provider，能否同时登录多个 provider，并通过 model 选项切换它们提供的所有模型"。
- 核对日期：2026-11-12，基于 main `70344aa`。
- 人工评审记录：无。

## 1. 背景与目标

现状（核对自代码，非推测）：

- `AppConfig`（[config.hpp](../../include/mini_agent/config.hpp)）只有一组 `base_url / api_key / model`，单活配置；
  `load_config_merged()` env 覆盖文件，`config_from_json` 拒绝 `schema_version > 1`，未知字段忽略（forward-read）。
- TUI `/provider <url>` 覆盖 `base_url`、`/login [key]` 覆盖 `api_key`（均重置 `AuthProbe` 并写盘），
  `/model <name>` 只改模型名，**不换 endpoint**；`/models` 单 worker 拉取当前 endpoint 的 `GET /models`
  （15 s 上限，兼作认证探针），结果以编号菜单呈现，输序号选定（[tui.cpp](../../src/tui.cpp) `start_models_fetch` / `model_choice`）。
- 每回合从当次配置快照新建 `OpenAiProvider`（design 007 §5.5 "one provider == one agent endpoint"，无全局单例）；
  session `meta.model` 记录模型名；context window 解析链 config → /models caps → models.dev 目录（design 010）。
- 密钥持久化在 config.json（0600，目录 0700，原子写），是 design 007 §6 记录的自觉取舍。

目标：同一份配置中**并存多个已登录的 OpenAI 兼容 provider**（各自 URL + key），
`/models` 汇总所有 provider 的模型为一个菜单，选中即同时切换 endpoint、key 与 model；
现有单 provider 用法（含 env/CI 脚本流）行为不变。

## 2. 范围与非目标

范围：

- config schema：新增可选 `providers` 命名档案表（URL + key + 每档案模型元数据），沿用可选字段惯例。
- TUI：`/provider add|rm|list` 档案管理；`/login`、`/models`、`/model` 的多档案语义；聚合模型菜单。
- CLI：`--model [<provider>/]<name>` 限定名解析（沿用 env > file 优先级）。
- 文档：parameters.md、architecture.md、.env.example 说明同步。

非目标（本期明确不做）：

- 非 OpenAI 兼容协议（Anthropic/Gemini 原生）；多 provider 并行推理或按请求路由/负载均衡（架构扩展点保留，见 007 §5.5）。
- 每 provider 独立超时/代理等连接参数（沿用全局 `MINI_AGENT_TIMEOUT_SECONDS`）。
- 密钥库/加密存储（沿用 0600 文件取舍，见 §6）；跨进程配置热同步（多开进程各持启动时快照，last-writer-wins，与现状一致）。
- 模型列表跨启动缓存（避免陈旧列表与额外落盘面，`/models` 显式重拉）。

## 3. 约束与参考资料

- 项目约束：AGENTS.md（密钥不入仓库/日志；小步修改；不新增依赖）；[ADR 002](../decisions/002-http-json-dependency.md)
  （OpenAI 兼容 Chat Completions + 0600 配置持久化）；[ADR 001](../decisions/001-session-model.md)（一进程一会话）。
- 既有设计：[007 §5.5/§6](007-agent-v1.md)（provider 注入、密钥取舍、TUI 配置流规则 8–10）、
  [008 §5.3](008-mcp-tools.md)（可选字段 + forward-read + `upsert_mcp_server` 先例）、
  [010](010-model-metadata.md)（配置权威、window 解析链、AuthProbe 兼探针）。
- 外部 API 事实：OpenAI 兼容 `GET {base_url}/models` 返回 `data[].id`（部分网关附带能力字段，v4.2 已解析）；
  各家网关对 `/models` 的开放程度不同（有的 404/405），必须容忍单档案失败。
- 本设计不引入新第三方库；全部复用现有 nlohmann/json、OpenAiProvider、worker join-then-read 纪律。

### 3.1 参考实现调研（2026-11，本地只读快照）

对四个主流开源 agent 的多 provider 实现做了源码级调研（只读参考，未复制代码；许可证均允许借鉴思路：
codex Apache-2.0，其余 MIT）。快照：`codex-main`（源码目录，无 commit 号）、`opencode@69269e4ebe`、
`Codewhale@4df5d2914`、`pi@2c8d1feb1`（均位于本机 `/home/marvin/code/repos/AI/`）。

| 维度 | codex (Rust) | opencode (TS) | Codewhale (Rust) | pi (TS) |
| --- | --- | --- | --- | --- |
| provider 配置 | `model_providers` 命名 map + `model_provider` 活动指针（`model-provider-info/src/lib.rs:149`、`core/src/config/mod.rs:978`） | provider 定义 map，无活动指针；模型身份即 `(providerID, modelID)` 对 | `[providers.<name>]` 命名表 + 顶层 `provider` 指针（config.example.toml） | provider 定义与凭据分离；models.json 不含密钥 |
| 凭据存储 | 单个 OpenAI 登录（auth.json+keyring）；第三方 key 走各 provider 的 `env_key` 环境变量，不做多 key 存储 | SQLite 凭据库，每 integration 多凭据、可切换激活（`credential.ts`） | keyring 优先，config 里只留 `__KEYRING__` 哨兵；事务式写入+回滚（`credentials.rs`） | auth.json 按 providerId 存 key，0600 文件/0700 目录 + lockfile（`auth-storage.ts`） |
| key 优先级 | CLI > env（`env_key` 缺失即报错） | 显式配置 > 凭据库 | CLI > 配置文件 > keyring > env（`config_resolve.rs:823`） | 显式 > 已存凭据 > env；刷新失败不静默回落 env（`resolve.ts:28`） |
| 模型目录 | 内置 preset + 启动时 JSON 目录；ChatGPT OAuth 仅限原生 openai provider | models.dev 集中目录快照+HTTP 刷新；本地端点（ollama/vllm/lmstudio）实时 discover | 逐 provider 实时 `list_models()`，30s 预算+防密钥泄漏进目录（`client.rs:4060`） | 静态 models.json + 动态 provider `refreshModels()`，按 provider 加锁缓存（`models-store.ts`） |
| 切换 UX | profile 预设（model+provider+审批打包）+ preset 弹窗 | 弹窗按 provider 分组、fuzzy 搜索、最近使用置顶（`dialog-model.tsx`） | provider 选择器发出原子 `SwitchProvider{provider, model}`（`apply.rs:2582`） | `/model` 菜单只列已登录 provider；文本 `provider/id` 可省前缀，歧义显式报候选（`model-resolver.ts:70`） |
| 逐模型 window | preset 内置 | 目录/用户配置每模型 limit | `model_context_windows` 逐模型表（`provider_toml.rs:118`） | models.json 每模型 contextWindow |

对本设计的印证与差异（采纳思路，不采纳实现）：

- **印证**：命名档案 + 活动指针 ≈ Codewhale（含旧顶层字段迁移进档案表的做法）；`provider/model` 限定名
  ≈ opencode（schema 强制限定）与 pi；裸名歧义 fail-closed ≈ pi 的"先收集全部候选再选 provider，歧义报错"
  （Q2 取本稿策略有先例）；key 0600 + 掩码 ≈ pi；逐模型 window 覆盖 ≈ Codewhale/010；单档案失败不拖垮
  聚合 ≈ Codewhale 30s 预算、pi 按 provider 独立刷新。
- **差异（有意不采纳）**：Codewhale keyring / opencode SQLite 凭据库引入新依赖，与"零新增依赖"冲突，
  维持 0600 config.json；pi 的 stored>env 与本稿 env-wins 相反——env-wins 保住既有 CI 语义，代价记入 Q3；
  opencode 无活动指针（provider 由所选模型推导）不适配我们"顶层三元组向后兼容"的前提，保留指针；
  models.dev 式集中目录依赖外部服务，维持逐档案 `GET /models`。

## 4. 方案与关键取舍

**选定：命名档案 + 聚合菜单 + 活动指针。**

- `providers` 为名字 → `{base_url, api_key, context_window?, reasoning_effort?}` 的表；
  顶层 `base_url/api_key/model` 仍是"当前生效值"（活动档案的展开），`active_provider` 记录活动档案名。
- `/models` 对每个档案并行拉取，聚合成一个带 provider 标签的编号菜单；选中即把该档案设为活动并写盘。
- `/model <provider>/<name>` 限定名直达；裸名先精确匹配活动档案，再全局唯一匹配，歧义报错并提示限定。

备选与否决理由：

| 备选 | 否决理由 |
| --- | --- |
| 仅靠 `MINI_AGENT_CONFIG` 多文件多开（现状变通） | 不满足"一个会话内切换所有模型"；每个实例要重复登录 |
| 无名字、按 URL 去重的隐式列表 | `/model` 限定名和 `/provider rm` 需要稳定短标识；URL 太长且可能带敏感 userinfo |
| 顶层字段废弃、只留 `active` 指针 | 破坏 env 覆盖与旧文件/旧版本兼容；展开式让旧二进制仍能读活动档案（§7） |
| 菜单只显示活动档案，加 `/provider <name>` 手动切 | 满足登录并存但不满足"通过 model 选项切换它们提供的所有模型"的核心诉求 |

取舍记录：本设计不改变任何已接受 ADR 的红线（密钥仍 0600 文件、无 push 面、审批策略不变）；
schema 采用可选字段而非 bump（沿用 009 D6 / 008 §5.3 先例），不另立 ADR。

## 5. 详细设计

以下全部为规划内容（未实现）。

### 5.1 配置 schema 与合并

```jsonc
{
  "schema_version": 1,
  "base_url": "https://api.a.com/v1",   // 活动档案展开（保持现状字段）
  "api_key": "...",
  "model": "model-x",
  "active_provider": "a",               // 新增，可选；空 = 未命名（legacy/env 态）
  "providers": {                        // 新增，可选；名字 [A-Za-z0-9_-]+（同 MCP 名规则）
    "a": { "base_url": "https://api.a.com/v1", "api_key": "...",
            "context_window": 200000, "reasoning_effort": "high" },
    "b": { "base_url": "https://api.b.com/v1", "api_key": "..." }
  }
}
```

- `struct ProviderProfile { name, base_url, api_key, context_window, reasoning_effort; valid(); }`；
  `AppConfig::providers`（有序 vector，落盘为 object）+ `active_provider`。
- 读：`providers` 缺席 = 现状单档案行为；档案名/base_url 非法 fail-closed（同 `McpServerConfig::valid` 风格）；
  `active_provider` 指向不存在的档案 → 读档报错（防静默换端点）。
- **活动档案展开不变量**：`base_url/api_key` 恒等于活动档案值（`active_provider` 空时为未命名值）。
  切换 = `activate_provider(name)`：展开三元组 + 覆写 `context_window/reasoning_effort` 为档案值（未设则清零，
  防止上一档案的 window 泄漏到新模型——配置权威原则，010）。
- 写：`providers` 仅在非空时落盘；密钥红线不变（`/config` 掩码 `***set***`，日志零 key）。
- env：`MINI_AGENT_BASE_URL/_API_KEY/_MODEL` 仍最高优先。任一生效时进入**会话级 override 态**：
  活动档案挂起（菜单仍可列出档案模型，但选中档案会提示 env 覆盖中，需 unset 后重启才真正生效——沿用"env 优先"语义，
  不做运行时改 env）；`_MODEL` 支持 `provider/name` 限定名，命中档案则整组取档案值再套 env 覆盖。
- CLI：`--model` 同限定名语法；`--provider <name>` 新增可选旗标（等价于选中该档案的当前 model）。

### 5.2 TUI 命令语义

| 命令 | 新语义 |
| --- | --- |
| `/provider` | 无参：列出全部档案（名字 + URL + key 掩码 + 活动标记）与当前生效值 |
| `/provider <url>` | 兼容保留：改当前（未命名或活动档案的）URL；活动档案存在时同步回写档案 |
| `/provider add <name> [url]` | 新建/覆盖同名档案（upsert，同 MCP 先例）；缺 url 时提示下一步 `/login`；不自动激活 |
| `/provider rm <name>` | 删除档案；删活动档案 → 回到未命名态（顶层值保留），提示 `/provider add` 或 `/models` |
| `/login [key]` | 写入活动档案（无档案时写未命名顶层，现状不变）；`/login <name> [key]` 定向写某档案（首 token 命中档案名时） |
| `/models` | 聚合拉取全部档案（含未命名顶层值，若与档案不重复）；单档案失败不阻塞其他，菜单尾部汇总失败原因 |
| `/model <name>` | 活动档案内精确匹配 → 全局唯一 `provider/name` → 歧义列候选报错；纯自由文本仍允许（现状：网关未列出的模型名也可直填） |
| `/model <provider>/<name>` | 激活该档案 + 设模型，一步完成 |

- `/help`、`config_next_hint()`、Tab 补全（`kCommands` 及 `/model` 参数补全）同步；`/config` 增加 `providers=N active=<name>` 行。
- 编号菜单沿用 `model_choice` 数字选择 + Esc dismiss + 自由输入穿透三条既有交互契约（pty smoke 已断言，不得回归）。
  行格式 `N. <provider>/<model>`（单档案时省略前缀，保持现有 `models (2)` 断言兼容）；`<- current` 标记活动组合。

### 5.3 聚合拉取 worker

- 现 `models_worker` 单线程改为**一次一个聚合 job**：job 内为每个档案顺序或并行（`std::thread` 数组，
  上限档案数，无新依赖）发 `GET /models`；每档案沿用 5 s connect / 15 s total 上限。
- join-then-read 纪律不变：worker 只填 `ModelsResult`（扩为 per-profile 子结果 + 失败表），
  主线程 join 后统一渲染菜单与日志；`models_fingerprint` 扩为全部档案的 `name+url+key` 串接。
- `AuthProbe` 扩为 per-profile map；提示符与 `/config` 的 login 状态取**活动档案**的 verdict；
  聊天回合结束仍把结果折叠进活动档案（source=chat 语义不变）。
- 日志：`models_list` 增加 `provider=<name>` 字段（URL 经 `Logger::redact_url`）；`model_select` 增加 `provider=`；
  新增 `provider_add/provider_rm/provider_activate` 元数据事件。零 key、零模型正文，红线不变。

### 5.4 模型元数据随档案

- `context_window/reasoning_effort` 移入档案粒度（顶层字段保留为活动展开，兼容旧文件与 env）。
- window 解析链不变：档案 config → 该档案 /models caps → `lookup_models_dev(档案 url, model)`；
  `/context`、`/reasoning` 写活动档案；`reset_usage_readings()` 在跨档案切换时同样触发。
- session：`meta.model` 继续记模型名，新增可选 `meta.provider`（session schema 仍 v2 可选字段，旧会话缺席 = 未命名）；
  恢复会话不自动改当前配置（现状语义：会话记录历史，配置决定下一回合）。

## 6. 异常与安全设计

- 密钥面扩大：N 个 key 同在 0600 config.json（原子的 tmp+rename、目录 0700 不变）。`/config`、菜单、错误文本、
  日志一律掩码/零输出；`/provider` 列表只显 URL 与 `***set***`。风险与单 key 现状同性质，量变不变更取舍（007 §6）。
- 单档案 `/models` 失败（401/403/404/网络）只标注该档案，不使整次聚合失败；全部失败时保留各档案错误供排查。
- 档案名进入日志与菜单：限制 `[A-Za-z0-9_-]+`，杜绝 ANSI/换行注入（同 MCP 名规则，008 D1）。
- `/model` 歧义 fail-closed：绝不静默选一个 endpoint（把请求发给错误服务商 = 数据外流风险）。
- 删除活动档案不删顶层生效值，避免"删档案导致下一回合无配置"的意外中断；回合执行中改档案沿用 `/approve` 同款
  worker join 纪律，回合内快照不受影响。

## 7. 兼容性与发布

- 旧文件（无 `providers`）：行为逐字节不变（回归基线：现有 agent_test config 组 + pty smoke 全绿）。
- 新文件被旧二进制读：forward-read 忽略 `providers/active_provider`，顶层展开值即活动档案 → 旧版仍可正常聊天，
  只是看不到其他档案；旧版下一次保存会丢档案表（文档注明：混用旧版前备份 config.json）。schema 不 bump。
- env/CI 脚本流：三个既有环境变量语义不变；限定名为纯增量语法。
- 回滚：删 `providers/active_provider` 两字段即回到单档案态。
- 里程碑：M1 config 层（schema+activate+env/CLI 解析+单测）→ M2 聚合 worker+AuthProbe map →
  M3 TUI 命令/菜单/补全 → M4 文档+pty smoke 扩展+mock 双 provider E2E。

## 8. 验证情况

| 验收项 | 测试位置或证据 | 实际执行命令 | 结果 | 限制 |
| --- | --- | --- | --- | --- |
| 旧配置回归（无 providers 行为不变） | tests/cpp/agent_test.cpp config 组（计划扩展） | 未执行 | 未执行 | 设计草案 |
| schema 读写/校验/activate 不变量 | agent_test 新增用例（计划） | 未执行 | 未执行 | 同上 |
| 限定名解析（唯一/歧义/env 覆盖） | agent_test 新增用例（计划） | 未执行 | 未执行 | 同上 |
| 聚合菜单交互（数字选择/Esc/穿透不回归） | tests/pty_tui_smoke.py（计划扩展） | 未执行 | 未执行 | 同上 |
| 双 provider 登录→聚合→跨档案切换→聊天 | tests/mock_openai_server.py 双实例 E2E（计划） | 未执行 | 未执行 | 同上 |
| 密钥红线（菜单/日志/config 掩码） | pty smoke 既有断言 + 新档案面扩展（计划） | 未执行 | 未执行 | 同上 |
| GCC/Clang 双 preset + tidy + ASan | scripts/cpp_check.py | 未执行 | 未执行 | 同上 |

## 9. 实现偏差与已知限制

尚未实现，无偏差可记。已知限制（设计内接受）：

- 多开进程间配置不同步（last-writer-wins），与现状一致；档案并发编辑可能互相覆盖，文档注明。
- 档案数无硬上限，聚合拉取耗时随档案数线性增长（顺序模式）；并行模式线程数 = 档案数，建议文档劝 ≤ 5。
- env override 态下菜单选档案不生效（需 unset env），是"env 优先"约束的直接推论。

## 开放问题（待维护者批复）

- Q1 命令面：`/provider add|rm|list` + `/login <name> [key]` 的语法是否认可？（备选：`/providers` 独立命令族）
- Q2 `/model` 裸名歧义策略：报错列候选（本稿）还是"活动档案优先、其余列出提示"？本稿取 fail-closed 报错。
- Q3 env override 态的菜单行为：本稿取"提示不生效"；是否需要"会话内临时压过 env"的更强语义？
- Q4 是否要求本期同时交付 CLI `--provider` 旗标，还是 TUI 先行？

## 最终核对

- [x] 接口、配置、路径与当前代码一致（§1 现状核对自 `70344aa`）。
- [x] 已实现与规划内容清楚区分（§5 起全部标注规划）。
- [x] 外部来源、版本和实际采用内容可追溯（§3）。
- [x] 验证结果有证据，未执行事项如实标明（§8 全部未执行）。
- [x] 索引与架构总览按需更新（design/README.md 已登记；architecture.md 待实现时更新）。