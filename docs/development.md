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

脚本中的项目路径根据脚本位置计算。质量命令固定从仓库根目录执行。

## mini-agent 业务开发

本仓库已接入 C++ 业务（v1 已实现）。业务代码在 `src/` + `include/mini_agent/`，架构见 [系统架构](architecture.md)。

| 命令 | 用途 |
| --- | --- |
| `cmake --preset linux-gcc-debug && cmake --build --preset linux-gcc-debug` | 配置+构建（另有 `linux-clang-debug`、Release preset；Ninja） |
| `ctest --test-dir build/linux-gcc-debug --output-on-failure` | CTest（`agent.core`、`starter.project_name`） |
| `./build/linux-gcc-debug/agent_test` | 单元/契约测试（chat/session/loop/config/log/md/tui 纯函数） |
| `python3 tests/pty_tui_smoke.py ./build/linux-gcc-debug/mini-agent` | pty 端到端冒烟：真实伪终端驱动 TUI，mock SSE 服务，断言帧不变量/方向键/历史/中断/持久化（非质量命令，手工运行） |
| `python3 scripts/cpp_check.py` | 质量命令入口（configure → build → CTest），由 `check.py --strict-project` 调用 |
| `clang-format --dry-run -Werror <files>` / `python3 scripts/cpp_format.py` | 第一方 C++ 格式检查（风格见 `.clang-format`） |

运行配置：环境变量 `MINI_AGENT_BASE_URL`/`MINI_AGENT_API_KEY`/`MINI_AGENT_MODEL` 优先于 `~/.config/mini-agent/config.json`；也可全部不设，在 TUI 内用 `/provider`、`/login`、`/model` 配置。模板见 `.env.example`（注释为英文；真实密钥只放本地 `.env` 或 shell 环境）。

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
