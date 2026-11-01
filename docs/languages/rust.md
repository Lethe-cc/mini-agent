# Rust 项目配置与 AI 开发规范

## 初始化与运行

在未初始化的模板副本中执行：

```sh
python3 scripts/init_project.py --name data-core --description "项目目标" --language rust --dry-run
python3 scripts/init_project.py --name data-core --description "项目目标" --language rust
python3 scripts/doctor.py
python3 scripts/check.py --strict-project
```

需要已安装 Rust 1.85+、Cargo、rustfmt、Clippy，以及目标平台链接器。采用 edition 2024，`rust-version = "1.85"` 是最低版本声明，不是强制下载工具链或生产版本推荐。

初始化不执行 rustup，不下载工具链或 crates。只有 rustc/cargo 命令可见不代表链接环境完整。

生成文件：

```text
Cargo.toml
Cargo.lock
rustfmt.toml
src/lib.rs
src/main.rs
tests/cli.rs
scripts/rust_check.py
.github/workflows/project.yml
```

示例只返回并输出项目名称，附有库单元测试、CLI 集成测试和文档测试。它们验证起步行为，不代替业务测试。

## Cargo 名称与锁文件

为兼容所有合法仓库 slug（包括数字开头的名称），骨架固定使用：

- Cargo package：`starter-app`。
- library crate：`starter_core`。
- binary：`starter-cli`。

项目实际名称仍记录于 `project.json` 并由示例函数返回。需要重命名 crate 时，同时更新 Cargo manifest、源码引用、文档测试、CLI 集成测试和锁文件，不仅修改仓库名。

默认 `publish = false`，不会意外将内部示例发布到 crates.io。

起步没有第三方依赖，因此附带与 Cargo.toml 一致、只有根 package 的确定性 Cargo.lock。它不包含未经获取的依赖、伪造校验和或猜测的解析结果。该模板包含 CLI，默认提交 lockfile，不将其加入忽略规则。

新增依赖或重命名 package 后，在授权的准备阶段使用 Cargo 更新锁文件并审查差异。无外部依赖时可显式使用 `cargo generate-lockfile --offline`；缺少缓存时不得假称离线依赖已经准备好。

## 默认质量门禁

```sh
python3 scripts/rust_check.py
python3 scripts/rust_check.py --release
```

脚本先检查 Cargo.lock 存在，再按顺序执行：

1. `cargo fmt --all -- --check`，只检查不重写。
2. `cargo clippy --offline --locked --all-features --all-targets -- -D warnings`。
3. `cargo test --offline --locked --all-features`，保留 Cargo 默认的单元、集成和文档测试。
4. `cargo build --offline --locked --all-features --all-targets`。

`--release` 会对 Clippy、测试和构建统一增加 Release 标志，格式检查不受影响。前一步失败立即停止。

同时设置 `CARGO_NET_OFFLINE=true` 和 `RUSTUP_AUTO_INSTALL=0`。缺少工具链、rustfmt/Clippy、链接器或依赖缓存时应失败，由使用者明确准备环境，不能让质量检查静默安装工具或更新锁文件。

上述失败语义已于 2026-09-14 在 macOS 上实测：缺 `Cargo.lock` 时在调用 cargo 前退出 1；不存在静默跳过。`Cargo.toml`/`rustfmt.toml` 经 `tomllib` 解析确认与文档一致（edition 2024、`rust-version` 1.85、`publish = false`、`unsafe_code = "forbid"`、无外部依赖）。

**test/build 已真实执行并通过**（2026-09-14，darwin/arm64 + cargo/rustc 1.98.1）：`cargo build --offline --locked --all-features --all-targets` 与 `--release` 均退出 0 并真实编译；`cargo test --offline --locked --all-features` 退出 0，库单元测试 1/1、CLI 集成测试 1/1、文档测试 1/1 全部通过。

**完整四步门禁已真实跑通**（同日补做，darwin/arm64 + rustfmt 1.9.0 + clippy 0.1.98 + cargo/rustc 1.98.1）：`python3 scripts/rust_check.py` 与 `--release` 均退出 0，fmt → clippy → test → build 四步全部真实执行。

为避免把「退出 0」误当成「工具真的跑了」，做了两组反证：

| 反证 | 做法 | 结果 |
| --- | --- | --- |
| fmt 真实生效 | 向 `src/lib.rs` 追加 `pub fn    badly_formatted(  )->String{...}` | `cargo fmt --check` 输出 diff 并退出 1，门禁在第一步就停止 |
| Clippy 真实生效 | 追加一个 `for i in 0..v.len()` 索引循环 | `-D warnings` 下报 `clippy::needless_range_loop` 与 `clippy::items-after-test-module`，退出 101 |

两组违规还原后完整门禁重新退出 0，证明失败与通过都由真实分析驱动，而非缓存或跳过。

早期记录见 [补做验证记录](../design/005-macos-verification.md)：当时本机无 cargo/rustc/rustup，只验证了缺 `Cargo.lock` 时先于调用 cargo 退出 1、以及缺工具链时 `cargo fmt` 明确报错不静默跳过。上述结果均为 macOS/arm64 单机数据，**不证明 Linux CI 通过**；仍无 `rustup`，因此未验证多工具链与 MSRV 1.85 兼容性。

当前为单 package，无 feature 冲突。以后出现互斥 features、多 crate workspace、no_std 或特定 target 时，应重新设计 feature/target 矩阵，不直接把 `--all-features` 当成普遍正确的方案。

## 工具链与 CI

不生成自动触发工具链下载的 `rust-toolchain.toml`。本地默认使用已安装的当前工具链；若需要固定版本，在明确安装授权后，团队可使用指定工具链或增加经审查的配置。

生成的 CI 在 Linux 上显式准备 stable、rustfmt 和 Clippy，再运行严格检查和 Release 检查。CI 安装是可见的环境准备步骤，与本地初始化不安装工具的约定不同。

stable 会随上游更新；需要可复现结果时固定经验证的补丁版本，并额外配置最低 Rust 版本测试。当前矩阵不单独证明 MSRV 1.85 兼容性。

`--offline` 限制 Cargo 依赖访问，不是完整网络沙箱。依赖中的 build.rs、过程宏、测试代码仍可执行任意逻辑；授权引入前应审查，不向不可信构建提供生产凭据。

## AI 开发规则

- 优先用所有权、借用和明确的生命周期表达资源关系，不为了绕开 borrow checker 大量 clone。
- 返回借用或持有引用前核对来源寿命；不要用任意增加 `'static` 隐藏生命周期问题。
- 普通失败使用 `Result`/`Option`，为公开 API 明确错误语义；不在生产路径无理由 unwrap/expect/panic。
- 测试中可以用清晰的 unwrap/expect 表示断言失败，但不能把错误吞掉而让测试继续通过。
- 初始 manifest 对 unsafe 使用 forbid。确需 FFI/unsafe 时，先评审需求、安全不变量和验证方法，再显式调整策略，不能偷偷删 lint。
- 明确 `Send`/`Sync`、Arc/Mutex、锁持有范围和任务退出语义，不在异步等待时无故持锁。
- 不默认引入 Tokio、serde、错误框架或数据库；先确认运行时、依赖与 feature 约束。
- 修改公开 trait、类型、feature、序列化格式或 FFI ABI 时检查兼容性。
- 关注 Drop、取消、部分初始化和失败路径的资源清理，不把“通过编译”当成业务正确性证明。

## 设计文档补充项

按需记录 crate 和 module 边界、所有权/借用策略、错误类型、unsafe/FFI 不变量、线程与异步模型、feature 组合、MSRV、edition、目标 triple、链接器和打包方式。

验收区分格式、Clippy、单元/集成/文档测试、Release、Miri、Sanitizers、交叉编译和目标平台实际运行。未执行的工具不能写成通过。
