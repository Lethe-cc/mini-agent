# 005：macOS 补做验证记录

## 文档状态

- 状态：已执行；本轮为 [002：语言配置](002-language-profiles.md)、[003：Go/Rust](003-go-rust-profiles.md)、[004：Linux C++](004-linux-cpp.md) 中列为「未执行」的项目补做真实验证。
- 核对日期：2026-09-14（CST）。
- 适用版本：模板 0.3.1，`schema_version` 1，母模板保持未初始化 generic 状态。
- 人工评审：无评审通过记录；本文只记录自动执行过的命令与输出。
- 本文不改变 002/003/004 的历史结论，也不把本轮结果反向解释为 Linux 或 Windows 已通过。

## 验证环境

全部命令在一台 macOS（Darwin 25.6.0，系统版本 26.6.2，arm64）上执行，语言骨架初始化在 `$TMPDIR` 下的模板临时副本中进行。

| 工具 | 版本或位置 |
| --- | --- |
| Python | 3.12.3（`/Library/Frameworks/Python.framework/.../python3`） |
| CMake / CTest | 4.4.3 |
| Ninja | 1.13.2 |
| C++ 编译器 | Apple clang 21.0.0（`/usr/bin/clang++`，CMake 识别为 AppleClang 21.0.0.21000101，target arm64-apple-darwin25.6.0） |
| clang-format | 23.1.1（Homebrew） |
| clang-tidy | 不在 PATH，Xcode 也未提供 |
| Go | 1.27.1 darwin/arm64（`/usr/local/go/bin`，不在默认 PATH，验证时显式加入） |
| Rust | 无 cargo、rustc、rustup |
| Node / npm | v24.21.0 / 同前缀 npm |
| GCC（真正的 g++） | 无；macOS 的 `g++`、`gcc` 是 Apple clang 别名，不能当作 GCC 证据 |
| 容器或虚拟机 | 无 docker、podman、colima、lima、multipass，本机不是 Linux 主机 |
| AI 客户端 | pi 0.84.4（`/opt/homebrew/bin/pi`） |

## 已执行结果

| 项目 | 实际命令或证据 | 结果 |
| --- | --- | --- |
| 母模板工具回归 | `python3 scripts/check.py`（48 个 unittest + 基线配置与文档检查） | 通过。这是 0.3.1 在 macOS + Python 3.12 上的新数据点，不替换 0.1/0.2 的 Windows + 3.14 记录 |
| C++ 初始化 | 临时副本 `init_project.py --name sensor-core --language cpp` | 通过，按 manifest 创建 13 个路径，无覆盖 |
| C++ Debug 真实构建 | `python3 scripts/cpp_check.py`（默认 `dev`：Ninja + AppleClang） | 通过：configure → 6 个编译/链接步骤 → `ctest --preset dev --no-tests=error` 1/1 Passed |
| C++ Release 真实构建 | `python3 scripts/cpp_check.py --preset release` | 通过，CTest 1/1 Passed，独立 `build/release` 目录 |
| C++ 告警即错误 | `cmake --preset dev -DSTARTER_WARNINGS_AS_ERRORS=ON` + `cmake --build` | 通过：`-Wall -Wextra -Wpedantic -Werror` 下 6/6 干净构建 |
| C++ Sanitizers 门禁 | `cmake --preset dev -DSTARTER_SANITIZERS=ON` | 按设计失败：`CMake Error at CMakeLists.txt:32: STARTER_SANITIZERS requires Linux GCC/Clang`。只验证了非 Linux 的拒绝路径，Linux 正路径仍未验证 |
| C++ clang-tidy 门禁 | `cmake --preset dev -DSTARTER_CLANG_TIDY=ON` | 按设计失败：`Could not find CLANG_TIDY_EXECUTABLE`。本机没有 clang-tidy，正路径未验证 |
| C++ 格式检查 | `python3 scripts/cpp_format.py`（clang-format 23.1.1 `--dry-run --Werror`） | 通过，未改写文件 |
| C++ 起步行为 | `./build/dev/starter_cli` | 输出 `sensor-core`，退出码 0 |
| C++ 严格项目检查 | 另一干净副本 `python3 scripts/check.py --strict-project` | 通过（工具测试 + `cpp_check.py` 真实编译与 CTest） |
| CMake presets 平台可见性 | `cmake --list-presets` 与 `cmake --list-presets=all` | 与 004 记录一致：configure 段隐藏 `linux-*`（host condition 为假，`--all` 也仍隐藏）与 `windows-msvc`（生成器不可用）；build/test 段无条件列出全部 8 个 preset 名（0.3.1 当时状态；0.4 起 configure/build/test 各为 6 个，Windows preset 已移除） |
| Go 初始化与真实检查 | 临时副本 `--language go`，`PATH=/usr/local/go/bin` + `python3 scripts/go_check.py` | 通过：gofmt 3 文件无输出、`go vet`、`go test -count=1`（internal/project ok）、`go build`，全部 `-mod=readonly` |
| Go race 选项 | `python3 scripts/go_check.py --race` | 通过（`-race` 测试 ok）。仅证明本机 darwin/arm64 可运行，不替代 CI 的 Linux race |
| Go 只读与不改写 | 检查前后 `go.mod`、`internal/project/*.go` 的 md5 对比 | 完全一致：gofmt 只报告不改写，`-mod=readonly` 未改动依赖清单 |
| Go 失败路径 | 临时放入未格式化文件后运行 `go_check.py` | 失败退出 1 并列出未格式化路径，文件未被改写 |
| Go 起步行为与严格检查 | `go run ./cmd/starter`；`python3 scripts/check.py --strict-project` | 输出 `csv-go`；严格检查通过 |
| Go 离线约束 | `go_check.py` 固定 `GOTOOLCHAIN=local`、`GOPROXY=off`、`GOSUMDB=off`、`GOWORK=off` | 通过：全程未下载工具链或模块 |
| Python 初始化与严格检查 | 临时副本 `--language python` + `python3 scripts/check.py --strict-project` | 通过：工具测试 + 2 个 `app_tests` 起步测试 |
| Rust manifest 结构 | 临时副本 `--language rust` 后以 `tomllib` 解析 `Cargo.toml`、`rustfmt.toml` | 通过：edition 2024、`rust-version` 1.85、`publish = false`、`unsafe_code = "forbid"`、无 dependencies；`Cargo.lock` 存在 |
| Rust 缺锁文件保护 | 移走 `Cargo.lock` 后 `python3 scripts/rust_check.py` | 按设计失败：`FAIL: Cargo.lock is missing; ...`，在调用 cargo 之前退出 1 |
| Rust 缺工具链行为 | 恢复锁文件后再次运行 | 按设计失败：`cargo fmt` 步骤 `No such file or directory: 'cargo'`，退出 1，没有静默跳过 |
| TypeScript 无依赖失败路径 | 临时副本 `--language typescript` 后 `node scripts/ts_check.mjs` | 按设计失败：`MODULE_NOT_FOUND`（缺 `node_modules/typescript/bin/tsc`），退出 1 |
| 工作流 YAML 结构 | 以 `YAML.load_file` 解析 4 个生成的 `project.yml` 与通用 `ci.yml` | 全部可解析；`ci.yml` 为 `permissions: contents: read`、checkout `persist-credentials: false`、矩阵 ubuntu/windows × 3.11/3.13、步骤仅 `doctor.py` 与 `check.py --tooling-only`（0.3.1 当时状态；0.4 起矩阵仅 ubuntu） |
| AI 入口自动发现（pi 0.84.4） | 在模板临时副本作为工作区根目录时执行 `PI_OFFLINE=1 pi -p "<只依据已加载上下文复述规则>"` | 通过：未调用任何工具、未读文件、未执行命令的前提下正确复述 `AGENTS.md` 的任务前读取顺序与 `check.py` 三种模式（并区分 `--tooling-only` 是附加标志） |
| 同客户端对照组 | 同提示词 + `--no-context-files` | 通过（预期失败语义）：明确回答「未加载到上下文」，未编造规则内容 |
| 工作区根目录约定 | 本轮 pi 会话本身以模板的父目录为 cwd | 印证 [语言配置入口](../languages/README.md) 与 README 第 2 步：pi 从 cwd 向上查找上下文文件，打开父目录不会加载子目录的 `AGENTS.md` |

## 未执行与阻塞原因

以下项目本轮仍为「未执行」，不能由上表推断：

| 未验证项 | 阻塞原因 | 需要的准备 |
| --- | --- | --- |
| Linux GCC/Clang × Debug/Release 四组合编译与 CTest（004 的主要目标） | 本机不是 Linux，且无 docker/podman/lima 等可用运行环境；Apple clang 不能冒充 GCC | Linux 主机、容器或虚拟机；授权安装或连接 |
| Sanitizers 正路径（ASan/UBSan 实际运行） | 同上，CMake 显式要求 Linux | 同上 |
| clang-tidy 正路径 | 本机未安装 clang-tidy | 安装 LLVM 工具集并审查版本。**0.4 更新**：本机其后已装 clang-tidy 23.1.1，macOS `dev` preset 下的正路径**已真实验证**（cache 写入、verbose 日志调用 3 次、故意 `NULL` 违规被检出）；Linux GCC/Clang 下的 tidy 正路径仍未验证。见 [006：移除 Windows 支持](006-drop-windows-support.md)。 |
| Rust 真实 `fmt --check`、Clippy `-D warnings`、test（含文档测试）、build | 本机无 cargo/rustc/rustup | 授权安装 Rust 工具链；无依赖时仍需离线缓存约定。**0.4 更新**：本机其后已有 cargo/rustc 1.98.1、rustfmt 1.9.0、clippy 0.1.98，该行**全部四项均已真实执行并通过**，不再是未验证项：`rust_check.py` 与 `--release` 退出 0，fmt → clippy `-D warnings` → test（单元/集成/文档各 1/1）→ build 四步齐备；并经两组反证（故意破坏格式 → fmt 退出 1；故意引入 `needless_range_loop` → clippy 退出 101）确认工具真实生效而非静默跳过。仍无 `rustup`，故多工具链与 MSRV 1.85 兼容性未验证。见 [006：移除 Windows 支持](006-drop-windows-support.md)。 |
| TypeScript 完整类型检查、构建与应用测试 | 未安装声明的 `typescript@5.7.3` 依赖，也没有 lockfile | 审查依赖并 `npm install`、生成并提交 lockfile |
| Python 3.11 / 3.13 矩阵（本地） | 本机仅 3.12 | 由远端 CI 覆盖，或本地准备对应解释器 |
| 远端 CI 实际运行（`ci.yml` 与各 `project.yml`，含 Go race、Rust CI 安装步骤） | 本目录不是 Git 仓库，无远端与凭据；模板规则也不允许我擅自初始化或推送 | 由使用者建立仓库、推送并查看真实运行结果 |
| GitLab 流水线 | 模板未提供 `.gitlab-ci.yml` | 需自行移植 |
| 完整 Markdown lint、锚点、外链可用性、密钥扫描、依赖漏洞扫描 | 明确不在模板基础检查范围内 | 按团队政策另行接入 |

本表在 0.3.1 交付时还列有「Windows + MSVC presets、`python`/`py -3` 路径」一项；0.4 起 Windows 不再是目标平台，该项作废，不再作为待验证项跟踪，见 [006：移除 Windows 支持](006-drop-windows-support.md)。

表中「远端 CI 实际运行」一行的阻塞原因「本目录不是 Git 仓库」是本轮执行当时的事实；该目录其后已初始化为 Git 仓库并配有远端，但截至 0.4 仍无任何提交、未推送，因此远端 CI 依旧未运行。该项仍为待验证，只是阻塞原因已从「没有仓库」变为「没有提交与推送」。

## 本轮附带发现的仓库外风险

修改 `README.md` 时，本机的 pi-lens markdownlint 自动修复钩子把「快速开始」列表重新编号（`4.`→`1.` 等），因为原写法在被代码块和段落打断的第二段列表上从 4 开始编号。已将说明句缩进并入第 3 项，使列表成为连续的 1–7，既保留作者编号意图又对该规则稳定。这是本地格式化钩子行为，不是模板 `check.py` 的门禁（模板明确不包含完整 Markdown linter）。

## 交付说明

- 变更：新增本记录；同步 [设计索引](README.md)、[README 当前验证状态](../../README.md)、[模板维护](../template-maintenance.md)、[AI 启动验收记录](../ai-onboarding.md) 与 [Go](../languages/go.md)、[C++](../languages/cpp.md) 中被本轮证据覆盖的措辞。
- 未变更：`scripts/`、`profiles/`、`tests/` 与 `project.json`，母模板仍是未初始化 generic；未安装依赖、未联网抓取、未执行 Git 操作。
- 复现方式：在 `$TMPDIR` 复制模板并 `chmod -R u+w`，按上表命令执行；Go 需把 `/usr/local/go/bin` 加入 PATH。
- 剩余风险：见「未执行与阻塞原因」。C++ 的主要目标（Linux 双编译器四组合）在本机无法证明，本文不宣称其通过。
