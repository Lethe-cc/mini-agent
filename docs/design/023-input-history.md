# 023：TUI 输入历史统一与斜杠命令持久化（P1+P2）

## 文档状态
- 状态：已实现（P1+P2，2026-11-23，dev 分支）；待人工评审；验证见 §8。
- 关联任务：维护者 2026-11-22 需求——↑/↓ 只能召回普通输入，无法召回 `/context`、`/sidebar` 等历史设置操作；2026-11-23 批准 P1+P2 在 dev 分支实现。
- 核对日期：2026-11-24；代码修订基线 `6c1b47b`（dev），首次实现提交 `c5a8b79`。
- 编号沿革：初稿/首次实现编号为 **022**（2026-11-23，提交 `c5a8b79` 及其提交信息、当日验证记录均沿用 022）；2026-11-24 发现 `feature/cxxmcp` 已先行占用 022（`022-mcp-capabilities-m2.md`，2026-11-22 起，且已被 ADR 011/021/mcp-usage 多处以"022 §x"引用），本设计遂改号 **023** 并同步全部代码注释、测试断言标签与文档引用；行为与实现无任何变化。
- 人工评审记录：无。

## 1. 背景与目标

现状（007 §5.6 规则 3）：↑/↓ 单行历史刻意排除 `/` 行——Enter 提交处（`tui_keys.hpp`）、启动播种处（`tui.cpp`）、`/session` 切换重播种处（`tui_slash.hpp`）三处均以 `content.front() != '/'` 过滤，斜杠命令由 Tab 菜单负责补全。用户需要重复执行设置类命令（`/context`、`/sidebar on` 等）时只能重打。

目标：
- **P1**：↑/↓ 历史统一收录斜杠行（脱敏后），会话切换不丢失斜杠历史。
- **P2**：脱敏后的斜杠历史跨进程重启持久化。

成功标准：↑ 能召回并回车重发 `/context` 等命令；`/login <key>` 的 key 永不出现在召回历史与持久化文件；重启后仍可召回上次进程的斜杠命令；既有普通输入召回、bash 草稿驻留、Tab 补全行为不变。

## 2. 范围与非目标

范围：TUI ↑/↓ 历史来源与持久化；`/login` 脱敏纯函数；新增数据文件 `input_history`。

非目标：
- **P3 前缀过滤召回**（输入 `/co` 后 ↑ 只走 `/context`…）：本次不做。
- 普通输入不新增持久化——它们仍由 session chat 的 user 消息播种（既有机制，避免双份存储）。
- 不改变斜杠命令不入 chat history / session 存储的既有边界（007 §5.6 规则 7）。
- 不引入新依赖；CLI 行模式无 ↑/↓ 编辑，不涉及。

## 3. 约束与参考资料

- 项目约束：007 §5.6 规则 3（键位契约、bash 草稿驻留）、规则 7（密钥边界：api_key 不入菜单/错误文本/日志）；012 §5.2 / 013（`/login <name> <key>` 首 token 为已配置 profile 名时指向该档案，否则整段 arg 是活动档案的 key）；ADR 003（自绘 TUI，不引入 readline）。
- 代码事实（实现时核对）：`default_session_root()`（`src/core/session.cpp`）按 `XDG_DATA_HOME` → `HOME/.local/share` 推导；`MINI_AGENT_SESSION_DIR` 覆盖 session 根（`src/cli/cli_common.cpp::make_store`）；config 原子写模式（tmp 0600 + fsync + rename，`src/core/config.cpp::save`）。
- 外部参考：无新增外部来源；bash/readline 的统一历史语义为常识性借鉴（斜杠行与普通行同列表、时间序），无代码复制。

## 4. 方案与关键取舍

选定：**单列表统一历史**（P1）+ **仅斜杠行落盘**（P2）。

- 单列表：Enter 处不再按 `/` 过滤，全部行经 `sanitize_history_line` 后进入同一个 `history` 向量；↑/↓ 导航代码（`tui_input.hpp`）内容无关，零改动。
- 仅斜杠行落盘：普通输入已由 session JSON 持久化并在启动/切换时播种；斜杠行不进 chat，故只需为它们补一个轻量文件。启动时把文件中的斜杠行接在 chat 播种的普通行之后（时间序近似：上次进程的斜杠操作早于本次恢复会话的新输入；同 session 内旧斜杠行与新 chat 行的精确交错不可恢复，属可接受近似）。
- 备选（已否决）：双列表（普通/斜杠各一，↑ 在某模式走对应列表）——需要模式判定与新键位，破坏 bash 肌肉记忆，实现面更大。
- `/exit` 不记录：召回一个立即退出进程的命令无意义且易误触。
- 写入时机：每次记录斜杠行时同步原子写整文件（≤200 行，微秒级；进程崩溃也不丢已执行命令），不做退出集中 flush（`goto done`/信号路径多，易漏）。

## 5. 详细设计

### 5.1 函数（`include/mini_agent/tui.hpp` 声明，新增 `src/tui/tui_history.cpp` 实现；`tui_render.cpp` 保持"纯函数、无 I/O"的既有约定，落盘逻辑独立成 TU）

```cpp
// 返回入历史的行；nullopt = 不记录（/exit、空行、masked key 不经过此路径）。
std::optional<std::string> sanitize_history_line(
    std::string_view line, const std::vector<std::string>& provider_names);
```

规则：
- 非 `/` 行原样返回（普通输入行为不变）。
- `/exit`（含带参前缀形式）→ `nullopt`。
- `/login …`：镜像 `handle_slash` 的 arg 解析——首 token 命中 `provider_names` 则保留 `"/login <name>"`（丢弃其后 key）；否则整段 arg 视为 key，收敛为 `"/login"`。裸 `/login`（masked 入口）原样保留。
- 其余 `/` 行原样返回。

```cpp
// 斜杠历史文件路径：session 根的父目录 / "input_history"。
// root 为空时返回空 path（禁用持久化）。
std::filesystem::path input_history_path(const std::filesystem::path& session_root);
// 容忍加载：文件缺失/行损坏静默跳过；只收 '/' 开头行；再次脱敏（防手改文件塞 key）；
// 截尾至 cap。
std::vector<std::string> load_slash_history(const std::filesystem::path& file,
                                            const std::vector<std::string>& provider_names,
                                            std::size_t cap = 200);
// 追加一行：相邻去重（与末行相同则不动）、超 cap 丢最旧、原子写（tmp 0600+rename）。
// 失败静默返回 false（历史持久化是增强，不是关键路径）。
bool append_slash_history(const std::filesystem::path& file, std::string_view line,
                          std::size_t cap = 200);
```

路径推导：调用方传 `store.root()`（已含 `MINI_AGENT_SESSION_DIR` 覆盖），文件落在其父目录——默认 `~/.local/share/mini-agent/input_history`。测试把注入的 session 根再嵌套一层（`MINI_AGENT_SESSION_DIR=<临时目录>/sess`），使父目录仍在临时目录内、`input_history` 自动隔离；无需新 env 旋钮（`pty_tui_smoke.py` 各流已按此嵌套）。

### 5.2 接线（三处过滤点的处置）

1. **Enter 提交**（`src/tui/tui_keys.hpp`）：`if (line.front() != '/') push` 替换为——`sanitize_history_line(line, S.provider_name_list())` 有值则 push；若原行以 `/` 开头且被记录，再 `append_slash_history(input_history_path(S.store.root()), *sanitized)`。位置保持在 `/exit` 判定与 pending 入队之前（streaming 中排队的行同样立即入历史，与现状普通行一致；队列 drain 不重复记录）。masked `/login` 的 key Enter 走 `secret_input` 分支，天然不经过此路径。
2. **启动播种**（`src/tui/tui.cpp`）：chat user 消息播种保持 `!= '/'` 过滤（chat 里本就不该有斜杠行，过滤保留是防御）；随后 `load_slash_history(...)` 的行 append 到 `history` 尾部，`hist_pos` 取最终 size。
3. **`/session` 切换**（`src/tui/tui_slash.hpp`）：clear 前收集旧 `history` 中 `/` 开头的行；重播种新 chat 后把这些斜杠行 append 回去（斜杠历史属 UI 级，跨会话保留；与 P2 文件内容一致）。不重写文件。

`hist_up_impl`/`hist_down_impl`/`mark_edited_impl`、Tab 补全、菜单数字选择（在入历史前 `line.clear()`）均零改动。run_loop 巨型 ctx 初始化列表不新增字段（持久化经自由函数 + 既有 `store` 引用完成）。

### 5.3 文件格式

纯文本，一行一条已脱敏斜杠命令，UTF-8，无表头；上限 200 行（超出丢最旧）；0600 权限、tmp+rename 原子替换。选择纯文本而非 JSON：内容是平坦字符串列表，逐行容错加载比整体 JSON 解析更抗损坏，且便于用户自查/清理。

## 6. 异常与安全设计

- **密钥**：`/login` 内联 key 在入内存历史前即被剥离；`load_slash_history` 对盘上内容再次跑同一脱敏（纵深防御：手工编辑或旧版本文件不会把 key 带进召回）。masked 输入路径本就绕过历史记录。持久化文件权限 0600，目录沿用 session 根父目录（`ensure_root` 已保证 0700 的 sessions 子目录；父目录由 `append_slash_history` 按需 `create_directories`）。
- **损坏/缺失文件**：加载逐行容错，非 `/` 行丢弃；写失败静默忽略（下一次 Enter 再试），不影响会话与配置保存。
- **并发**：多进程同时写同一文件为 last-writer-wins（原子 rename 保证不出现半文件）；与 config 保存的既有并发语义一致，不新增锁。
- **注入**：文件内容仅作为 composer 文本召回，仍需用户按 Enter 才执行，与手打等价；不自动执行。

## 7. 兼容性与发布

- 无 schema 变化：session JSON、config 均不动；新增独立文件，删除即回到"斜杠不持久化"状态（内存内 P1 行为仍在）。
- 旧版本升级：文件不存在 → 空加载，首次记录时创建。降级：旧版本忽略该文件，无副作用。
- 文档同步：007 §5.6 规则 3 中"斜杠行不入历史"表述就地修订并回链本设计；`/help` 键位图例无需变更（↑/↓ 语义未变，仅覆盖面扩大）。

## 8. 验证情况

执行环境：Linux（Debian，共享开发机），GCC 14.2.0 / Clang 19（clang-tidy 19.1.7），Python 3.13.5；下表 `python3` 即文档惯用 `python` 入口（本机无 `python` 别名，`check.py` 会把质量命令首参数 `python` 替换为当前解释器，语义一致）。

| 验收项 | 测试位置或证据 | 实际执行命令 | 结果 | 限制 |
| --- | --- | --- | --- | --- |
| 脱敏纯函数（/login 各形态、/exit、普通行、profile 名边界） | `tests/cpp/agent_test.cpp::test_input_history` | `python3 scripts/cpp_check.py --preset linux-gcc-debug`（ctest `agent.core`） | 通过（四 preset ctest 均 3/3，见下） | — |
| 加载/追加（cap、相邻去重、损坏行、盘上再脱敏、0600） | 同上 | 同上 | 通过 | — |
| ↑ 召回斜杠行并重发 | `tests/pty_tui_smoke.py` 主流程 `/context` 召回段（"023 Up recalls the settings command and resends it" 等） | `python3 tests/pty_tui_smoke.py ./build/linux-gcc-debug/mini-agent` | 通过（2026-11-24 改号后复跑 209 PASS / 0 FAIL，exit 0；断言逻辑与首验完全一致，仅标签改号） | 另在 gcc 平铺 `build/mini-agent` 上先行跑过一遍，同样全过 |
| /login key 不入召回历史 | 同上（"023 Up recalls /login without the key argument"、"023 recalled login never echoes the key"） | 同上 | 通过 | — |
| 重启后召回持久化斜杠行 | 同上（batch 流 "023 slash history persists across restarts (Up recalls /model)" + `input_history` 内容/0600 断言） | 同上 | 通过 | — |
| 会话切换保留斜杠召回 | 同上（"023 session switch keeps slash recall (Up lands on /context)"） | 同上 | 通过 | — |
| 既有行为保持（普通召回、草稿驻留、菜单、session 消息数） | 既有 pty 断言全量 + `python3 tests/pty_cli_smoke.py ./build/linux-gcc-debug/mini-agent` | 同命令列 | 通过 | — |
| 全量门禁（CI 同命令） | 四 preset build+ctest；clang-debug 加 tidy+ASan/UBSan；冷启动预算 | `python3 scripts/cpp_check.py --preset linux-gcc-debug` / `--preset linux-gcc-release` / `--preset linux-clang-debug --tidy --sanitizers` / `--preset linux-clang-release`；`python3 tools/cold_start_check.py ./build/linux-gcc-debug/mini-agent` | 全部通过（各 ctest 3/3；tidy 0 告警；ASan/UBSan 干净；冷启动 median 4.3/5.6ms < 100ms） | macOS 未验证（本机 Linux）〔0.5 起 macOS 已移出支持范围，此限制不再是缺口，见 [024](024-drop-macos-support.md)〕 |
| `check.py` 统一入口 | `python3 scripts/check.py --fast` / `--strict-project` | 同命令列 | **2026-11-24 起全部通过**（baseline + 52/52 模板 unittest + 默认 preset build/ctest 3/3）。此前入口失败系 dev 既有问题：`docs/design/README.md` 021 行引用的 `021-mcp-full-client.md`、`../decisions/010-mcp-full-client-cxxmcp.md` 只存在于 `feature/cxxmcp`（`0730e4d`），索引行却随 `6c1b47b` 进入 main/dev；已在 HEAD 干净导出复现，与本变更无关。2026-11-24 将 dev 侧该行去链接并注明"随 feature/cxxmcp 交付"后入口恢复；合并该分支时以其索引行为准 | 合并 `feature/cxxmcp` 后本行注记应随其索引行一并被覆盖 |

## 9. 实现偏差与已知限制

- 时间序近似：重启后斜杠行统一排在 chat 播种行之后，同 session 内的精确交错不恢复（§4 已述取舍）。
- 会话切换保留的是**内存中**本进程斜杠行（含启动加载的持久化行），不清理、不去重跨会话重复；cap 只约束盘上文件。
- 多进程 last-writer-wins 可能丢并发写入的少量行（§6）。
- P3 前缀过滤召回未实现，留待后续需求。

## 最终核对
- [x] 接口、配置、路径与当前代码一致。
- [x] 已实现与规划内容清楚区分（P3 明确标注未实现）。
- [x] 外部来源、版本和实际采用内容可追溯（无新增外部来源）。
- [x] 验证结果有证据，未执行事项如实标明（macOS 未验证〔0.5 起已移出支持范围，见 [024](024-drop-macos-support.md)〕；`check.py` 入口曾被 dev 既有缺失链接阻塞，2026-11-24 经索引行注记后恢复并全过，历史阻塞已如实留痕）。
- [x] 索引与架构总览按需更新（README 索引 + 007 规则 3 修订 + `docs/architecture.md` TUI 内部模块句补 `tui_history.cpp`）。