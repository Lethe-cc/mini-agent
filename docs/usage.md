# mini-agent 日常使用指南

任务导向的使用手册：装好之后**怎么干活**。逐项参数语义见 [参数使用大纲](parameters.md)，工具/MCP 专题见 [MCP 与工具使用指南](mcp-usage.md)；本文不重复字段表，只讲流程与取舍。

## 1. 安装与启动

```sh
cmake --preset linux-gcc-debug && cmake --build --preset linux-gcc-debug
cmake --install build/linux-gcc-debug     # ~/.local/bin/mini-agent
```

- `mini-agent`（裸命令）或 `mini-agent run`：进 TUI；`run` 默认**复用最近会话**（`--new` 新建、`--temp` 临时不落盘、`--session <id|前缀>` 显式恢复）。
- `mini-agent ask "问题"`：一次性问答，适合脚本（stdout 只有回复；失败非零退出码：1 运行时 / 2 配置）。
- 非 TTY 的 `run` 自动退化为 line-mode（逐行对话，脚本可用）。

## 2. 首次配置（免环境变量）

直接 `mini-agent` 进 TUI，按状态行提示三步录入（每步独立保存，可分多次/多进程完成）：

```
/provider https://your-gateway/v1     ← 服务地址（自动拉模型列表兼鉴权探针）
/login <key>                          ← 密钥（不回显、不入日志，0600 落盘）
/models                               ← 编号菜单选模型（或 /model <name>）
```

无任何档案时，第一条持久化命令自动创建 `default` 档案；之后可随意增删改名（见下节）。也可以完全用环境变量：`MINI_AGENT_BASE_URL` / `MINI_AGENT_API_KEY` / `MINI_AGENT_MODEL`（**env 恒赢**：覆盖生效期间 TUI 里改档案只写盘、本进程不生效，`/config` 会标注）。

## 3. 多 provider 与运行时切换

```
/provider list                        ← 所有档案（密钥掩码，活动标记）
/provider add deepseek https://api.deepseek.com
/login deepseek <key>
/provider rm default                  ← 删档案；删活动的会切到剩余首个
```

**运行时切换 provider 的两条路**（都会持久化选择）：

- `/models`：聚合所有配置齐全档案的模型列表，多来源时条目带 `provider/` 前缀，输序号即切换；
- `/model <provider>/<model>`：限定名直达，如 `/model deepseek/deepseek-chat`。

裸名 `/model <name>` 在聚合结果中恰属一个档案时也会自动切换；多档案同名会 fail-closed 报错列候选。CLI 侧 `--provider NAME` / `--model provider/model` 是**会话级**覆盖（只改本次进程，不写盘）。

## 4. 审批与工具安全

默认 `ask`：模型每次请求工具都弹审批块（写/改类先给 diff 预览）。按键：`y` 放行本次、`n` 拒绝、`?` 展开参数、Esc/Ctrl-C 拒绝并中断。

**`a` 要慎用**：按工具名整会话免问、**不看参数**——对 `run_bash` 就是任意 shell 命令都不再问（按下时会回显警示行）。不可信任务请逐次 `y`。

档位：`/approve ask|allowlist|off`（持久化）。`allowlist` 只免问 config `tool_allowlist` 里的名字；`off` 全免问（状态行常显红色 `auto-tools`）。**无沙箱**：工具以你的用户权限运行，审批是唯一的门；git 工具永不 push（远端操作留人工）。

## 5. 工作区与编码

- 会话绑定 workspace：启动 `--cwd <dir>` 或 TUI `/cwd <dir>`；文件五工具（read/write/edit/list/search）限制在 workspace 内（软边界），`run_bash` 同步 chdir。
- 侧栏：`/sidebar on` 看目录树 + git 改动；Ctrl-F 聚焦、方向键选择、Enter 插入路径、`p` 预览。
- `/preview <path>`：给人看的文件预览（不进模型上下文）。
- 编辑审批看 diff：`+`绿/`-`红，批准即按预览落盘（执行前重读校验，不一致即拒绝）。

## 6. 会话管理

```
/sessions                             ← 列出（标当前）
/session <id|前缀>                    ← 切换（保存旧、加载新；需回合空闲）
/session new                          ← 新会话
mini-agent session list|rm <id|前缀>  ← CLI 侧管理
mini-agent session export <id> [--format md|json] [--out 文件]   ← 导出转录/存档
mini-agent session prune --older-than 30 [--dry-run]             ← 清理久未用的会话
```

导出：md 适合人读/分享（含完整正文，分享前自查隐私；`--out` 落盘 0600）；json 就是会话文件格式，放回 sessions/ 目录即可重新导入。清理：阈值必填、按最后使用时间（mtime）、正在使用的会话（活锁）自动跳过；先 `--dry-run` 看清单再真删。

**上下文满了怎么办**：长会话撞窗口时错误行会带 `[hint] context window exceeded`。两个选择：

- `/compact`：把历史浓缩成一条摘要（保留系统提示；有损——精确代码片段可能丢；失败/中断历史不动）。适合"继续当前任务但减负"。
- `/session new`：彻底重新开始。

状态行 `ctx` 段显示上下文占用（设过 `/context <tokens>` 或模型目录有窗口值时带百分比；≥70% 黄、≥90% 红）。

## 7. 韧性与排障

- **瞬时失败自动重试**：网络错误/429/5xx 在"尚未输出任何内容"时最多重试 2 次（1s/2s 退避，尊重 Retry-After）；流开始后绝不重试。`MINI_AGENT_RETRIES=0` 关闭，`1–5` 调整。
- 提示符即连接状态：`?` 未配置/未验证 · `~` 探测中 · `>` 已验证 · `!` 密钥被拒 · `x` 不可达（图例见 `/help`）。
- 日志：默认 `/tmp/mini-agent-<uid>.log`（仅元数据，零密钥零正文）；`/log <path>|off|default` 改路径，`/log debug|trace` 开抓包（正文进 `.debug` 文件，注意隐私）。
- 常见坑：
  - TUI 里改了配置"没生效"→ 先 `env | grep MINI_AGENT`，env 覆盖恒赢；
  - 401/403 → 密钥问题（`!`），重试无意义，`/login` 重录；
  - 工具列表缺 MCP 工具 → `/tools` 强制握手重列，`/mcp` 看 server 清单；`MINI_AGENT_NO_MCP=1` 是全禁开关；
  - 中断：Ctrl-C/Esc 断当前回合（历史保留），二次 Ctrl-C 或 Ctrl-D 退出。

## 8. 延伸阅读

- [参数使用大纲](parameters.md)：每个旗标/env/字段/命令的语义、默认值与持久化范围。
- [MCP 与工具使用指南](mcp-usage.md)：MCP server 配置、`--mcp` 注入、审批交互细节、安全边界。
- [架构总览](architecture.md)：模块边界、TUI 帧模型、验证入口。