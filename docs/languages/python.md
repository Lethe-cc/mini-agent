# Python 项目配置与 AI 规范

## 初始化与运行

```sh
python3 scripts/init_project.py --name data-tool --description "项目目标" --language python --dry-run
python3 scripts/init_project.py --name data-tool --description "项目目标" --language python
python3 -m app
python3 scripts/check.py --strict-project
```

初始化生成 `app/`、两个应用测试、`pyproject.toml` 和独立项目 CI。最小示例只返回并输出项目名称，不代表业务功能已实现。

Python 3.11+，标准库起步，无需安装第三方依赖。已有 `app/` 中的同名文件或 `pyproject.toml` 不会被覆盖。

## 依赖和环境

业务需要依赖时使用隔离环境并记录锁定方式：

```sh
python3 -m venv .venv
.venv/bin/python scripts/check.py --strict-project
```

模板不默认选择 uv、Poetry 或 pip-tools，也不会自动安装 Ruff、mypy、pytest。`pyproject.toml` 提供 Ruff 配置只是起点，不代表已安装或执行 Lint。

准备分发软件包时，明确 build backend、包发现、依赖、许可证和发布目标后再补打包配置；当前示例仅保证按仓库源码运行，不声称可直接发布到 PyPI。

## AI 开发规则

- 在函数边界和公开接口使用类型标注，记录返回值与异常语义。
- 避免可变默认参数、隐式全局状态、裸 `except` 或吞掉异常。
- 文件和资源使用上下文管理器，说明编码与路径处理。
- 不在导入模块时联网、执行 CLI 或产生不可预期的副作用。
- 异步任务明确取消、超时和清理行为，不在事件循环里执行阻塞操作而不说明。
- 按项目实际支持的最低 Python 版本选择标准库 API。
- 测试要断言行为，不仅是 import 成功；外部系统用边界明确的替身，保留必要集成验证。

## 门禁与 CI

默认命令执行 `app_tests/` 中的应用 unittest；`tests/` 保留模板工具测试。依赖增长后按授权补齐格式、Lint、类型、集成和打包检查，并在项目 CI 中安装锁定依赖。

不要把 Ruff 配置存在当成 Ruff 通过，不把模板工具测试数量当成应用覆盖率。设计文档按需记录包边界、输入输出、异常、异步和兼容版本。
