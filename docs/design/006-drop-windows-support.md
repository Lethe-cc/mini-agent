# 006：移除 Windows 支持，聚焦 Linux 与 macOS

## 文档状态

- 状态：已实现。
- 适用版本：模板 0.4.0，`schema_version` 保持 1。
- 核对日期：2026-09-14（CST）。
- 关联任务：将模板目标平台收敛为 Linux 与 macOS，删除 Windows/MSVC 相关配置、命令示例与 CI 矩阵。
- 代码修订：当前本地文件；仓库已初始化并配有远端，但尚无提交。
- 人工评审：无评审通过记录。
- 本文更新 [001](001-template-core.md)、[002](002-language-profiles.md)、[003](003-go-rust-profiles.md)、[004](004-linux-cpp.md) 中描述平台范围与 C++ preset 的段落，并在 [005](005-macos-verification.md) 中标注其阻塞项的最新状态；不改写这些文档中「当时实际执行了什么」的验证记录。

## 1. 背景与目标

0.1 至 0.3.1 期间，模板同时保留 Windows/MSVC 路径：C++ 有 `windows-msvc` / `windows-msvc-release` presets 与 `if(MSVC)` 告警分支，通用 `ci.yml` 与 Go/Python/Rust/TypeScript 的 `project.yml` 矩阵都含 Windows runner，文档大量使用 `py -3` 与 PowerShell 代码块。

问题是这条路径从未被真实验证过，却一直以待验证项的形式存在：

- [005](005-macos-verification.md) 明确列出「Windows + MSVC presets、`python`/`py -3` 路径」因无 Windows 主机而未执行。
- [002](002-language-profiles.md) 中 C++ 默认构建在配置阶段就因找不到 Visual Studio 2022 失败。
- 使用者已明确主要环境是 Linux + GCC/Clang，开发机为 macOS。

保留一条既不验证、也不使用的平台路径，成本是持续的：每份文档要维护两套命令示例，每个 CI 矩阵要为不会运行的 job 付时间与配额，而 `cmake --list-presets` 里出现 `windows-msvc` 还会诱导使用者误以为工具链已就绪——[002](002-language-profiles.md) 已记录过「把 generator 出现在 `--list-presets` 中当成工具链已安装」这类误判。

目标：删除 Windows 作为目标平台的全部配置与文档表述，使平台声明与实际验证范围一致；同时保留历史验证记录的证据完整性。

## 2. 范围与非目标

范围内：

- **C++ profile**：删除 `windows-msvc` 的 configure/build/test presets、`CMakeLists.txt` 的 `if(MSVC)` 分支、`cpp_check.py` 的 Windows preset 与 `os.name == "nt"` 默认选择、`.clang-tidy` 的 Windows 反斜杠路径分支。
- **CI**：通用 `ci.yml` 与 Go/Python/Rust/TypeScript 生成的 `project.yml` 矩阵移除 Windows runner。
- **文档**：`py -3` → `python3`，`powershell` 代码块 → `sh`，`.venv\Scripts\python.exe` → `.venv/bin/python`，`D:\Projects\...` → `~/projects/...`，删除 WSL、Developer PowerShell、VS 2022 Build Tools、Windows SDK、MSVC target 链接、`.exe` 后缀、Windows 商店占位程序等表述。
- **规则**：`AGENTS.md` 删除「不以 Windows 结果代替 Linux 结果」（不再有 Windows 结果可对照）。
- **版本**：0.3.1 → 0.4.0。

非目标：

- **不改写历史验证记录。** [001](001-template-core.md)/[002](002-language-profiles.md)/[004](004-linux-cpp.md)/[005](005-macos-verification.md) 中「当时在哪台机器上跑了什么、结果如何」的表格保持原样；删改会让「0.2 时 C++ 构建为何失败」这类问题失去答案。对其中描述*当前配置*的段落，就地标注 0.4 变更并链接本文。
- **不新增 macOS CI job。** 见第 4 节取舍 3。
- 不改变 Linux C++ 四组合（GCC/Clang × Debug/Release）的目标地位，也不声称其已通过真实编译。
- 不迁移已初始化的下游项目；迁移步骤见第 7 节。
- 不联网安装依赖，因此 TypeScript 完整门禁仍未验证。

## 3. 约束与参考资料

- 模板工具仍只用 Python 3.11+ 标准库，不新增依赖。
- 沿用 [002](002-language-profiles.md) 确立的 create-only profile 机制与 [004](004-linux-cpp.md) 的 Linux preset 结构；本文只删减平台分支，不重构生成流程。
- CMake presets 的 `condition` 语义、clang-tidy 的 `HeaderFilterRegex` 参考 [cpp-tooling](../references/cpp-tooling.md) 登记的 CMake 与 Clang 文档；本次未在线核对该来源。
- 约束：`profiles/` 是被生成内容，删除文件会影响 manifest，必须同步 `profile.json` 与回归测试。

## 4. 方案与关键取舍

**取舍 1：彻底删除，而不是保留为「可选路径」。**
0.3.1 的做法是「Windows 不在默认 CI，但配置仍保留，团队有需求时可追加」。实践中这产生了最坏的组合：使用者看到 preset 存在会尝试使用，而模板无法提供任何验证证据或排查经验。删除后，需要 Windows 的团队可以显式 fork 并自行承担验证，边界比「保留但不验证」清楚。

**取舍 2：非 Linux 平台的 C++ 默认 preset 回退到 `dev`。**
`default_preset()` 原逻辑为 `linux → linux-gcc-debug`、`nt → windows-msvc`、其他 `→ dev`。删除 Windows 分支后收敛为 `linux → linux-gcc-debug`、其他 `→ dev`。macOS 因此走 `dev`（Ninja + 当前默认编译器），与 [005](005-macos-verification.md) 已实测通过的路径一致，不需要为 macOS 新造 preset。

**取舍 3：不新增 `macos-latest` CI job。**
「聚焦 Linux 和 mac」可以理解为 CI 也应覆盖 mac。本次没有这么做，原因：

- GitHub Actions 的 macOS runner 计费倍率显著高于 Linux，默认开启会改变所有下游项目的 CI 成本，属于使用者应自行决定的政策。
- C++ 的 `linux-*` presets 有 Linux host condition，在 macOS 上只有 `dev`/`release` 可用，加 macOS job 并不能扩大 C++ 编译器覆盖。
- Python/TypeScript/Go/Rust 的工具链与平台无关，macOS job 的边际收益主要是路径分隔符与 shell 差异。

需要时在 `ci.yml` 与相应 `project.yml` 的 `matrix.os` 加入 `macos-latest` 即可，无需改代码。作为后续工作记录在第 9 节。

**取舍 4：历史文档加指针而非改写。**
备选是直接编辑 001–005 删除所有 Windows 字样。放弃该方案的理由见第 2 节非目标。改为在描述当前配置的段落就地标注「0.4 起已变更，见 006」。

**取舍 5：版本号取 0.4.0 而非 0.3.2。**
`windows-msvc` / `windows-msvc-release` 是对外可见的 preset 契约，删除后既有下游项目的 `CPP_PRESET=windows-msvc` 调用会被 argparse `choices` 直接拒绝（退出 2）。这是破坏性变更，按 [模板维护](../template-maintenance.md) 的迁移约定需要显式版本号与迁移说明。

**取舍 6：`.gitignore` 保留 Windows/VS 条目。**
`.vs/`、`*.user`、`*.suo`、`Thumbs.db` 予以保留并加注释说明。理由：忽略规则是防御性措施，防止在 Windows 上工作的贡献者误提交本地编辑器或操作系统元数据；这与「是否支持 Windows 作为构建目标」是两个独立问题。删除它们不会减少任何 Windows 支持，只会让误提交变得可能。

## 5. 详细设计

### C++ profile

`profiles/cpp/files/CMakePresets.json.in`：configure/build/test 三段各从 8 项减为 6 项，删除 `windows-msvc`（唯一使用 `Visual Studio 17 2022` generator 的 preset）及其 build/test 派生项。剩余 preset 全部使用 Ninja。

`profiles/cpp/files/CMakeLists.txt.in`：`starter_options()` 的 `if(MSVC) ... elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")` 收敛为单一 `if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")`，删除 `/W4 /permissive-` 与 `/WX`。`STARTER_SANITIZERS` 的 Linux 断言未变。

`profiles/cpp/files/cpp_check.py.in`：`PRESETS` 从 8 项减为 6 项；`default_preset()` 删除 `os.name == "nt"` 分支；删除 `--tidy` 与 `windows-msvc` 组合的显式拒绝分支，以及 `windows-msvc-release → windows-msvc` 的 configure preset 名映射（该映射只有 VS 多配置 generator 才需要，Ninja preset 名与 configure 名一一对应），configure 命令直接使用 `args.preset`。

`profiles/cpp/files/clang-tidy.in`：`HeaderFilterRegex` 由 `'.*[/\\]include[/\\]starter[/\\].*'` 简化为 `'.*/include/starter/.*'`，去掉 Windows 反斜杠分支。已逐用例比对新旧正则，对所有 POSIX 路径行为完全一致（裸相对路径 `include/starter/x.hpp` 两者都不匹配，属既有行为，非本次回归）。

### CI

`.github/workflows/ci.yml`：`matrix.os` 由 `[ubuntu-latest, windows-latest]` 改为 `[ubuntu-latest]`。
`profiles/{go,python,rust,typescript}/files/project.yml.in`：同样移除 Windows runner（Rust 原为 `windows-2022`）。C++ 的 `project.yml` 本就是 `ubuntu-24.04` 单平台，未改。

### 文档与规则

命令示例统一为 `sh` + `python3`；虚拟环境路径统一为 `.venv/bin/python`；示例目录改为 `~/projects/csv-report`。[user-guide.md](../user-guide.md) 常见问题表中「`python` 打开商店」一行替换为「`python3` 未找到」。`docs/languages/cpp.md` 中 `.sln` 改为 Makefile/Bazel/Meson，「STL 跨 DLL 边界」改为共享库边界并补充 Linux 上真实存在的 libstdc++/libc++ 混用与 `_GLIBCXX_USE_CXX11_ABI` 风险。

### 测试

新增 4 项回归测试（`tests/test_profiles.py`）：

| 测试 | 锁定的行为 |
| --- | --- |
| `test_non_linux_default_preset_is_dev` | darwin/freebsd 下 `default_preset()` 返回 `dev`；`PRESETS` 恰为 6 项且无 `windows-msvc` |
| `test_cpp_profile_has_no_windows_or_msvc_paths` | 生成的 presets 无 windows/msvc 名与 `Visual Studio` generator；`CMakeLists.txt` 与 `cpp_check.py` 中不出现 `msvc`/`windows`/`/W4` |
| `test_generated_ci_targets_linux_only` | 5 种语言生成的 `project.yml` 均不含 `windows` 且含 `ubuntu` |
| `test_template_ci_targets_linux_only` | 模板自身 `ci.yml` 不含 `windows` 且含 `ubuntu-latest` |

同时把 `generated_driver()` 中重复的 `spec_from_file_location` / `module_from_spec` / `exec_module` 三步提取为 `load_generated()`，并对 `spec`/`spec.loader` 为 `None` 的情况显式失败——原先该 `None` 会以 `AttributeError` 形式暴露，掩盖真实原因。

`new_config()` 不再硬编码 `template_version`，改为从 `project.json` 读取，避免每次版本升级都要同步测试常量。

## 6. 异常与安全设计

- 删除 `if(MSVC)` 分支后，非 GCC/Clang 编译器（Intel、IBM XL 等）不再获得告警选项。这等同于 0.3.1 在其他编译器下的行为（原 `elseif` 也不覆盖它们），不是新增缺口；模板仍不声称为未知编译器配置告警。
- 删除 `--tidy` 的 VS generator 拒绝分支后该保护不再需要，因为剩余 preset 全部使用 Ninja。**若将来重新引入非 Ninja generator，必须同时恢复该保护**，否则 `CMAKE_CXX_CLANG_TIDY` 在 VS generator 下不生效却会被误报为已分析（[002](002-language-profiles.md) 记录的原始风险）。
- `CPP_PRESET=windows-msvc` 现在由 argparse `choices` 明确拒绝并退出 2，不会静默回退到 `dev` 后给出误导性通过。
- `confined_path()` 对 `C:/absolute`、`bad\path` 的拒绝**予以保留**：这是路径穿越安全防御，不是 Windows 平台支持。已实测四种非法路径均被拒绝。
- CI 矩阵缩减不涉及权限、凭据或供应链变化；`permissions: contents: read` 与 `persist-credentials: false` 未改。

## 7. 兼容性与发布

**破坏性变更。** 从 0.3.1 及更早版本迁移的已初始化项目：

1. 建立备份或独立分支，不重跑 `init_project.py`（已初始化会被拒绝，且不应覆盖业务代码）。
2. `CMakePresets.json`：删除 `windows-msvc` 的 configure/build/test 条目。
3. `CMakeLists.txt`：删除 `if(MSVC)` 分支；如需保留 MSVC 支持，改为团队自行维护的分支。
4. `scripts/cpp_check.py`：从 `PRESETS` 删除两个 Windows 名，删除 `os.name == "nt"`、`--tidy` 的 VS 拒绝分支与 `windows-msvc-release` 的 configure 名映射。
5. `.clang-tidy`：可选地把 `HeaderFilterRegex` 简化为 `'.*/include/starter/.*'`（行为等价）。
6. `.github/workflows/*.yml`：从 `matrix.os` 删除 Windows runner。
7. 若此前依赖 `CPP_PRESET=windows-msvc`，改用 `dev`/`release` 并自行准备 Ninja 与编译器。
8. 更新 `project.json` 的 `template_version`，并在项目自己的设计文档中记录本次平台范围收敛。

回滚依靠版本控制或备份；模板不自动创建提交。发布前按 [模板维护](../template-maintenance.md) 在临时副本中试初始化。

## 8. 验证情况

全部真实构建均在 **macOS arm64** 的 `$TMPDIR` 临时副本中完成（Python 3.12.3、CMake 4.4.3、Ninja 1.13.2、clang 23.1.1、clang-tidy 23.1.1、Go 1.27.1、cargo/rustc 1.98.1、rustfmt 1.9.0、clippy 0.1.98、Node v24.21.0）。母模板保持未初始化 generic；未安装依赖、未联网抓取、未执行 Git 写操作。

| 验收项 | 证据位置 | 实际执行命令 | 结果 | 限制 |
| --- | --- | --- | --- | --- |
| 模板配置与文档基线 | `scripts/common.py` | `python3 scripts/check.py --tooling-only` | 通过，退出 0 | 不含完整 Markdown linter、锚点与外链可用性 |
| 工具回归测试（含 4 项新增） | `tests/test_profiles.py`、`tests/test_template.py` | 同上 | 通过，52 个测试 | 不等于任何语言的真实构建通过 |
| C++ presets 结构与数量 | `test_cpp_profile_has_no_windows_or_msvc_paths` | 同上 | 通过：三段各 6 项，无 windows/msvc 名与 VS generator | JSON 结构校验 |
| C++ presets 被 CMake 本身接受 | 临时副本 | `cmake --list-presets` / `--list-presets=all` | 通过：configure 段 macOS 仅见 `dev`/`release`（`linux-*` 因 host condition 隐藏），build/test 段列 6 项 | — |
| C++ 真实编译与 CTest | 临时副本 | `python3 scripts/cpp_check.py`、`--preset release` | 通过：两者均完成 6 个编译/链接步骤，CTest 各 1/1 Passed | macOS + clang，非 Linux GCC |
| C++ 告警标志实际生效 | `build/dev/compile_commands.json` | `-DSTARTER_WARNINGS_AS_ERRORS=ON` 后读取编译命令 | 通过：`-Wall`/`-Wextra`/`-Wpedantic`/`-Werror` 均在，无 MSVC `/W4` | — |
| C++ 已删 preset 的拒绝行为 | 临时副本 | `CPP_PRESET=windows-msvc python3 scripts/cpp_check.py` | 按设计退出 2 并列出 6 个合法 preset；无静默回退 | — |
| C++ Sanitizers 非 Linux 拒绝 | 临时副本 | `cmake --preset dev -DSTARTER_SANITIZERS=ON` | 按设计 FATAL_ERROR：requires Linux GCC/Clang | Linux 正路径仍未验证 |
| C++ clang-tidy **正路径** | 临时副本 | `cpp_check.py --preset dev --tidy` | 通过：`CLANG_TIDY_EXECUTABLE` 写入 cache，verbose 日志中 clang-tidy 被调用 3 次 | Linux 下未验证 |
| clang-tidy 反证（防「退出 0 = 没跑」） | 临时副本 | 向 `src/project.cpp` 加入 `const char* p = NULL;` | 通过：真实报出 `warning: use nullptr [modernize-use-nullptr]`；但构建仍退出 0（`WarningsAsErrors` 为空） | 印证默认不把诊断升级为失败 |
| C++ 格式检查 | 临时副本 | `python3 scripts/cpp_format.py` | 通过，退出 0，未改写文件 | — |
| C++ 严格项目检查端到端 | 临时副本 | `python3 scripts/check.py --strict-project` | 通过（工具测试 + 真实 C++ 构建与 CTest） | — |
| **Rust 完整四步门禁** | 临时副本 | `python3 scripts/rust_check.py` 与 `--release` | 通过，退出 0：fmt → clippy `-D warnings` → test → build 四步真实执行；单元 1/1、CLI 集成 1/1、文档测试 1/1 | 无 `rustup`，多工具链与 MSRV 未验证 |
| Rust fmt/Clippy 反证 | 临时副本 | 追加 `pub fn    badly_formatted(  )`；追加 `for i in 0..v.len()` 索引循环 | 通过：`cargo fmt --check` 输出 diff 退出 1；clippy 报 `needless_range_loop` 与 `items-after-test-module` 退出 101；还原后重新退出 0 | — |
| Go 真实检查与 race | 临时副本 | `python3 scripts/go_check.py`、`--race`、`go run ./cmd/starter` | 通过：gofmt/vet/test/build 与 `--race` 均退出 0；起步程序输出项目名 | Linux race 未在本地验证 |
| Python 起步应用真实测试 | 临时副本 | `python3 -m unittest discover -s app_tests`、`python3 -m app` | 通过：Ran 2 tests OK；输出项目名 | — |
| TypeScript 缺依赖失败路径 | 临时副本 | `node scripts/ts_check.mjs` | 按设计退出 1（`MODULE_NOT_FOUND`），不回退到全局编译器 | 完整门禁未执行 |
| 5 种 profile 初始化与 CI 矩阵 | 临时副本 | `init_project.py --language {cpp,python,go,rust,typescript}` | 均退出 0；4 个非 C++ profile 生成的 `project.yml` 矩阵均为 `os: [ubuntu-latest]` | — |
| 版本号一致性 | `project.json` | `load_config(ROOT)['template_version']` | `0.4.0` | — |
| 路径穿越防御未被削弱 | `scripts/profiles.py` | 对 `C:/absolute`、`bad\path`、`../escape`、`/absolute` 调用 `confined_path` | 四种均被拒绝 | — |
| Linux GCC/Clang 四组合 | — | 未执行 | 未执行 | 本机非 Linux，无 docker/podman/lima/colima/multipass/vagrant，无真 GCC（`/usr/bin/g++` 是 Apple clang 21 别名） |
| TypeScript 完整类型检查/构建/测试 | — | 未执行 | 未执行 | 未安装 `typescript@5.7.3`；按模板规则不联网安装依赖 |
| Python 3.11/3.13 本地矩阵 | — | 未执行 | 未执行 | 本机仅 3.12.3 |
| 远端 CI 实际运行 | — | 未执行 | 未执行 | 仓库已配有远端 `origin`，但尚无提交、未推送 |

## 9. 实现偏差与已知限制

- **macOS 未进入 CI 矩阵**，只有本地实测记录。若团队要求 macOS 持续验证，需显式添加 `macos-latest` 并接受相应 runner 成本；这是政策决定，不应由模板默认开启。
- **Linux GCC/Clang 四组合仍未真实编译验证**，本文没有改变该状态，也不因删除 Windows 而让 Linux 结果变得「相对更好」。本轮所有真实构建都是 macOS/arm64 + clang 数据，**不得当作 Linux GCC 证据**。
- **clang-tidy 默认不作为门禁**：实测确认 `WarningsAsErrors` 为空时诊断不会导致失败，构建仍退出 0。团队若要把 tidy 当严格门禁，必须自行配置 `WarningsAsErrors`，否则会把「分析跑过但没拦下问题」误读为「静态分析通过」。
- `dev`/`release` preset 无 host condition，在任意平台可见；macOS 上其编译器是 AppleClang/Homebrew clang。
- Rust 仍无 `rustup`，因此多工具链切换与 MSRV 1.85 兼容性未验证；`--offline` 只在无外部依赖时成立。
- 历史文档中的 Windows 记录被有意保留。读者可能因此误以为模板仍支持 Windows——已通过在相关段落就地标注 0.4 变更并链接本文降低风险，但无法完全消除。
- 本轮曾一度因 PATH 检测方式有误，把 `rustfmt`/`clippy` 判为「未安装」；其后在 `/opt/homebrew/bin` 下发现二者可用并补做了完整验证与反证。记录此点以免读者把中途结论当成最终状态。同理，`cargo`/`clang-tidy` 相对 [005](005-macos-verification.md) 也是「从无到有」——**工具链可用性会随时间变化，未验证项应定期重新探测，而不是沿用旧结论**。
- 未处理 GitLab 流水线（模板始终未提供 `.gitlab-ci.yml`）。
- 后续工作：如确定需要 macOS CI，应同时补充 macOS 上 `dev`/`release` preset 的真实构建步骤，而不是只加一个 runner 标签。

## 最终核对

- [x] 接口、配置、路径与当前代码一致（presets 三段各 6 项、`PRESETS` 6 项、CI 仅 ubuntu、`template_version` 0.4.0、`HeaderFilterRegex` 已简化）。
- [x] 已实现与规划内容清楚区分（macOS CI 明确标为非目标与后续工作）。
- [x] 外部来源、版本和实际采用内容可追溯（CMake/Clang 文档仅登记未在线核对，已注明）。
- [x] 验证结果有证据，未执行事项如实标明（Linux 四组合、TypeScript 完整门禁、Python 3.11/3.13、远端 CI 均标为未执行并写明阻塞原因）。
- [x] 索引与架构总览按需更新（[设计索引](README.md)、[模板维护](../template-maintenance.md)、[README](../../README.md)）。
