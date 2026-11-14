# mini-agent 技术方案 016：会话导出与清理（session export / prune）

## 文档状态

- 状态：已实现（2026-11-14；验证见 §5）；待人工评审。
- 关联任务：2026-11-14 维护者批准的优化清单第 (l) 项——会话只增不减、无导出手段。
- 核对日期：2026-11-14，基于 main `232da19`。
- 人工评审记录：维护者 2026-11-14 指示"一个一个解决"优化清单（含本项）；非逐条书面评审。

## 1. 目标与范围

会话文件（`sessions/<id>.json`）目前只能 list/rm/恢复。补两个 CLI 子命令：

- `session export <id|前缀> [--format md|json] [--out PATH]`：把会话导出为 Markdown（人读/分享/评审）或 JSON（原始存档）。默认 stdout。
- `session prune --older-than DAYS [--dry-run]`：按文件 mtime 清理久未使用的会话；`--dry-run` 只列不删。

非目标：TUI 内命令（CLI 足够，避免 TUI 面膨胀）；自动清理策略/定时任务；导入（export 的 JSON 就是 session 文件本身，手工放回 sessions/ 即可恢复，文档注明）；schema 变化。

## 2. 方案

**export**：前缀解析复用 `match_session_ref`（与 rm 同规则：歧义列候选退出 2）。`load` 后：

- `json`：`session_to_json(meta, chat)` 原样输出（与盘上格式一致）。
- `md`：新增纯函数 `session_to_markdown(const Session&)`（session.hpp/cpp，可单测）：标题 + 元信息行（id/model/created/updated/workspace/provider）+ 逐消息 `## User / ## Assistant / ## Tool (<tool_call_id>) / ## System` 段落；assistant 的 tool_calls 以 fenced json 列出；正文原样 fenced/段落输出，不做内容改写。
- `--out`：写文件，**0600**（会话正文属隐私，与 session 文件同级）；缺省 stdout。

**prune**：`--older-than DAYS` 必填（正整数；无默认值 = 不会误删）。候选 = mtime 早于 `now - DAYS*24h` 的会话文件（mtime 语义与 `list_by_recency` 一致：save 刷新 mtime = 最后使用）。**锁保护**：对每个候选 `try_lock`——被活进程持有则跳过（输出 `skipped (in use)`）；拿到锁（含回收死锁）即 `remove` 并 `unlock`。`--dry-run` 只打印 `would prune: <id>`。退出码：0 成功（含 0 个候选）；2 用法错误；1 IO 错误。

取舍：

- 否决"按 created_at 清理"：meta 时间戳是文本且语义是创建而非使用；mtime 已是 recency 的权威（v4.15 同款）。
- 否决"prune 默认 30 天"：破坏性操作必须显式给阈值（fail-safe）。
- export 不做脱敏：导出是用户显式行为，正文就是目的；0600 + 文档提醒分享前自查。

## 3. 接口变化

```cpp
// session.hpp
std::string session_to_markdown(const Session& session); // 纯函数
```

CLI：`session export|prune` 两个分支 + usage 文案；无 config/env/schema 变化；无新依赖。

## 4. 安全与兼容

- prune 只删 sessions/ 内通过 id 安全校验的文件（`remove` 既有守卫）；锁保护防删正在用的会话。
- export --out 拒绝覆盖？不拒绝（用户显式路径，常规 CLI 语义），但 0600 创建。
- 既有 list/rm 行为字节级不变。

## 5. 验证计划与结果

| 项 | 方法 | 结果 |
| --- | --- | --- |
| session_to_markdown | agent_test：元信息头、四种角色段落、tool_calls fenced、空会话 | 通过（2026-11-14） |
| export E2E | pty_cli_smoke：line-mode 造会话 → export md 含正文/元信息、json 可被 session_from_json 解析、--out 落盘 0600、未知 id 退出 1 | 通过（2026-11-14） |
| prune E2E | pty_cli_smoke：两会话一旧一新（utime 造旧）→ dry-run 只列旧、实删只删旧、活锁跳过、缺 --older-than 退出 2 | 通过（2026-11-14） |
| 既有契约 | agent_test/mcp_test、pty 两冒烟、GCC/Clang Debug | 通过（2026-11-14） |

验证补充（2026-11-14）：CLI smoke ALL PASS（lifecycle 段 9 断言）；TUI smoke 171 PASS / 0 FAIL；GCC/Clang Debug CTest 3/3。偏差 D1：未知 id 的导出错误文案沿用 load 的 "cannot open <path>"（"no such session" 是 remove 的文案），退出码 1 不变；前缀歧义路径与 rm 共用 match_session_ref（既有单测覆盖）。另新增 SessionStore::root() 只读访问器供 prune 取 mtime。

## 6. 限制

- Markdown 导出是线性转录，不渲染 diff/审批历史（那些本就不在 session 里）。
- prune 的 mtime 可被外部工具触碰（备份恢复等）——与 recency 复用同一语义，文档注明。
- 导入无专用命令：JSON 导出文件放回 sessions/ 即被 list/恢复识别（id 冲突时手工改名）。