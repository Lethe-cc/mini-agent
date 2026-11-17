# 04 · CLI pty 端到端冒烟设计

载体：`tests/pty_cli_smoke.py`（43 检查）+ `tests/mock_openai_server.py`。
大纲见 [README](README.md)。验证 CLI 子命令契约与退出码；CI `smoke` 门禁。

## 1. 进程模型（PtyProc）

- `pty.openpty()` + `Popen`：stdout/stderr 接 pty（满足 SIGINT 安装的
  "stdin **或** stdout 是 tty" 门）；`stdin_pipe=True` 时 stdin 保持管道
  （line-mode `tty()==false`），从而可直接 `send_signal(SIGINT)` 复现 tty Ctrl-C
  的 handler 路径。
- `pump(seconds, idle=0.3)`：**自适应**——首字节前最多等 `seconds`；一旦有数据，
  空闲 0.3s 即返回（mock chunk 间隔 ≤0.1s，不会截断流），只砍输出 settle 后的
  死尾。select 0.05s 切片抽干 master 累积 `buf`。实测整套 ~11s（旧固定窗口 ~53s）。
- `wait_exit()`：**进程死亡后再 pump 0.6s**——子进程最后写入（cancelled 错误行、
  session 保存横幅）留在 master 缓冲，不抽干会丢。
- mock 以**子进程**拉起 `mock_openai_server.py`（SSE；错 key 真 401），loopback、
  零凭据；session 目录隔离。

## 2. 断言分组（check）

| 分组 | 契约 |
| --- | --- |
| `ask` 流式/中断 | 中断前仍在流式；Ctrl-C 打印 `cancelled:` 且退出码 **1**；流提前停止（无末尾 chunk）；一次性 ask **不落 session 文件** |
| `run` line-mode | 管道 stdin 逐行回合；`/exit`/EOF 保存退出；空闲 Ctrl-C 中断不退出 |
| `session` 生命周期 | `list`/`rm`/`export`(md/json,--out 0600)/`prune --older-than [--dry-run]`（mtime 阈值必填、活锁跳过） |
| provider/model 覆盖 | `--provider/--model` 会话级覆盖；未知 provider 退出码 **2** 并列出可用档案 |
| 配置错误 | 缺配置/坏配置退出码 **2**；`--help/--version/version` 不触网不读密钥 |

退出码契约（007 §5）：0 成功 / 1 运行时错误 / 2 配置错误。

## 3. 断言机制

`check(cond,name)` 打印 `PASS/FAIL`、累计 failures，末尾非零退出。断言看
`text()`（原始 pty 输出）+ 退出码 + session 目录文件数三重证据。

## 4. 运行

```bash
python tests/pty_cli_smoke.py ./build/linux-gcc-debug/mini-agent
```

## 5. 扩展约定

新子命令/退出码 → 补 check（输出+退出码+盘上副作用三视角）；中断类用例必须
`wait_exit` 后抽干再断言；同步 [case-inventory](case-inventory.md)。