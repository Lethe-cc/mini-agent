# tools/ — 项目开发小工具

约定：项目过程中产生的**开发辅助小工具**（手动调试、探测、一次性验证脚本等）统一放在本目录；
CI/质量门禁调用的脚本仍在 `scripts/`，被自动化测试引用的 mock 服务与测试脚本仍在 `tests/`。

本目录下的工具**不是业务质量门禁**，不写入 `project.json` 的 `quality_commands`，
仅在开发者手动运行或按需引用时生效。新增工具请在本 README 登记一行说明。

## 工具清单

| 工具 | 用途 |
| --- | --- |
| [mcp_probe.py](mcp_probe.py) | Streamable HTTP MCP 客户端探针：对任意 MCP server 手动做握手、列工具、调工具、发原始 JSON-RPC。与 `src/mcp_http.cpp` 使用同样的头规则（`Mcp-Session-Id`、`MCP-Protocol-Version`、`Accept`）和 JSON/SSE 双响应解析，可用来排查 mini-agent 连不上某个 HTTP MCP server 的问题。 |

## mcp_probe.py 快速上手

```bash
# 起本仓库的 mock HTTP server（测试资产，仍属 tests/）
python3 tests/mock_mcp_http_server.py 8933 &

# 完整探测：initialize -> tools/list -> 调一个工具 -> 可选 DELETE 关闭会话
python3 tools/mcp_probe.py probe http://127.0.0.1:8933/ --call echo --args '{"text":"hi"}' --close

# 只列工具 / 只调工具
python3 tools/mcp_probe.py tools http://127.0.0.1:8933/
python3 tools/mcp_probe.py call  http://127.0.0.1:8933/ fail

# 任意 JSON-RPC 方法（握手后）或不握手的原始单条消息（协议兼容性试验）
python3 tools/mcp_probe.py request http://127.0.0.1:8933/ ping
python3 tools/mcp_probe.py raw     http://127.0.0.1:8933/ bogus/method

# 需要鉴权的真实 server：token 走环境变量，不要写进命令行历史
MCP_PROBE_BEARER=xxx python3 tools/mcp_probe.py tools https://example.com/mcp
```

全局参数：`--timeout`（默认 15s）、`--protocol`（默认 `2025-06-18`）、`--quiet`、`--close`、
`--raw`（把每条收发的原始 JSON-RPC 消息打到 stderr：`>>` 发送、`<<` 接收；美化摘要仍在
stdout，便于 `2>&1 | jq` 或直接重定向查看线上数据）。
放在子命令前或后均可。退出码：0 成功、1 协议/传输失败、2 用法错误。

注意：探针刻意**不支持 stdio 传输**——stdio 服务器可以直接手工管道测试（如
`python3 tests/mock_mcp_server.py <<< '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'`），
HTTP 才是需要会话/头部细节才能调试的一侧。
