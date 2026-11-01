# 003：Go 与 Rust 起步配置

## 状态与范围

- 版本：模板 0.3.0；schema_version 保持 1。
- 状态：已实现，0.3 的 46 个工具测试通过；本地缺少 Go/Rust 工具链，未完成实际编译；无人工审批通过记录。
- 需求：在已有可选语言机制上增加 Go 和 Rust，不改变 generic、C++、Python、TypeScript 项目的初始化行为。
- 本设计补充 [语言配置设计](002-language-profiles.md)，不重用旧版测试结果作为新增能力的证据。

## 配置与初始化

`LANGUAGES` 增加 go/rust，配置默认 stack 文本。沿用现有显式 manifest、create-only 路径校验、dry-run、冲突拒绝和写入失败恢复机制，不新增下载或命令执行步骤。

Go/Rust 的同名 manifest、源码或项目 CI 已存在时拒绝覆盖。`doctor` 只报告可执行文件是否在 PATH，不安装环境或声称 compiler/linker 可用。

`AGENTS.md` 按项目主语言读取规范。文档、开发指南、完整手册、索引和忽略规则同步扩展。

## Go 方案

- 标准库起步，最低版本声明 Go 1.23.0。
- 示例 module 为 example.com 加仓库 slug，CLI 放在 cmd/starter，业务示例放在 internal/project。
- 一项真实函数行为测试，无外部依赖，不生成没有依据的 go.sum。
- 检查顺序为 gofmt、vet、test、build，失败即停止。gofmt 既检查退出码也检查未格式化路径输出。
- 测试使用 count=1 避免缓存替代本次运行；race 为显式选项。
- 检查时固定 GOTOOLCHAIN=local、GOWORK=off、GOPROXY=off、GOSUMDB=off，并使用 mod=readonly。
- CI 使用显式安装的 stable Go 并运行 race 检测（0.3 交付时矩阵含 Windows、race 仅在 Linux；0.4 起矩阵只有 Linux，见 [006：移除 Windows 支持](006-drop-windows-support.md)）。新增依赖后需单独授权准备缓存。

## Rust 方案

- Rust 1.85+、edition 2024，标准库起步。
- package/lib/bin 使用稳定的 starter-app/starter_core/starter-cli，避免将任意仓库 slug 直接当成 Rust 标识符。业务名称由函数返回。
- 初始 Cargo.lock 为只有根 package 的确定性锁文件，无未经解析的依赖和校验和；publish=false。
- 提供库单元测试、CLI 集成测试和文档测试。
- 默认禁止 unsafe。检查顺序为 fmt、Clippy、test、build，失败即停止；不使用会跳过文档测试的 test all-targets 参数。
- Cargo 使用 offline/locked，环境禁用隐式 rustup 安装。Release 可选，并在生成的 CI 中执行。
- CI 显式安装 stable、rustfmt 和 Clippy；本地初始化及质量检查不安装。（0.3 交付时矩阵含 Windows，其链接依赖 runner 的 C++ 工具链；0.4 起矩阵只有 Linux，见 [006：移除 Windows 支持](006-drop-windows-support.md)。）

## 规范与文档优化

Go 强调错误处理、context、goroutine/channel 生命周期、共享状态与资源关闭。Rust 强调所有权/借用、Result、unsafe/FFI、不使用 clone 掩盖设计问题，以及 target/features/MSRV 边界。

最终设计应记录这些语言特有取舍，验证结果区分静态检查、测试、race、Release、目标平台运行和未验证部分。

## 安全与兼容性

- 默认离线只限制工具的依赖解析，不是测试或构建代码的网络沙箱。
- Go 依赖缓存、Cargo build.rs、过程宏和测试仍需审查，不能给不可信构建提供凭据。
- Rust target 输出和显式 Go bin 输出被忽略；Cargo.lock 保持可跟踪。
- 不自动迁移混合语言、Go workspace、Rust workspace、CGO、no_std 或交叉编译项目。
- stable CI 不是完整版本固定，也不单独证明最低版本兼容性；团队需补自己的矩阵和升级策略。

## 验证记录

已执行 `py -3 scripts/check.py --tooling-only`：配置与文档检查通过，46 个工具测试全部通过，包括新配置、失败路径、只读/离线参数与 Cargo manifest/lock 一致性测试。

PATH 检查没有找到 go、gofmt、cargo、rustc 或 rustup。本次没有安装任何工具链；Go/Rust 实际格式、编译、静态分析和测试运行尚未验证。随后分别在临时副本中完成 Go 和 Rust 初始化，并通过严格模式的基础配置与文档校验。实际质量命令因工具缺失明确跳过；该验证进程退出成功只表示已执行的生成检查通过，不代表 Go/Rust 编译、静态分析或应用测试通过。

若工具缺失或运行环境不完整，应记录具体阻塞；不能把 mock 测试、配置生成或编译器命令存在等同于真实构建通过。远端 CI 尚未运行。

## 后续补做（2026-09-14）

[005：macOS 补做验证记录](005-macos-verification.md) 已将 Go 从「未执行」改为真实验证通过：darwin/arm64 + go1.27.1 下 gofmt/vet/test/build 与 `--race` 均通过，检查前后源文件 md5 一致，未格式化输入的失败路径也已在真实工具链上复现。Rust 仍只完成 manifest/lock 结构与失败路径（缺 `Cargo.lock` 时在调用 cargo 前退出 1；无工具链时明确报错退出 1），其 fmt/Clippy/test/build 仍未执行。两者均不覆盖远端 CI。
