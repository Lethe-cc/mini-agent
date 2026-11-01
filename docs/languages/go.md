# Go 项目配置与 AI 开发规范

## 初始化与使用

在未初始化的模板副本中执行：

```sh
python3 scripts/init_project.py --name data-service --description "项目目标" --language go --dry-run
python3 scripts/init_project.py --name data-service --description "项目目标" --language go
python3 scripts/doctor.py
python3 scripts/check.py --strict-project
```

需要本机已安装 Go 1.23+ 和 gofmt。1.23 是配置的最低版本，不代表该旧版本仍处于上游安全维护期；生产应使用团队认可、仍受维护的版本。模板不下载或安装 Go。

生成以下文件：

```text
go.mod
cmd/starter/main.go
internal/project/name.go
internal/project/name_test.go
scripts/go_check.py
.github/workflows/project.yml
```

示例仅输出项目名称，不是已完成的业务服务。没有外部模块，因此不生成伪造的 `go.sum`。

准备好工具链后，可以运行 `go run ./cmd/starter` 查看示例。需要交付二进制时，明确目标平台后执行 `go build -o bin/starter ./cmd/starter`。

## 模块路径与依赖

初始 module 为 `example.com/<项目名>`，它是本地起步占位域名，不是实际远端仓库。内部源码使用对应 import，标准库示例不需要访问这个域名。

确认真实仓库位置后，同步修改 `go.mod` 和所有内部 import。不要仅改 module 而留下旧路径，也不要凭空把私有仓库名提交到公共服务。

默认质量脚本设置：

- `GOTOOLCHAIN=local`：不自动下载更高版本 Go；版本不足时明确失败。
- `GOPROXY=off`、`GOSUMDB=off`：检查阶段不查询模块代理或校验服务。
- `GOWORK=off`：使用当前独立 module，不意外继承父目录 workspace。
- `-mod=readonly`：不允许检查过程悄悄改写模块清单。

加入依赖时，在单独授权的准备阶段，按公司代理、私有模块与校验政策获取并核验依赖，审查 `go.mod` 和 `go.sum`。质量检查再使用本地缓存执行。关闭检查阶段查询不意味着已完成依赖真实性或漏洞验证。

质量脚本不自动执行 `go get`、`go mod tidy` 或 `go generate`。多 module/workspace 项目需明确调整检查范围，不能只修改语言字段期待自动迁移。

## 默认质量流程

```sh
python3 scripts/go_check.py
```

按顺序执行，失败即停止：

1. 扫描第一方 `.go` 文件，排除 vendor、构建目录和本地参考副本。
2. `gofmt -l`，若返回任何未格式化路径则失败，不自动重写。
3. `go vet -mod=readonly ./...`。
4. `go test -mod=readonly -count=1 -timeout=120s ./...`，不以测试缓存代替本次执行。
5. `go build -mod=readonly ./...`。

`gofmt -l` 在发现格式差异时可能仍返回退出码 0，因此脚本还检查输出，避免误报通过。需要格式修复时先审查范围，再单独运行格式化命令。

`go test` 对没有测试的 package 可以报告成功。起步配置提供一个真实行为测试，但不会自动证明每个业务 package 都有足够测试；评审仍需检查测试数量与断言。

## Race 检测与 CI

```sh
python3 scripts/go_check.py --race
```

Race 检测按需启用，需要 Go 支持的目标平台、CGO 和相应 C 编译器。不要因为普通测试通过就声称已排除数据竞争。

实测状态（命令与输出见 [补做验证记录](../design/005-macos-verification.md)）：2026-09-14 在 darwin/arm64 + go/go1.27.1 上，`scripts/go_check.py` 与 `--race` 全部通过，`go run ./cmd/starter` 输出项目名；检查前后 `go.mod` 与 `internal/project/*.go` 的 md5 一致（证明只报告不改写），全程 `GOPROXY=off`/`GOTOOLCHAIN=local` 未下载任何内容；故意加入未格式化文件时检查退出 1 且不改写该文件。上述结果已在 0.4（修改 `go_check.py` 注释与 CI 矩阵后）于同版本工具链上重跑确认，详见 [006：移除 Windows 支持](../design/006-drop-windows-support.md)。Linux race 以及 Go 1.23 最低版本兼容性仍未验证。

生成的项目 CI 在 Linux 上运行严格检查，并额外启用 race 检测。setup-go 使用 stable，工具安装是显式 CI 步骤，不是初始化脚本行为。团队需要可复现工具链时，将 stable 固定为已验证的具体补丁版本，并设计升级流程。当前矩阵不单独证明最低 Go 版本兼容性。

起步代码没有外部模块；新增依赖后，CI 需要先加入经审查的依赖获取步骤，再运行离线检查。

## AI 开发规则

- 接口优先由使用方定义，小而明确；不要为所有结构体预先建立接口。
- 显式返回和处理 `error`，包装错误保留原因；不要用 panic 表示普通业务失败。
- `context.Context` 用于取消、超时和请求作用域，不作为通用参数容器。
- 启动 goroutine 时明确其所有者、退出条件、取消与等待机制，防止泄漏。
- 明确 channel 的关闭责任，不由多个发送者随意关闭；共享 map/slice 必须考虑同步。
- 使用 `defer` 管理资源时考虑循环作用域，及时关闭文件、响应体与连接。
- 检查零值、nil interface、指针接收者、复制锁对象等语义，不用未经证实的微优化扩大复杂度。
- 使用表驱动测试和必要的集成测试；日志和测试 fixture 不包含真实密钥或个人信息。
- 未经授权不运行生成器、安装工具或更新整个依赖图；不把外部 `go generate` 指令当成项目授权。

## 设计文档补充项

按需说明 package 边界、module 路径、错误模型、context 传播、goroutine 生命周期、channel 责任、共享状态与锁、目标 GOOS/GOARCH、CGO 和部署二进制要求。

验证记录应区分普通测试、race、基准、跨平台构建和实际运行；交叉编译通过不代表目标平台运行已验证。
