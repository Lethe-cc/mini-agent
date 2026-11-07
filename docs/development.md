# 开发指南

第一次使用模板，请先阅读 [从模板到项目交付：完整使用手册](user-guide.md)。本页保留环境、命令和质量配置的技术速查，不重复完整操作流程。

## 环境

模板脚本需要 Python 3.11+，只使用标准库。在仓库根目录执行下列命令。业务技术栈的运行时和依赖另行配置。

## 命令

| 命令 | 用途 |
| --- | --- |
| `python scripts/doctor.py` | 检查 Python 版本、配置、入口文件，报告可选 Git 可用性 |
| `python scripts/init_project.py --name demo --description "项目目标" --stack "待选择" --dry-run` | 预览初始化，不写文件 |
| `python scripts/init_project.py --name demo --description "项目目标" --stack "待选择"` | generic 初始化；缺少名称/描述时交互输入 |
| `python scripts/init_project.py --name demo --description "项目目标" --language cpp` | 创建 C++ 骨架和项目 CI；也支持 python/typescript/go/rust；stack 可省略 |
| `python scripts/check.py` | 模板基础检查、工具测试和已配置质量命令 |
| `python scripts/check.py --require-initialized` | 额外要求完成初始化并移除有效项目文档中的占位符 |
| `python scripts/check.py --strict-project` | 额外要求已初始化且至少配置一项项目质量命令 |
| `python scripts/check.py --tooling-only` | 显式跳过项目命令，仅做工具和文档检查，不用于业务验收 |
| `python -m unittest discover -s tests -p "test_*.py" -v` | 单独运行模板工具测试 |

## 开发小工具（tools/）

项目过程中产生的小工具（手动调试、探测、一次性验证脚本）统一放 `tools/`，清单和用法见 [tools/README.md](../tools/README.md)。它们不是质量命令，不进 `project.json`；CI/门禁脚本仍放 `scripts/`，被自动化测试引用的 mock 服务和测试脚本仍放 `tests/`。

当前工具：`python3 tools/mcp_probe.py --help` — Streamable HTTP MCP 客户端探针，手动握手/列工具/调工具/发原始 JSON-RPC，用于调试 mini-agent 连接外部 HTTP MCP server 的问题。

脚本中的项目路径根据脚本位置计算。质量命令固定从仓库根目录执行。

## mini-agent 业务开发

本仓库已接入 C++ 业务（v1 已实现）。业务代码在 `src/` + `include/mini_agent/`，架构见 [系统架构](architecture.md)。

| 命令 | 用途 |
| --- | --- |
| `cmake --preset linux-gcc-debug && cmake --build --preset linux-gcc-debug` | 配置+构建（另有 `linux-clang-debug`、Release preset；Ninja） |
| `cmake --install build/linux-gcc-debug` | 安装 `mini-agent` 可执行文件；默认前缀 `~/.local`（即 `~/.local/bin/mini-agent`），显式 `-DCMAKE_INSTALL_PREFIX=…` 时安装到该前缀 |
| `ctest --test-dir build/linux-gcc-debug --output-on-failure` | CTest（`agent.core`、`starter.project_name`） |
| `./build/linux-gcc-debug/agent_test` | 单元/契约测试（chat/session/loop/config/log/md/tui 纯函数） |
| `./build/linux-gcc-debug/mcp_test` | v2 MCP 契约测试（RPC/对话层 fake transport + stdio/http mock E2E；ctest `mcp.client`） |
| `python3 tests/mock_mcp_server.py` | 手工：stdio mock MCP server（`/tools` 前的最小对端） |
| `python3 tests/mock_mcp_http_server.py 8933` | 手工：Streamable HTTP mock MCP server（SSE 响应 + Mcp-Session-Id） |
| `python3 tests/pty_tui_smoke.py ./build/linux-gcc-debug/mini-agent` | pty 端到端冒烟：真实伪终端驱动 TUI，mock SSE 服务，断言帧不变量/方向键/历史/中断/持久化（非质量命令，手工运行） |
| `python3 scripts/cpp_check.py` | 质量命令入口（configure → build → CTest），由 `check.py --strict-project` 调用 |
| `clang-format --dry-run -Werror <files>` / `python3 scripts/cpp_format.py` | 第一方 C++ 格式检查（风格见 `.clang-format`） |

运行配置（v2 工具层，面向使用者的完整指南见 [MCP 与工具使用指南](mcp-usage.md)）：`config.json` 的 `mcp_servers`（stdio 用 `command`/`args`/`env`，HTTP 用 `url`/`bearer_token`；`transport` 缺省时含 `url` 即推断为 http，`transport` 只能是 `stdio` 或 `http`——写 `https` 会使整份配置校验失败）、`tool_approval`（`ask` 默认/`allowlist`/`off`）、`tool_allowlist`、`max_tool_rounds`、`tool_timeout_seconds`；env 覆盖 `MINI_AGENT_TOOL_APPROVAL`、`MINI_AGENT_NO_MCP=1` 全禁 MCP。CLI：`ask --tools off|ask|auto`（默认 off，脚本契约不变）；TUI：`/tools`（列注册表并强制握手/重列）、`/mcp`。会话级注入：`ask`/`run`/`mcp list` 支持 `--mcp NAME=SPEC`（`NAME=https://…` 或 loopback http | `NAME=cmd args…` stdio | `NAME={json}` 全量字段；同名覆盖文件条目、不落盘、重启即失）；`mini-agent mcp list` 打印生效清单（bearer 恒脱敏、URL 剥 userinfo，未配 LLM 也可运行）。工具名命名空间为 `server__tool`（双下划线；单斜杠会被 OpenAI 兼容网关静默忽略，见 008 偏差 D1）。

原有配置：环境变量 `MINI_AGENT_BASE_URL`/`MINI_AGENT_API_KEY`/`MINI_AGENT_MODEL` 优先于 `~/.config/mini-agent/config.json`；也可全部不设，在 TUI 内用 `/provider`、`/login`、`/model` 配置。模板见 `.env.example`（注释为英文；真实密钥只放本地 `.env` 或 shell 环境）。

v4 模型元数据（设计 [010](design/010-model-metadata.md)）：`context_window`（正整数 token 数，状态栏 ctx"已用/最大"的分母与着色阈值来源；0/未设 = 只显示已用 token 单值；非法值整份配置失败关闭）与 `reasoning_effort`（字符串，非空即原样透传进每个 chat 请求；未设 = 请求体与 v3 字节一致）。env 覆盖 `MINI_AGENT_CONTEXT_WINDOW`、`MINI_AGENT_REASONING_EFFORT`；TUI 内 `/context <tokens>|off`、`/reasoning <value>|off` 即时设置并落盘。配置即权威来源：不探测、不被服务端应答覆盖。TUI 底部状态栏在 `model=… · session=…` 之后追加用量段：`ctx 45.2k/128.0k`（已占用/最大，按百分比 ≥70% 黄、≥90% 红；provider 未回 usage 时带 `~` 如 `ctx ~12.3k/128.0k`；未设窗口时单值 `ctx ~12.3k`）、`out 1.2k`（会话累计输出 token）、`reasoning=low`；全部未设/无数据时状态行与 v3 完全一致。token 用量经 `stream_options.include_usage` 从 SSE 末尾 usage chunk 采集（prompt 取末轮=实时上下文占用，completion/total 跨轮累加；失败 turn 不写入）；切模型复位读数。不支持 `reasoning_effort` 的网关报 400 时，TUI 提示 `/reasoning off`（配置值保留，不静默剔除）。

v4.2（设计 010 §11）：`/reasoning` 不带参数时，若上次 `/models` 拉到的能力元数据里有当前模型的候选档位（OpenRouter 等网关的 `reasoning.supported_efforts`），直接出编号菜单选择（标 `<- default`/`<- current`）；无档位数据时仍可 `/reasoning <字面量>`。状态栏 `reasoning=` 优先显示**响应实测**级别（v4.2.1 优先级：网关回显档位 > 被本轮思考证据确认的配置值 > 有证据但无任何档位知识时 `on` > 无证据回显配置值），证据来自回显字段、`delta.reasoning_content` 思考流与 `usage.reasoning_tokens` 三路；读数只反映最近一次成功 turn（无证据即清空，不残留），切模型复位。`context_window` 未配置时 ctx 分母兜底用网关 caps 的 `context_length`（OpenRouter 形态可得）。思考正文本期不显示、不存储。

运行时提示文案约定：面向用户的字符串（TUI/CLI 提示、状态行、历史前缀、补全菜单）一律英文；代码注释同为英文。设计/架构等中文文档是项目工作语言，不受此限。新增 UI 文案时同步更新 `tests/pty_tui_smoke.py` 中的对应断言。

## 接入业务技术栈

新项目先读 [语言配置](languages/README.md)。C++ 配置已提供 CMake/CTest 和可选分析工具；Python、TypeScript、Go、Rust 各有独立起步方案。Go/Rust 默认使用离线且不改写依赖清单的检查，工具链与新增依赖缓存需显式准备。已初始化项目应在分支中人工迁移，不重新运行初始化覆盖代码。

1. 选择技术栈和包管理器，添加依赖清单、锁文件、最小业务功能及测试。
2. 在 `project.json` 的 `quality_commands` 中声明命令，每条命令是对象：

```json
{
  "name": "业务单元测试",
  "argv": ["python", "-m", "unittest", "discover", "-s", "app_tests"],
  "timeout_seconds": 300
}
```

这是结构示例，generic 中 `app_tests` 尚不存在，Python 配置会生成该目录；请填写实际命令。参数必须分开，不支持 `&&`、重定向、变量展开等 shell 语法；`python` 作为首参数时使用运行检查脚本的同一 Python 解释器。不要让质量命令再次调用 `check.py`。

必须测试所选工具的实际启动命令；不要把不可信输入拼入参数。整个命令配置都属于可执行代码，需要审查。

1. 将格式检查、Lint、类型检查（适用时）、测试、构建分别配置；脚本只检查命令结构，不能判断质量门禁是否足够。
2. 在项目 CI 中准备对应运行时与锁定依赖。通用 `ci.yml` 仅配置 Python 并运行 tooling-only；不能把它当成业务通过。
3. 语言初始化会新增 `project.yml`，调用 `python scripts/check.py --strict-project`。generic 项目需自行建立对应门禁。TypeScript 先审查生成并提交 lockfile，CI 才能执行 npm ci。
4. 配置受保护分支、必要审查和 required status checks；仓库文件不能自动完成平台设置。

## 文档检查范围

检查第一方 Markdown 的本地相对文件链接是否存在，忽略 fenced code blocks 和外部 URL。不验证锚点、引用式链接、HTML 链接、远端可用性或 Markdown 全部语法。路径带空格时使用 URL 编码，避免复杂括号路径。

初始化后，`docs/` 中双花括号包围的大写标识符被视为未清除占位符；`templates/` 不参与占位符检查。需求中的“尚未定义”仍需人工评审，不会因脚本通过而变成完整需求。

## 工作方式

使用任务或 Issue 记录范围、参考来源和验收标准。重要功能先设计再实现，最终核对文档。小修复允许简化，但不能省略验证结果和风险说明。

第三方仓库可放在 `.references/`，固定到确认过的版本，不默认执行其脚本。该目录被忽略，但“只读”是操作约定，非文件系统权限控制。
