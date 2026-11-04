# mini-agent v3 技术方案（编码能力：文件编辑工具 + 工作区边界 + diff 审批 + git 工具 + system prompt）

## 文档状态
- 状态：草案（2026-11-05 起草；同日维护者批复 git 策略"commit 可自动，push 等人工"——[ADR 008](../decisions/008-git-tool-policy.md) 升为已接受并反映于 §4/§5.2/§6/Q2/Q3/Q4；其余内容仍待评审）。
- 关联任务：维护者 2026-11-05 提出阶段目标"agent 能否进行代码修改、项目开发"，补充范围："能够修改代码和文档，git 能力，diff 显示修订，以及其他所有和项目开发相关的能力"。承接 [008](008-mcp-tools.md)（v2 工具循环，已实现）。
- 核对日期与适用版本：2026-11-05 实读本仓库 `src/tools.cpp`、`src/loop.cpp`、`include/mini_agent/tools.hpp`、`include/mini_agent/loop.hpp`、`src/tui.cpp`（审批区块与 `style_history_line`）、`src/config.cpp`（schema_version 1）、`src/cli_main.cpp`（`ask --tools off|ask|auto`）。代码基线：main@725b21d。
- 人工评审记录：维护者 2026-11-05 对话批复 git 策略："git 提交可以自动，push 等人工操作"（已落实至 [ADR 008](../decisions/008-git-tool-policy.md)，该 ADR 状态升为已接受；本文相应章节同日修订）。其余章节未获人工批复，推进方式仍为 AI 务实默认（design-first）。

## 1. 背景与目标

v2 之后，模型已经能执行命令（`run_bash`）、接入 MCP 工具、每一步人工审批。但用它"开发一个项目"仍有四个硬缺口（2026-11-05 盘点）：

1. **没有文件编辑一等工具**：改文件要靠 shell 引号转义与 heredoc，token 贵、易错、审批界面只能看到命令串而看不到"改了哪个文件哪几行"。
2. **没有工作区概念**：全仓库无 `cwd`/项目根配置，`run_bash` 裸继承主进程目录；没有"这个 session 在改哪个项目"的边界。
3. **没有默认 system prompt**：内核只透传 `Role::System`，工具用法、审批语义、输出约定全靠模型猜。
4. **非交互 CLI 断在审批上**：`ask` 子命令默认 `--tools off`，`off` 又无沙箱前提，批量场景没有安全档位。

成功判据（可验证）：

- 在 TUI 中对一个真实仓库说"把 X 函数的返回值从 int 改成 int64 并同步调用点"，模型经 `read_file`/`edit_file` 完成修改，审批界面展示 unified diff，y/n/a 三路径语义正确；构建测试可由模型经 `run_bash` 驱动。
- 文件工具拒绝工作区外路径；`git_commit` 随审批档位可自动（批复 2026-11-05），审批块与历史区均可见提交内容（文件清单 + diff + message）；任何路径下不存在 `push` 工具。
- 不新增第三方依赖；无 v3 配置时冷启动与 v2 行为一致（回归钩子同 008 惯例）。

## 2. 范围与非目标

本期（v3）交付：

- **内置文件工具集**（新 `FileRuntime`）：`read_file`（带行号、长度上限、可按行区间读）、`write_file`（整文件覆写/新建）、`edit_file`（精确 old→new 唯一匹配替换，fail-closed）、`list_dir`、`search_text`（固定语义的行级子串/正则搜索，限工作区内）。
- **工作区边界**：`AppConfig`/session 新增 `workspace` 根路径；CLI `--cwd PATH` 与 TUI `/cwd PATH` 设定；文件工具路径规范化后必须位于根内；`run_bash` 子进程 cwd 设为工作区根。
- **diff 审批显示**：`write_file`/`edit_file` 执行前对目标计算 unified diff（自写行级 LCS diff 工具，纯函数可测），TUI 审批块渲染 diff（新增行绿、删除行红、上下文 dim，复用 `style_history_line`/`tint` 着色体系），替换命令摘要成为默认视图。
- **git 工具集**（新 `GitRuntime`，调 `git` 可执行文件，工作区根内运行）：只读 `git_status`/`git_diff`/`git_log`；写侧 `git_add`/`git_commit` 与文件工具同档（`ask` 逐次确认 / `allowlist` 命中免问 / `off` 自动——维护者 2026-11-05 批复"commit 可自动"，ADR 008）。审批/回看视图含将提交文件清单 + staged diff 摘要 + message 全文。**不做 `git_push`** 及任何 remote 类工具（批复："push 等人工操作"）。
- **默认 system prompt**：新 session 自动注入一条 System 消息（身份 + 工作区路径 + 工具用法与审批语义 + 输出约定）；`/prompt` 查看、config `system_prompt` 字段可覆写或置空。
- **CLI 非交互安全档**：`ask --tools allowlist` 走 `tool_allowlist`（含 `git_status` 等只读工具默认建议集）；`auto` 保持存在但文档警示不变。

非目标（明确排除，后续另出设计）：

- 沙箱/容器/权限隔离（项目既有声明：无沙箱，审批即控制；A 级风险不靠 v3 解决）。
- `git push`、创建 PR、远端操作类工具。
- LSP/语义级代码工具（跳定义、重构）、AST 级编辑。
- apply_patch 式多文件补丁格式（选 `edit_file` 单点替换路线，理由见 [ADR 007](../decisions/007-file-tools-workspace.md)）。
- 子代理/多 agent 编排、任务规划器。
- MCP server 侧能力扩展（resources/prompts 等，沿用 008 裁剪）。

## 3. 约束与参考资料

项目约束（必须遵循）：

- 零新增依赖：diff、路径规范化、搜索全部自写（POSIX + 标准库 + 已 vendor 的 nlohmann/json）；git 通过 fork/exec `/usr/bin/git`（与 `run_bash` 同款进程管理，复用超时与进程组 kill 模式）。git 缺失时 GitRuntime 整体缺席（`alive()=false`），与 MCP server down 语义一致。
- 日志红线不变：工具事件只记元数据（工具名、路径相对根的位置、字节数、时长、状态），**绝不**记文件内容、diff 正文、命令内容（[007 第 6 节](007-agent-v1.md)、008 第 3 节）。
- ADR 001 进程模型不变：一进程一会话；工作区是 session 的属性而非全局配置。
- 审批策略沿用 ADR 006 框架：`ask` 默认、`AllowSession` 按工具名免问、`off` 全自动。写侧 git 工具（`git_add`/`git_commit`）**同档适用**（维护者 2026-11-05 批复"commit 可自动"，ADR 008 已接受）：`a` 键与 allowlist 命中可免问，`off` 档自动执行；补偿性可见性 = 免问模式下每次 commit 仍以 `[tool] git_commit` 行进历史区（含 message 摘要）。
- 用户可见文案英文；中文仅限设计文档。

外部资料：

- [references/cli-as-tools.md](../references/cli-as-tools.md)：文件工具与"全 shell"路线的成本对比，支撑 §5.1 的取舍；progressive disclosure（`search_tools` 元工具）不在本期——内置工具固定 5+5+1 个，全量注入成本可忽略。
- [references/agent-protocols.md](../references/agent-protocols.md)：MCP human-in-the-loop WARNING 与 annotations 不可信原则，转化为 v3 对 diff 内容同样 sanitize 的要求（工具输出是 UNTRUSTED 文本）。
- 设计模式参照（不复制代码，各仓库源码仍未精读）：主流编码 agent 的 `read_file` 行号前缀、`edit_file` 唯一匹配 fail-closed、审批含 diff 预览的三件套形态。

## 4. 方案与关键取舍

选定与备选详见两份 ADR，此处只记结论：

| 取舍 | 选定 | 备选 | 理由（详见 ADR） |
| --- | --- | --- | --- |
| 文件编辑形态 | `edit_file` 精确唯一替换 + `write_file` 整写 | apply_patch 多文件补丁；纯 shell | 唯一匹配天然 fail-closed、审批块最小、实现与测试面最小（[ADR 007](../decisions/007-file-tools-workspace.md)） |
| 工作区边界 | session 级 `workspace` 根 + 路径规范化校验（软性约束） | 强隔离（chroot/namespace/openat 逐级） | 与"无沙箱、审批为控制"的既有声明一致；防误操作不防恶意（同 ADR 007） |
| git 能力 | 只读自由 + `add/commit` 随审批档位（可自动）含 diff 预览 + **无 push/remote**（2026-11-05 批复落定） | 全只读；写侧恒逐次审批（原稿默认，被批复放宽）；commit+push 全审批 | 用户明确要求 git 能力；push 属人工操作（[ADR 008](../decisions/008-git-tool-policy.md) 已接受） |
| diff 实现 | 自写行级 LCS unified diff（纯函数） | vendored 库；`git diff --no-index` 借外部进程 | 零依赖约束；git 方案在未 init 目录/临时文件场景脆弱；百行级 LCS 足够（同 ADR 007） |
| system prompt 注入点 | 建 session 时作为首条 System 消息持久化 | 每请求动态拼接 | 与 session 重放语义一致、可在历史区可见；代价是 `/cwd` 变更需同步更新该消息（§5.5） |

## 5. 详细设计

### 5.1 FileRuntime（`src/tools.cpp` 新增段，声明入 `tools.hpp`）

```
class FileRuntime final : public ToolRuntime {
  public:
    FileRuntime(std::filesystem::path workspace_root, std::size_t read_limit_bytes);
    std::vector<ToolDefinition> list() override;   // read_file/write_file/edit_file/list_dir/search_text
    ToolResult call(const ToolCall&, const ShouldAbort&) override;
};
```

- **路径规则**：入参 `path` 为相对工作区根的路径或根内绝对路径；`std::filesystem::weakly_canonical` 后必须以根为前缀，否则返回 `is_error` Tool 消息（"path escapes workspace"），模型可自纠。符号链接解析后的目标同样必须在根内。不存在"读配置外的文件"路径——`read_file` 亦然（边界双向）。
- `read_file {path, from_line?, to_line?}`：输出 `行号↵内容`（cat -n 形态），默认上限 64 KiB（复用 `cap_tool_output` 头尾截断）。
- `write_file {path, content}`：覆写/新建，父目录必须已存在；执行前计算 diff（§5.3）。
- `edit_file {path, old_str, new_str, replace_all?}`：`old_str` 必须恰好匹配 1 处（`replace_all=true` 时 ≥1 处全部替换）；0 处或多处且未 `replace_all` → `is_error` 并回报匹配数（fail-closed，与 `parse_bash_arguments` 同款姿态）。执行前计算 diff。
- `list_dir {path?}`：目录名/文件名 + 类型标记，限 512 条目。
- `search_text {pattern, path_glob?, is_regex?}`：默认子串、行级、大小写敏感；跳过二进制（NUL 探测）；输出 `path:line:文本` 限 200 行 / 32 KiB。
- 所有文本读写按字节处理、UTF-8 透传（不转换、不保证 BOM 剥离）；工具结果文本进 TUI 前一律过 `sanitize_display`（008 5.4 惯例）。

### 5.2 GitRuntime

```
class GitRuntime final : public ToolRuntime {  // 构造时探测 git 可用性与根内是否 git 仓库
    // git_status{} / git_diff{staged?, path?} / git_log{limit?}
    // git_add{path} / git_commit{message, body?}
};
```

- 执行 = fork/exec `git -C <workspace> --no-pager <sub> [args]`，复用 `BuiltinRuntime` 的管道/超时/进程组 kill 读取循环（提取公共 `run_subprocess` 辅助，避免第三份拷贝）。输出 32 KiB 截断。
- 参数面 fail-closed：`git_log.limit` 数字校验、`git_add.path` 走 §5.1 同一边界校验、`git_commit` 拒绝空 message。**argv 固定数组，绝不把模型文本拼进 shell**（与 `run_bash` 的 `-lc` 不同层）。
- `git_status`/`git_diff`/`git_log` 无副作用：归入 CLI `--tools allowlist` 的默认建议集；TUI `ask` 下按 ADR 006 逐次确认（`a` 键免问即生效——只读工具无副作用，粒度放宽无风险）。
- `git_add`/`git_commit`：与文件工具同审批档（批复 2026-11-05）；`ask` 下审批块默认视图 = 受影响文件清单 + staged diff 前 N 行（非 argv 摘要），commit message 全文展示；免问/自动档下执行结果仍以 `[tool] git_commit` 行入历史区保证可见性。
- **不提供**：push、pull、fetch、checkout/switch、reset、rebase、clean、stash、任何 remote 操作。模型请求时如实以 unknown tool 报错回灌。
- 非 git 目录：GitRuntime `alive()=false`，工具整体不出现在注册表（不教模型撞墙）。

### 5.3 diff 计算与渲染（`src/diff.hpp/.cpp` 新增）

- 纯函数：`std::string unified_diff(std::string_view old_text, std::string_view new_text, std::string_view label)`——按行切分，LCS（O(n·m) DP，行数上限 20 000，超限降级为"文件过大，展示前后字节数"提示）；输出标准 `-/+` 前缀 + `@@ -a,b +c,d @@` 头，上下文 3 行，hunk 总行数上限 200（超出截断并标注）。
- 新建文件 = 与空文本 diff；删除路径不经工具（无 delete 工具，见 §6）。
- TUI 审批块（`tui.cpp approval_rows` 扩展）：对 `write_file`/`edit_file`/`git_commit` 传入预计算的 diff 文本，默认视图渲染 diff（`+` 行绿 `0;32`、`-` 行红 `0;31`、`@@` 头 magenta、上下文 plain；复用 `tint` + `colors_enabled()`，diff 行先过 `sanitize_display` 再着色）；`?` 仍展开完整参数 JSON。历史区 `[tool]` 行不变（仍摘要形态），审批时已看过 diff，不重复占屏。
- 审批与执行之间的竞态防护：执行前重读目标文件，若与生成 diff 时内容不一致（并发外部修改），放弃本次执行返回 `is_error`（"file changed since preview, retry"）——审批所见必须等于将被写入的内容。

### 5.4 工作区与配置

- `AppConfig` 增 `workspace`（字符串，可空 = 进程启动 cwd）、`system_prompt`（可空 = 内置默认；显式空串 = 禁用）。config.json `schema_version` 升 **2**（读取兼容 1，写出为 2，与 session schema 升版惯例一致）。
- CLI：`ask`/`run` 增 `--cwd PATH`（存在性与目录校验）；TUI 增 `/cwd [PATH]`（无参查询；设定后更新 banner、工作区根与 session 的 System 消息中的路径行）。session 文件持久化 `workspace`（恢复即恢复边界）。
- `run_bash` 子进程：fork 后子进程 `chdir(workspace)` 再 exec；`auto`/`off` 档位行为不变，`ask`（TTY）审批提示的 primary 摘要规则不变。
- 工具注册装配（`build_tool_registry`）：顺序 = BuiltinRuntime(run_bash) → **FileRuntime(workspace)** → **GitRuntime(workspace)** → MCP servers。重名拒绝机制已内建。

### 5.5 默认 system prompt

新 session（`--new` / 首次创建）注入首条 System 消息，内容（英文，静态模板 + 变量）：

1. 身份：mini-agent，terminal coding assistant；
2. 工作区：`Workspace: <root>`（绝对路径）+ "文件工具限工作区内；越界请求会被拒绝"；
3. 工具约定：优先 `read_file`/`edit_file` 而非 shell 编辑；`edit_file` 的 `old_str` 需含足够上下文以唯一匹配；改动后用 `run_bash` 跑构建/测试验证；提交需用户批准；
4. 审批语义："每条工具可能弹出人工确认，被拒绝时收到 error 结果，不要重试同一调用"；
5. 可被 `system_prompt` 配置覆写；恢复旧 session 不改写其历史 System 消息（仅 `/cwd` 变更时更新路径行——单点重写首条 System 中的 `Workspace:` 行，其余不动）。

`loop.cpp` 与 provider 层零改动（System 透传已存在）；改动集中在 session 创建路径（`src/session.cpp` / cli / tui 的 new-session 工厂）。

### 5.6 代码路径汇总

| 文件 | 变化 |
| --- | --- |
| `include/mini_agent/tools.hpp` / `src/tools.cpp` | FileRuntime、GitRuntime、`run_subprocess` 提取、`build_tool_registry` 装配 |
| `include/mini_agent/diff.hpp` / `src/diff.cpp` | 行级 LCS unified diff（纯函数） |
| `include/mini_agent/config.hpp` / `src/config.cpp` | `workspace`/`system_prompt` 字段、schema 2、`--cwd` 合并规则 |
| `src/session.cpp` | session 持久化 workspace、new-session 注入 System prompt |
| `src/tui.cpp` | `/cwd` 命令、审批块 diff 渲染、approval_rows 签名扩展 |
| `src/cli_main.cpp` | `--cwd` flag、`--tools allowlist` 语义接 allowlist（当前仅有 off/ask/auto）、usage 文本 |
| `src/loop.cpp` | 预检钩子：审批后执行前调用 runtime 的 `precheck`（§5.3 竞态防护）——`ToolRuntime` 增可选虚函数 `prepare(call)`（返回 diff 预览文本 / 校验），`approval_rows` 与 loop 共用 |

## 6. 异常与安全设计

- **错误折叠**：文件/git 工具的一切常规失败（不存在、越界、匹配数≠1、git 报错、非仓库）→ `is_error` Tool 消息回灌模型；仅 fork/exec 级故障抛 ProviderError（008 5.2 契约不变）。
- **写边界**：无删除类工具、无覆盖工作区外路径、`git_commit` 不碰 remote；`write_file` 覆写前 diff 即审计记录。工具副作用不因回合失败回滚的限制延续（008 第 6 节，UI 已明示）。
- **不可信内容**：文件内容、git 输出、diff 行均为 UNTRUSTED——入 TUI 前 `sanitize_display`；入日志仅元数据（相对路径 + 字节数 + 状态）。
- **审批完整性**：diff 预览-执行一致性校验（§5.3 竞态防护）；写侧 git 工具随统一审批档（`a`/allowlist/off 均生效，批复 2026-11-05）；残余风险：`off` 档下 `run_bash "git push"` 仍可达远端——工具层不拦截（与 006 对 off 的风险声明同级），命令黑名单未获批复暂不做（ADR 008 后果·风险②）。
- **超时/中断**：git/文件工具继承 `ShouldAbort`；单文件读上限 64 KiB、diff 行数上限防 DoS（模型读巨型文件时先 `list_dir`/`search_text` 引导，属 prompt 工程非硬限）。

## 7. 兼容性与发布

- 老 config（schema 1）可读：缺字段取默认（workspace 空 → 进程 cwd；system_prompt 缺 → 内置默认）。老 session 文件（schema 2 已是 v2 工具版）再 bump 至 3：新增可选 `workspace` 与首条 System 消息容忍——恢复时无 workspace 则用当前进程 cwd，不迁移历史。
- 无 MCP/无 v3 配置且未用新命令时，CLI `ask --tools off` 与 TUI 纯对话行为与 v2 字节一致（回归钩子）。
- 分里程碑：M1 文件工具 + 工作区 + diff 函数（纯后端 + 单测）→ M2 TUI 审批 diff 渲染 + `/cwd` + system prompt → M3 GitRuntime + CLI allowlist 档 → M4 文档（007/008 交叉引用、usage 指南、references 回写）。可分批合入，各批过 `--strict-project`。

## 8. 验证计划（设计阶段，未执行）

| 验收项 | 测试位置 | 命令 | 通过标准 |
| --- | --- | --- | --- |
| diff 正确性（LCS、hunk 头、截断、空文件新建） | `tests/cpp/agent_test.cpp` 新 `test_unified_diff` | `python3 scripts/cpp_check.py --preset linux-gcc-debug` | 断言全过 |
| 文件工具语义（越界拒绝、edit 匹配数、行号读、cap） | 同上 `test_file_tools` | 同上 | fail-closed 分支全覆盖 |
| git 工具（临时 init 仓库内 status/diff/log/add/commit 成功与失败路径；非仓库缺席；push 不存在） | 同上 `test_git_tools`（mkdtemp + `git init -q`，无网络） | 同上 | argv 固定、message 注入不逃逸 |
| 审批 diff 预览-执行竞态 | 同上（预检后改文件再执行） | 同上 | 返回 is_error，不落盘 |
| TUI 审批含 diff、y/n/a（commit 走统一档：`a` 免问生效）、`/cwd`、着色 | `tests/pty_tui_smoke.py` 扩展场景 | `python3 tests/pty_tui_smoke.py <binary>` | 手工自动化脚本通过 |
| 双编译器 + tidy | CI / 本地 | `scripts/cpp_check.py --preset linux-{gcc,clang}-debug --tidy --warnings-as-errors` | 通过 |
| 真机编码冒烟（人工） | 对一个 toy repo 完成"改码 → 跑测试 → commit"（ask 与 allowlist 两档各验一次） | 手动 | 交付说明记录证据 |

日志红线断言复用 pty smoke 既有"key never reaches the log"模式，扩展"diff 正文与文件内容不入日志"。

## 9. 待确认问题（评审前不实现）

- **Q1（范围）**：`search_text` 是否够用，还是本期就要文件级 glob 的 `list_dir` 递归形态？当前按"浅列表 + 行搜索"设计。
- ~~Q2（off 档写侧 git 豁免）~~ **已批复**（2026-11-05："commit 可自动"）：`git_add`/`git_commit` 随统一审批档，allowlist/off 均可免问（ADR 008）。
- ~~Q3（AllowSession 粒度）~~ **随批复落定**：`a` 键对只读三件套与写侧 git 工具均生效（与普通工具一致）。
- ~~Q4（AGENTS.md 红线范围）~~ **已批复**（2026-11-05）：红线放宽为"提交可自动（经批准或预授权），push 等人工"；AGENTS.md"不自动提交、推送"条款已同步改写。
- **Q4b（新增，残余风险）**：`off` 档下模型可经 `run_bash` 执行 `git push`，工具层不拦截。是否需要 `run_bash` 的 push 类命令黑名单（fail-closed 拒绝 + 提示人工）？AI 默认：本期不做（与 006 的 off 风险声明同级），如批复要求可低成本补入。
- **Q5（优先级）**：§2 清单之外，维护者所列"其他所有项目开发相关能力"是否有本期必须项（如测试运行器封装、语言特定 lint 集成）？AI 默认判定这些可由 `run_bash` 覆盖，不单独立工具。

## 最终核对
- [ ] 接口、配置、路径与当前代码一致。（实现后回写第 9/10 节偏差）
- [x] 已实现与规划内容清楚区分。（本文为纯设计草案，第 8 节全部"未执行"）
- [ ] 外部来源、版本和实际采用内容可追溯。
- [ ] 验证结果有证据，未执行事项如实标明。
- [ ] 索引与架构总览按需更新。（docs/design/README.md 已登记；architecture.md 待批准后更新）
