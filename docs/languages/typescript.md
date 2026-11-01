# TypeScript 项目配置与 AI 规范

## 初始化

```sh
python3 scripts/init_project.py --name web-core --description "项目目标" --language typescript --dry-run
python3 scripts/init_project.py --name web-core --description "项目目标" --language typescript
```

生成严格 TypeScript、ESM 示例、Node 内置测试、统一检查脚本和项目 CI。不默认选择 React/Vue、Web 服务框架、打包器或数据库。示例只返回项目名称。

需要 Node.js 22+ 和 npm。模板工具仍需要 Python 3.11+。

## 首次依赖安装与锁文件

初始化不执行 npm，也不会联网生成锁文件。`package.json` 中 TypeScript 固定为 5.7.3，这是明确的起步版本，不是最新版本或已完成安全审计的声明。使用前按公司政策核实版本；需要升级时同步审查锁文件与兼容性。

审查依赖来源并批准联网后，由用户执行：

```sh
npm install --ignore-scripts
python3 scripts/check.py --strict-project
```

检查并提交生成的 `package-lock.json`。之后本地和 CI 使用 `npm ci --ignore-scripts`。初始没有锁文件，生成的项目 CI 会明确失败，而不是悄悄解析新依赖。

这里的 `--ignore-scripts` 禁用 npm lifecycle scripts，不代表依赖代码本身可信。后续确实需要安装脚本的依赖，应单独审查并更新安装流程。

## 默认检查

`node scripts/ts_check.mjs` 依次执行：

1. 清理生成目录 `dist/`，其中不能放手写源码。
2. 使用本地 TypeScript 编译器做 `--noEmit` 类型检查。
3. 编译到 `dist/`。
4. 通过 `node --test` 运行应用测试。

任一步失败就停止，避免测试旧构建产物。脚本调用当前 Node 可执行文件，不依赖平台特定的 npm/cmd 包装器，因此统一质量命令在不同系统上更明确。

失败路径已实测（2026-09-14，Node v24.21.0）：未安装 `typescript@5.7.3` 时 `node scripts/ts_check.mjs` 在第一步以 `MODULE_NOT_FOUND` 退出 1，不会回退到全局编译器或假定通过。类型检查、构建与应用测试仍未执行：需先审查依赖、完成安装并生成 lockfile，见 [补做验证记录](../design/005-macos-verification.md)。

当前没有默认 ESLint、Prettier、浏览器测试或完整前端构建。添加框架后需要配置相应依赖、脚本和 CI，不把本配置当成已完成的前端工程。

## AI 开发规则

- 保持 strict 检查，不能为了消除报错随意使用 `any`、非空断言或忽略注释。
- 对不可信输入用 `unknown` 配合运行时校验；TypeScript 类型在运行时不会自动保护外部 JSON。
- 明确 null/undefined、可选字段、错误返回和异常语义。
- 正确等待 Promise，处理拒绝、取消和超时，避免浮动异步操作。
- 核对 ESM/CJS、Node/browser 边界、路径大小写和构建后导入路径。
- 不将服务端密钥放入前端环境变量或可交付 bundle。
- UI 项目另行确认组件边界、状态管理、可访问性和端到端测试。
- 变更依赖时更新并审查 lockfile；不自动升级整个依赖树。

设计文档按需补充运行时边界、输入校验、异步流程、构建产物、公开类型和兼容性。
