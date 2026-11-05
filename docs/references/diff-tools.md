# diff-tools：开源 diff 工具与库

## 来源与版本
- 类型：第三方仓库 / 系统工具。
- URL 或本地路径：
  - git（`git diff --no-index` 借用法）：https://github.com/git/git ；本机实测版本 2.47.3
  - delta：https://github.com/dandavison/delta （Rust，32k★ 量级）
  - difftastic：https://github.com/Wilfred/difftastic （Rust，语法感知 structural diff）
  - libXDiff（git xdiff 的独立母库，Davide Libenzi）：经 git 仓库 `xdiff/` 目录实读文件头
  - similar：https://github.com/dandavison/similar （Rust 库，delta 底层引擎）
  - diff-match-patch：https://github.com/google/diff-match-patch （已归档）
  - kdiff3 / Meld / colordiff：GUI/终端着色类，仅登记不推荐进依赖
- 固定 commit SHA / tag / 产品版本：未固定（清单类登记）；引用具体实现前按当天仓库核对。
- 最近核对日期与阅读状态：**2026-11-05 元数据+许可证层核对**（GitHub API 元数据、delta LICENSE 文件实读、git xdiffi.h 头部实读、`git diff --no-index` 本机行为实测）；各仓库源码均未精读。
- 许可证及核实依据：
  - delta = **MIT**（`raw.githubusercontent.com/dandavison/delta/main/LICENSE` 当日实读，Copyright 2020 Dan Davison）。
  - difftastic = **MIT**（GitHub API license 字段当日核实）。
  - git 本体 = GPLv2 only（仓库 COPYING 当日实读：明示"仅此一版 GPL"）；其内嵌 **xdiff 子库 = LGPL**（`xdiff/xdiffi.h` 头部实读为 LibXDiff LGPL 措辞）——若考虑 vendor xdiff 需按 LGPL 静态链接义务评估并记 ADR，未评估前不得复制。
  - diff-match-patch = **Apache-2.0**（GitHub API 当日核实），但 **archived=True**（当日核实），字符级 diff 面向同步场景，非 unified 输出。
  - similar：**当日未核实成功**（GitLab raw 被 Cloudflare 挑战页拦截、GitHub raw LICENSE 404；作者同 delta 可旁证 MIT，但按规则引用前必须补核实际 LICENSE）。
  - kdiff3/Meld = GPL 系（未逐库实读，仅登记）；colordiff = GPL（未实读）。

## 参考目的与范围
- 借鉴什么：
  - `git diff --no-index` 的**借用法**：对任意两文件（含非仓库目录）产标准 unified diff，零新增依赖（git 已是 GitRuntime 运行时依赖）——审批"看全 diff"外置渲染的候选路径。
  - delta/difftastic 的**呈现层设计**：side-by-side、语法感知折叠、颜色主题约定——TUI diff 渲染演进参照（不复制代码）。
  - libXDiff 的**算法分层**（xprepare/xemit/xhistogram/xpatience）：若未来需要 Myers/histogram 优于朴素 LCS 时的结构参考。
- 重点文件或章节：git `xdiff/` 目录结构；delta README 的 pager 集成章节；difftastic 的 structural diff 说明。均为候选起点，未读。
- 不参考什么：各库的 CLI 胶水层、GUI、git 完整历史操作面；diff-match-patch 的字符级语义（与 unified 审批需求不符）。
- 何时阅读：审批 diff 渲染增强（`d` 键外置 pager）、diff.cpp 算法升级（LCS→histogram）、或考虑引入任何 diff 依赖时——引入前必须走 ADR（AGENTS.md 依赖规则）。
- 在任务中的角色：设计参考 + 许可证事实核对。任何"许可证为 X"的表述以当天实读 LICENSE 文件为准。

## 访问方式
在线读取 GitHub API/文件；git 行为以本机实测（2.47.3）。similar 的 LICENSE 经 GitLab/GitHub 两路当日不可达，标记待补核。

## 本地差异
本项目 diff 为自写 `src/diff.cpp`（行级 LCS unified diff，~250 行，零依赖，13 项单测 + OOM 单元守卫，见 [009 §5.3/§9 D7](../design/009-coding-v3.md)）。外部工具一律不链接；经 `run_bash` 调用属既有审批面，不构成依赖。

## 实际采用情况
2026-11-05 维护者问"有无开源 diff 工具/库可参考或直接用"，结论（当日核实支撑）：
- 采用现状：**自写 diff.cpp 维持**——审批预览只需行级 unified diff，LCS 足够；libXDiff 的 LGPL 静态链接义务与零依赖取向相比收益不抵合规成本。
- 可用未用：`git diff --no-index`（本机实测可用）列为后续"外置 pager 渲染全 diff"（delta/difftastic，装了即用、未装回退纯文本）的候选实现路径，未立项。
- 排除：diff-match-patch（归档+语义不符）；similar（许可证未核实，引用前补核）。
