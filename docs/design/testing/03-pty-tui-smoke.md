# 03 · TUI pty 端到端冒烟设计

载体：`tests/pty_tui_smoke.py`（~195 检查）。大纲见 [README](README.md)。
驱动**真实二进制** over pty，对接进程内 mock SSE；零凭据；CI `smoke` job 门禁
（不进 `quality_commands`，时序敏感）。

## 1. 进程与终端模型

- `pty.fork()` 起子进程跑 `mini-agent run`，父进程持 master fd；
  `set_pty_size(fd,24,80)`（后段 50,80）**硬编码**尺寸——env `COLUMNS/LINES`
  不生效，宽环境仅靠长 `USER` 改变 sysinfo 宽度。
- 环境注入 loopback `base_url`/`api_key`/`model=smoke`，`TERM=xterm`；
  session 目录隔离到 `/tmp/mini_agent_pty_smoke`（每次清空）。
- 读侧 `read_all(fd,timeout,idle=0.5)`：**自适应**——首字节前最多等 `timeout`；
  一旦有数据，空闲 0.5s 即返回（mock chunk 间隔 ≤0.1s，不截断流），只砍 settle
  后的死尾；select 0.05s 切片抽干 master。断言既看**原始流**（ANSI/CSI 不变量）
  也看**虚拟屏幕**（可见文本）。实测整套 ~85s（旧固定窗口 ~114s+）。

## 2. 虚拟屏幕回放（核心）

`virtual_screen(raw,rows,cols)` 重放光标/擦除序列而非简单 strip：
- 支持 `\x1b[nA/B`（**省略或 0 距离=1 行**，ECMA-48）、`\x1b[K/2K`、SGR/模式忽略；
  遇到不支持的控制序列直接 `raise`（把"发了意外序列"变成硬失败）。
- 模拟 auto-wrap（`wrap_pending`）与底部滚动（linefeed 弹出顶行）。
- 由此断言"历史在 idle 重绘后仍可见""live 块不触发 auto-scroll"等行记账不变量。
- `backup_counts(raw)` 提取每帧 `\x1b[nA` 回退数，断言**永不出现 `\x1b[0A`**
  （零距离上移会错移一行）。

## 3. 进程内 mock SSE（Handler）

`ThreadingHTTPServer(127.0.0.1:8939)`；`BetaHandler:8941` 用**自己的 key**
（v6 多 provider per-profile 鉴权可证）。按最后一条 user 消息分流：
- `GET /models`：错 key 真 **401**（登录状态判定走真实拒绝），对 key 返回两模型。
- `OVERFLOW…`：非 SSE 的 **400 JSON error**（015 溢出提示+hint）。
- 压缩请求前缀：返回结构化 `## Goal/## Next Steps` 摘要；`NUDGEFLOW` 先回纯散文、
  客户端 NUDGE 重试后才回模板（017 迭代摘要）。
- `EDITFLOW`：首轮分片 `tool_calls`(write_file) 触发**审批 diff 块**等 y/n；
  次轮读 tool 结果回 `tool said: …`（多轮提交边界）。
- `long-reply`：60 行×0.04s（高度缩放流式窗口 + Esc 中断窗口）。
- 默认：6 个 `tokN` chunk×0.1s。
- `handle_one_request` 吞 BrokenPipe/ConnectionReset（中断场景客户端先断开，属预期）。

## 4. 断言分组（expect）

启动横幅/提示符连通标志（黄?未验证→绿>已验证）、流式回合历史定格、历史着色、
行宽/回退不变量、侧栏单级浏览器/`p` 预览/pane 滚轮（SGR 1000+1006）/Esc 关闭、
审批 diff 行（added 绿）、`/context`(含 K/M)/`/compact`/`/reasoning`、模型菜单与
多 provider 切换、**日志红线**（key/正文零泄露）。侧栏高亮检测用 `\x1b[27m`
（反显重置）而非 `\x1b[7m`（composer 光标也用反显）。

## 5. 时序与稳健性

- 大输入**分块写**并抽干，避免单次 `os.write` 撑满 pty 缓冲死锁。
- 对墙钟敏感：自适应等待后常规 ~85s、宽 USER 略增；**超时截断 ≠ 产品缺陷**
  （019 §5 复核结论）。CI 给足 job 超时。
- 导航/插入类断言放在主会话**末尾**，避免扰动历史计数/门控断言。

## 6. 运行

```bash
python tests/pty_tui_smoke.py ./build/linux-gcc-debug/mini-agent
# 宽环境（长 USER）：
env USER=abcdefghijklmnopqrstuvwxyz python tests/pty_tui_smoke.py ./build/…/mini-agent
```

退出码 0=全过；逐条 `PASS/FAIL <label>`，末尾汇总。

## 7. 扩展约定

新 TUI 行为 → 先扩 mock 分流（新 user 前缀）→ 加 expect（原始流+虚拟屏幕双视角）
→ 同步 [case-inventory](case-inventory.md)；涉及行记账的必须断言 `\x1b[nA` 不变量。