# 06 · 冷启动预算设计

载体：`tools/cold_start_check.py`。大纲见 [README](README.md)。
对应 project-context 成功标准 #1：**进程启动到就绪 ≤100ms**。属开发小工具
（tools/），不进 `quality_commands`，由 CI `smoke` job 门禁。

## 1. 测量模型（两种读数，各 N 次取中位数）

1. `--version`：纯启动代理——不触网、不读密钥、不读配置，度量二进制冷启动本身。
2. 管道 `ask` 对接**即时** loopback SSE mock（`InstantHandler:8943` 立即应答）：
   全就绪路径（config 合并 + session 建立 + 首请求被接受）。mock 即时 ⇒
   墙钟 ≈ 启动 + loopback RTT，**网络往返按构造排除**。

门禁取**中位数**（max 仅报告），避免单次调度抖动误判。

## 2. 退出码与参数

- 0=预算内；1=超预算；2=用法错误。
- `--runs N`（默认若干次）、`--budget-ms 100` 可调。

## 3. 运行

```bash
python tools/cold_start_check.py ./build/linux-gcc-debug/mini-agent
python tools/cold_start_check.py <binary> --runs 20 --budget-ms 100
```

## 4. 设计约束

- 启动路径不得引入同步网络/重 IO（否则破坏预算）；models.dev 目录为内嵌快照、
  构建期刷新，不在启动期 fetch。
- 与 03/04 相同：零凭据、loopback；对负载敏感，CI 给足超时。

## 5. 扩展约定

新增启动期工作必须复跑本门禁；若预算需调整，先改 project-context 成功标准
再改 `--budget-ms` 默认值，保持文档与门禁一致。