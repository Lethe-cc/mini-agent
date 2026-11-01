# 语言配置入口

模板采用通用核心加可选语言配置。AI 首先读取 `project.json` 的 `language`，再读取对应语言规范；不要每次全量加载所有语言文档。

| language | 自动生成 | 默认检查 | 操作说明 |
| --- | --- | --- | --- |
| `generic` | 仅项目元数据，不生成业务代码 | 模板工具检查；项目命令由用户填写 | [完整手册](../user-guide.md) |
| `cpp` | C++17 库、CLI、测试、Linux GCC/Clang presets、四组合 CI、可选分析工具 | 配置 → 编译 → CTest，Linux 默认 GCC Debug | [C++ 指南](cpp.md) |
| `python` | 标准库应用、unittest、pyproject、项目 CI | 应用 unittest | [Python 指南](python.md) |
| `typescript` | 严格 TypeScript、ESM 示例、Node 测试、项目 CI | 类型检查 → 构建 → Node 测试 | [TypeScript 指南](typescript.md) |
| `go` | Go module、CLI、internal 包、测试和项目 CI | gofmt → vet → test → build；可选 race | [Go 指南](go.md) |
| `rust` | Cargo 库/CLI、锁文件、单元/集成/文档测试、项目 CI | fmt → Clippy → test → build；可选 Release | [Rust 指南](rust.md) |

默认仍是 `generic`，避免替未选型项目做决定。C++ 是重点支持配置，但其他语言项目不会附带 CMake 文件，各语言骨架相互独立。

## 初始化新项目

在干净的、未初始化的模板副本中运行：

```sh
python3 scripts/init_project.py --name my-cpp-project --description "项目目标" --language cpp --dry-run
python3 scripts/init_project.py --name my-cpp-project --description "项目目标" --language cpp
```

这里优先展示主要目标 Linux 的操作；macOS 用户请使用 `python3`。

Go 或 Rust 将命令中的 `--language cpp` 改为 `--language go` 或 `--language rust`；请在不同的新项目副本中分别初始化。

`--stack` 可省略，使用所选配置的默认技术栈描述；也可填写更具体的目标环境。`--stack` 只是描述，不会切换编译器或修改 CMake 标准。

初始化只创建选定 profile manifest 声明的文件。已有同名文件或已有质量命令时拒绝覆盖。`--dry-run` 展示全部计划，不写文件。不会下载依赖、执行编译、初始化 Git 或推送代码。

生成的代码只实现“返回项目名称”的最小行为，其测试证明工具链能连接起来，不代表你的业务完成。之后仍需完善需求、参考、架构和业务验收。

## 质量与 CI 分层

- `python scripts/check.py --tooling-only`：显式只检查模板工具和文档，跳过所有项目命令。不能作为业务合并门禁。
- `python scripts/check.py --strict-project`：运行工具检查和已配置的项目质量命令。非 generic 配置自动接入最小起步命令。
- `.github/workflows/ci.yml`：通用工具矩阵，使用 tooling-only。
- `.github/workflows/project.yml`：初始化对应语言时新增，运行该语言的严格门禁；不会覆盖已有同名工作流。

项目必须将适用的 project 检查配置为 required checks。TypeScript 首次需生成并审查锁文件，否则其 CI 会有意失败。Go 起步没有外部模块；Rust 提供只含根 package 的确定性无依赖锁文件。二者默认检查不下载工具链或依赖，新增依赖需要显式准备本地缓存。GitLab 使用者需移植对应流水线，本模板不自动转换。

## 已有项目如何采用

不要在已初始化项目里重新运行初始化，也不要仅修改 `language` 期待自动迁移。

1. 建立备份或单独 Git 分支。
2. 在临时模板副本中生成目标语言配置，比较差异。
3. 人工或经授权由 AI 合并适用的构建、规范、测试和 CI，不覆盖业务源码。
4. 更新 `project.json` 的 `language`、`stack`、`quality_commands`，同步项目背景和架构。
5. 执行真实项目检查，再审查合并。

混合语言项目以主语言为入口，在任务中指出涉及的其他语言规范，并手工组合质量命令。这一版不支持一次初始化多个 profile，也不提供自动迁移。

## 扩展其他语言

在 `profiles/` 增加本地 manifest 与 create-only 文件模板，在 `scripts/profiles.py` 登记语言和默认描述，补配置校验、对应语言文档、初始化及失败保护测试。不要只增加一篇文档便声称已支持完整工具链。
