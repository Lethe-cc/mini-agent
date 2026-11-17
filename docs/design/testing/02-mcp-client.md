# 02 · MCP 客户端测试设计

载体：`tests/cpp/mcp_test.cpp`（CTest `mcp.client`）。大纲见 [README](README.md)。
被测三层：`McpTransport`（传输抽象）/ `McpRpcClient`（id 关联 JSON-RPC）/
`McpClient`（tools 对话），见 `include/mini_agent/mcp.hpp`、设计 008。

## 1. 双轨设计：fake transport + 真实子进程

- **FakeTransport**（进程内）：实现 `McpTransport`，记录 `sent_` 帧、注入响应，
  用于精确断言协议字节（initialize/initialized/tools/list/tools/call 顺序与字段），
  不 fork、无时序噪声。
- **真实传输 E2E**：stdio 用 `tests/mock_mcp_server.py`（真实子进程、换行分隔
  JSON-RPC）；HTTP 用 `tests/mock_mcp_http_server.py`（POST+SSE+会话头）。验证
  传输层在真实进程/网络边界下的行为与清理。

## 2. 用例与不变量（7）

| 用例 | 不变量 |
| --- | --- |
| `handshake_and_tool_round_trip` | 协商版本记录；initialize→initialized→tools/list 三帧顺序；tools/call 往返；`server__tool` 命名空间；tools/list 每进程缓存 |
| `foreign_and_error_messages` | 外来/无 id 消息跳过；握手失败→无工具且 `alive()=false`；工具级 `isError` 保留但**不**标记 server down |
| `registry_wires_mcp_tool` | `ToolRegistry` 正确接线 MCP 工具（source/命名空间） |
| `stdio_transport_real_child` | 真实子进程 stdio 往返 |
| `stdio_transport_dies_clean` | 子进程死亡→干净报错、不僵尸 |
| `http_transport_round_trip` | Streamable HTTP 往返（会话头/Bearer） |
| `call_restart_after_child_death` | 子进程死亡后再次调用→重启/失败关闭语义 |

## 3. 断言机制

同 01：plain `check()`，无框架；`main()` 顺序调用；失败非零退出。协议断言直接
对 `sent_` 帧做子串匹配（字段名/值），保证 wire 形状不被静默改坏。

## 4. 运行

```bash
ctest --test-dir build/linux-gcc-debug -R mcp.client --output-on-failure
./build/linux-gcc-debug/mcp_test     # 逐条 PASS/FAIL；自动拉起 mock 子进程
```

mock 服务零凭据、loopback；stdio mock 由测试自身 fork，HTTP mock 监听本地端口。

## 5. 扩展约定

新增传输/网关形状 → 先补 FakeTransport 协议断言，再补真实 mock E2E；
lenient 解析的每种网关变体（008 §9 R2）都应有对应用例，未知形状断言"结构指纹
错误且不含值"（日志红线）。