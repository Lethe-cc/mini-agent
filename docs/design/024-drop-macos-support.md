# 024：移除 macOS 支持，仅支持 Linux

## 文档状态

- 状态：已实现（待人工评审）。
- 适用版本：模板 0.5.0（由 0.4.0 升级，破坏性变更），`schema_version` 保持 1。
- 核对日期：2026-11-24（CST）。
- 关联任务：维护者 2026-11-24 指示「将 macos 的支持从项目中移除，只支持 linux」；dev 已合并回 main，本变更在 dev 分支（与 main 同点）继续。
- 代码修订：基线 `6ccb357`（main/dev HEAD）；设计草案提交 `e6b21f9`；实现提交见 dev 分支本轮落地提交。
- 人工评审：无评审通过记录。
- 本文沿用 [006：移除 Windows 支持](006-drop-windows-support.md) 的处理方式：删除平台配置路径与当前平台声明；[005](005-macos-verification.md) 等历史验证记录**不改写**，只就地标注「已移出支持范围」并回链本文。

## 1. 背景与目标

006 之后模板目标平台为「Linux 与 macOS」，但 macOS 路径的实际状态是：

- 业务侧 [project-context.md](../project-context.md) 的非目标自 007 起就写明「不支持 Windows 与 macOS」；mini-agent 业务验证（007–023）全部在 Linux 完成，CI 只有 `ubuntu-24.04`。
- macOS 证据停留在 2026-09-14 的模板骨架验证（005/006），从未覆盖业务代码；本机是 Linux 共享开发机，无 macOS 主机可持续验证。
- 保留这条不再验证、不再使用的路径的成本：`dev`/`release` 两个无 host condition 的通用 preset 与 `cpp_check.py` 的非 Linux 回退让「支持面」看起来比实际大，文档需持续维护 macOS 措辞与 AppleClang 免责声明。

目标：平台声明、构建配置、门禁脚本、文档与测试全部收敛为 **仅 Linux**，且拒绝路径明确（非 Linux 报错而非静默回退）；历史验证记录保持证据完整。

## 2. 范围与非目标

范围内：

- `CMakePresets.json` 与 `profiles/cpp/files/CMakePresets.json.in`：删除 `dev`/`release` configure/build/test presets（6 → 4）；`linux-gcc-debug`、`linux-clang-debug` 改为自包含（generator、Debug、BUILD_TESTING、Linux condition），不再 inherits。
- `scripts/cpp_check.py` 与 `profiles/cpp/files/cpp_check.py.in`：`PRESETS` 收敛为四个 Linux preset；删除 `default_preset()` 的非 Linux 回退，非 Linux 平台在 main() 入口明确报错退出 1。
- `CMakeLists.txt` 与 `profiles/cpp/files/CMakeLists.txt.in`：configure 阶段增加非 Linux `FATAL_ERROR`（防御直接 `cmake -S . -B build` 绕过 preset condition 的路径）。
- `tests/test_profiles.py`：更新既有锁定（pipeline 用例改用 Linux preset、preset 名单 6 → 4、`dev` 断言移除），将 `test_non_linux_default_preset_is_dev` 改为锁定「非 Linux 拒绝」；新增根仓库配置与 profile 源一致的回归锁定。
- 活文档：README、docs/development.md、docs/template-maintenance.md（新增 0.5 节）、docs/languages/{README,cpp,go,rust,typescript}.md、docs/ai-onboarding.md、docs/agent-handbook.md（如需）、设计索引。
- 历史文档标注：005 文档状态、001/002/003/004/006/008/023 中描述*当前平台范围*的句子就地标注 0.5 变更并回链本文；验证记录表格原样保留。
- `project.json` `template_version` 0.4.0 → 0.5.0（preset 契约破坏性变更，沿用 006 取舍）。

非目标：

- 不改写 005/006 的验证记录本身（当时执行了什么就保留什么）。
- 不删除源码中的 POSIX 通用实现——`src/`、`include/`、`tests/cpp/` 无任何 `__APPLE__`/`__MACH__`/darwin 条件编译（已全量 grep 核实），无需代码改动；POSIX 调用在 Linux 上即为所需行为。
- 不动 CI：`ci.yml` 与 `project.yml` 自 006 起就只有 ubuntu runner，本轮零改动（已核实）。
- 不清理 `.gitignore` 的 `.DS_Store` 等防御性条目（沿用 006 对 Windows 条目的处理）。
- 不引入跨平台抽象层、不新增依赖、不做 feature/cxxmcp 合并（维护者明确延后）。

## 3. 约束与参考资料

- 项目约束：AGENTS.md（小步修改、破坏性操作先确认——本轮平台收敛由维护者明确指示）；ADR 008（commit 可自动、push 留人工）。
- 先例：[006](006-drop-windows-support.md) 全文——删除路径 + 历史标注 + 版本升级 + 回归测试 + 迁移节的完整模板。
- 外部 API 事实：CMake presets v3（CMake ≥3.23）`condition`/`inherits` 语义沿用现有用法；`linux-gcc-debug` 改为自包含后语义与原先经 `dev` 继承完全等价（generator Ninja、Debug、BUILD_TESTING ON）。
- 无新增外部来源。

## 4. 方案与关键取舍

1. **删除 `dev`/`release` 而不是给它们加 Linux condition**。加 condition 只是把 macOS 路径藏起来，名单里仍留着与四个显式 preset 语义重叠的「当前默认编译器」入口，且 `cpp_check.py` 回退逻辑依旧存在。006 删 Windows presets 的同款理由适用：不验证、不使用的入口只会诱导误判。业务与 CI 从未引用 `dev`/`release`（全仓 grep 核实：仅模板文档示例与历史验证记录引用）。
2. **非 Linux 双闸**：`cpp_check.py` 入口报错（明确信息 + 退出 1，不落入 argparse choices 的静默困惑）；CMake configure `FATAL_ERROR`（防绕过 preset 的直接 configure）。两层信息一致：「only Linux is supported (see docs/design/024)」。
3. **历史文档标注而非改写**（006 §2 非目标同款）：005 是 macOS 真实证据，保留原文；在文档状态区加一行「0.5 起 macOS 已移出支持范围，本文仅作历史记录，见 024」。README 的 2026-09-14 macOS 验证叙述同样保留，段首加标注。
4. **版本升级 0.4.0 → 0.5.0**：`CPP_PRESET=dev`/`release` 与 `--preset dev` 从可用变为拒绝（退出 2/1），属破坏性契约变更，沿用 006 的 minor bump 惯例。
5. **`default_preset()` 保留但仅返回 `linux-gcc-debug`**：平台判断上移到 main() 入口；函数保留使 `test_linux_default_and_clang_override` 等既有测试语义不变。备选「直接删函数、内联常量」会破坏既有测试接口，无收益。
6. **文档中的 AppleClang「实测」句改为历史标注**：如 development.md 依赖表「实测 GCC 14、AppleClang 21」改为「实测 GCC 14（AppleClang 21 为 0.4 前历史记录，0.5 起仅支持 Linux）」。

## 5. 详细设计

### 5.1 CMakePresets（根 + profile .in 同步）

- configurePresets 仅剩 4 项；`linux-gcc-debug`/`linux-clang-debug` 直接携带 `generator: Ninja`、`CMAKE_BUILD_TYPE: Debug`、`BUILD_TESTING: ON` 与 Linux condition；Release 两项仍 inherits 对应 Debug。
- buildPresets/testPresets 同步删除 `dev`/`release` 两项。
- 既有测试 `test_cpp_presets_refer_to_real_configure_presets`、`test_linux_compiler_presets_are_explicit_and_isolated` 无需改动即应继续通过（后者已断言 condition/inherits/编译器）。

### 5.2 cpp_check.py（根 + profile .in 同步）

```python
PRESETS = ("linux-gcc-debug", "linux-gcc-release",
           "linux-clang-debug", "linux-clang-release")

def default_preset():
    return "linux-gcc-debug"

def main():
    if not sys.platform.startswith("linux"):
        print("ERROR: only Linux is supported (see docs/design/024-drop-macos-support.md)", file=sys.stderr)
        return 1
    ...
```

- 根版本保留 `--refresh-models` 业务旗标；profile 版本无此旗标（既有差异，保持）。
- 平台守卫在 argparse 之前，非 Linux 上无论传什么 preset 都得到同一条明确错误。

### 5.3 CMakeLists（根 + profile .in 同步）

`project()` 之后立即：

```cmake
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "mini-agent supports Linux only; see docs/design/024-drop-macos-support.md")
endif()
```

（profile 版措辞用通用项目名。）既有 `NOT WIN32` install-prefix 守卫保留——Linux-only 下恒真，但删除属无关清理，留给后续。

### 5.4 测试（tests/test_profiles.py）

- `test_cpp_pipeline_stops_after_configure_failure`：`--preset dev` → `--preset linux-gcc-debug`（mock subprocess，平台无关）。
- `test_non_linux_default_preset_is_dev` → `test_non_linux_platform_is_rejected`：patch `sys.platform` 为 darwin/freebsd14，main() 返回 1 且 subprocess 未被调用；PRESETS 锁定为四个 Linux 名单。
- `test_cpp_profile_has_no_windows_or_msvc_paths`：名单 6 → 4，同时断言无 `dev`/`release`/`apple`/`darwin` 字样（更名为 `test_cpp_profile_has_no_non_linux_paths`）。
- 新增 `test_repository_cpp_config_is_linux_only`：直接读根仓库 `CMakePresets.json`/`scripts/cpp_check.py`/`CMakeLists.txt`，断言与 profile 源同契约（四 preset、无 dev/release、含 Linux 守卫），防止根配置与模板源漂移。
- `test_linux_default_and_clang_override`：保持（default_preset 语义未变）。

### 5.5 文档

- README：L5 工具支持句、L89 平台声明、L100 preset 说明改为仅 Linux；「模板工具层验证」节冠以 0.5 说明，2026-09-14 macOS 两段保留原文并标注历史。
- development.md 依赖表：AppleClang 句改历史标注。
- template-maintenance.md：版本句改 0.5.0；新增「0.5 移除 macOS 支持，仅支持 Linux」节（要点 + 迁移 + 本轮真实验证证据）。
- languages/README.md L25、cpp.md（§1 macOS bullet、§3 命令表 dev/release 行与实测状态段、§4 tidy 实测段尾句）、go.md/rust.md/typescript.md 实测状态段：保留事实、加「0.5 起 macOS 已移出支持范围」标注，删除把 macOS 当作当前可用路径的指引句。
- ai-onboarding.md L31：pi 实测记录保留，标注当时环境为 macOS（历史记录）。
- project-context.md：非目标句已是「不支持 Windows 与 macOS」，与现实一致，无需改。
- 设计索引：登记 024 行；尾段补 0.5 说明（沿用 0.4 段落格式）。
- 005 文档状态、001 L100、002 §验证尾注、003 尾注、004 尾注、006 文档状态、008 §非目标、023 §8 限制列与最终核对：各加一句 0.5 标注/修订，原文记录不动。

## 6. 异常与安全设计

- 非 Linux 运行 `cpp_check.py`：明确错误 + 退出 1，无任何子进程调用（测试锁定）。
- 非 Linux 直接 cmake configure：FATAL_ERROR，信息指向本文。
- `CPP_PRESET=dev`（旧值）：argparse choices 拒绝，退出 2（与 006 对 windows-msvc 的行为一致）。
- 无密钥、无网络、无权限变化；本轮不触碰运行时代码。

## 7. 兼容性与发布

- 破坏性：`dev`/`release` preset 与 `CPP_PRESET=dev|release` 不再可用。迁移：改用 `linux-gcc-debug`（原 dev 等价物）/`linux-gcc-release`（原 release 在 Linux 上的对应物）；旧 `build/dev`、`build/release` 目录可自行删除。
- macOS 主机：不再支持在本仓库跑构建门禁；如需继续开发请使用 Linux 主机/容器/VM。
- 回滚：还原本轮提交即恢复 0.4 行为；无数据迁移。
- `template_version` 0.5.0；已初始化项目不自动继承（模板既有约定）。

## 8. 验证情况

| 验收项 | 测试位置或证据 | 实际执行命令 | 结果 | 限制 |
| --- | --- | --- | --- | --- |
| 模板回归（含新锁定） | tests/test_profiles.py 等（53 项，含 5 项 Linux-only 锁定） | `python3 -m unittest discover -s tests -q` | 通过（53/53，OK） | 本机 Debian Linux（GCC 14.2 / Clang 19.1，Python 3.13.5） |
| 统一入口 | scripts/check.py | `python3 scripts/check.py --strict-project`；`python3 scripts/check.py --tooling-only` | 均通过（strict 含默认 preset configure+build+ctest 3/3；tooling-only 链接检查通过） | tooling-only 明确跳过项目命令，不替代业务验收 |
| 四 preset 门禁 | cpp_check.py | `python3 scripts/cpp_check.py --preset linux-gcc-debug`；`--preset linux-gcc-release`；`--preset linux-clang-release` | 全部通过（各 ctest 3/3）；`cmake --list-presets` 恰列出 4 个 linux preset；根 CMakePresets.json 与 profile .in 生成结果逐字节一致（脚本比对） | 同上 |
| tidy + ASan/UBSan | cpp_check.py --tidy --sanitizers | `python3 scripts/cpp_check.py --preset linux-clang-debug --tidy --sanitizers` | 通过（ctest 3/3，tidy 0 告警，ASan/UBSan 干净） | 同上 |
| pty 冒烟 | tests/pty_tui_smoke.py / pty_cli_smoke.py | `python3 tests/pty_tui_smoke.py ./build/linux-gcc-debug/mini-agent`；`python3 tests/pty_cli_smoke.py ./build/linux-gcc-debug/mini-agent` | TUI 209 条断言全 PASS；CLI ALL PASS | 同上 |
| 冷启动预算 | tools/cold_start_check.py | `python3 tools/cold_start_check.py ./build/linux-gcc-debug/mini-agent` | PASS（--version median 4.4ms；ask median 5.7ms；预算 100ms） | 共享机器测量，波动属正常 |
| 非 Linux 拒绝反证 | test_non_linux_platform_is_rejected（mock platform） | 随 53 项模板回归执行 | 通过（mock Darwin/Windows 下 cpp_check 生成脚本在 argparse 前 exit 1） | 无 macOS 真机，拒绝路径靠 mock 锁定；CMake FATAL_ERROR 正路径本机无法触发（Linux 恒过），以代码审查 + 既有 Sanitizers 同款守卫先例为据 |

## 9. 实现偏差与已知限制

- 无 macOS 真机：非 Linux 的 CMake FATAL_ERROR 与真实 AppleClang 行为未实测，靠 mock 平台测试与 configure 逻辑审查覆盖（如实标注，不声称已验证）。
- §5.5 原列的「008 §非目标」标注实际不需要：`docs/decisions/008-git-tool-policy.md`（ADR）不含任何平台范围声明，其「006」引用指向 ADR 006（工具审批），与设计 006 同号不同文；故本轮未改动该文件。
- 远端 CI 结果以推送后实际运行为准（push 留人工）。

## 最终核对

- [x] 接口、配置、路径与当前代码一致。
- [x] 已实现与规划内容清楚区分。
- [x] 外部来源、版本和实际采用内容可追溯。
- [x] 验证结果有证据，未执行事项如实标明。
- [x] 索引与架构总览按需更新。