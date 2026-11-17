# 01 · 单元/契约测试设计

载体：`tests/cpp/agent_test.cpp`（CTest `agent.core`）、`tests/cpp/project_test.cpp`
（CTest `starter.project_name`）。大纲见 [README](README.md)，用例索引见
[case-inventory](case-inventory.md)。

## 1. 目标与边界

- 验证 **agent_core 各模块的纯函数/契约**：模型序列化、session 存储语义、
  循环提交边界、配置合并优先级、Provider 解析、渲染几何、工具行为、上下文计量。
- 不依赖真实网络/终端：Provider 用解析纯函数 + 本地 fixture；TUI 只测
  `tui.hpp` 暴露的纯函数（帧/终端行为交给 03）。
- 是 `quality_commands` 的核心（经 `scripts/cpp_check.py` 的 ctest 段）。

## 2. 断言机制

- 无测试框架依赖：`void check(bool, const char* what)` 打印 `PASS/FAIL <what>`，
  失败累计计数，`main()` 末尾以非零退出码反映失败（CTest 据此判红）。
- `main()` 顺序调用全部 `test_*`（当前 55 个）；每个 `test_*` 自包含、可独立阅读。
- 约定：失败关闭（fail-closed）路径必须显式断言（解析坏输入、未知 role、
  schema 门禁、workspace 越界等），不只测成功路径。

## 3. 分组设计要点

| 分组 | 关键不变量 |
| --- | --- |
| 模型/会话 | JSON round-trip 字节稳定；`schema_version` 门禁（v1 拒 v2）；原子写 tmp+rename；`.lock` 防同 id 并发；markdown 导出纯函数 |
| 循环/工具 | 失败轮**不回滚历史**；tool_calls 多轮提交边界；`max_tool_rounds` 上限；write/edit 唯一匹配 fail-closed；diff 审批行；git 固定 argv 无 push |
| 配置/Provider | env > file 优先级；v2 命名档案迁移；`/models` 解析排序去重失败关闭；probe 三形态白名单 + 1k..200M 合理域；retry 分类（408/429/5xx 可重试，401/403 不） |
| 用量/状态 | usage 仅成功提交写入；状态段 ctx% 70/90 阈值着色；turn bridge 线程边界（worker 不碰主线程状态） |
| 渲染/TUI | 每行 `display_width ≤ cols` 硬换行；SGR 延续/重置；md4c GFM 子集；补全/历史/pane 纯函数 |
| 上下文 | 015 压缩构造；017 锚点+尾部单一读数、breakdown、压力阈值、保留尾部切点、机械剪枝、摘要模板校验、溢出排除集 |

## 4. 运行

```bash
cmake --build --preset linux-gcc-debug
ctest --test-dir build/linux-gcc-debug -R "agent.core|starter.project_name" --output-on-failure
# 直接看逐条 PASS/FAIL：
./build/linux-gcc-debug/agent_test
```

四 preset（gcc/clang × debug/release）均跑；`--sanitizers` 时同套用例在
ASan/UBSan 下复跑（CI `quality`）。`cpp_check.py` 默认 **models.dev refresh OFF**
（`--refresh-models` 开启）以保证门禁构建快速、确定；vendored 快照为权威。

## 5. 扩展约定

- 新增模块纯函数 → 在 `tui.hpp`/对应公共头暴露测试缝 → 新增 `test_*` 并在
  `main()` 注册 → 同步 [case-inventory](case-inventory.md)。
- 回归缺陷优先补断言（AGENTS.md）；字节级契约（如 v1 请求体）用
  `v1_request_shape_regresssion` 这类回归用例钉死。