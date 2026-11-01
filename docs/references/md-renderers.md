# md-renderers：终端 Markdown 渲染库

## 来源与版本
- 类型：第三方仓库。
- URL 或本地路径：
  - md4c：https://github.com/mity/md4c（C，CommonMark 合规解析器）
  - cmark：https://github.com/commonmark/cmark（C，CommonMark 参考实现，AST）
  - glamour：https://github.com/charmbracelet/glamour（Go，终端 markdown 样式渲染器）
  - glow：https://github.com/charmbracelet/glow（Go，glamour 上层应用）
  - termimad：https://github.com/dragosv/termimad（Rust，终端 markdown 表格）
  - rich：https://github.com/Textualize/rich（Python，终端样式/能力探测）
- 固定 commit SHA / tag / 产品版本：未固定；引用前按任务当天核对。
- 最近核对日期与阅读状态：**许可证层已核对（2026-11-01，GitHub license API/raw 文件）**；源码均未读取。
- 许可证及核实依据：md4c = MIT（API 核对）；glamour = MIT；glow = MIT；rich = MIT；cmark = API 返回 NOASSERTION（实为其自带 Custom/ISC 型文本，**引入前必须读 LICENSE 原文**）；termimad = 预期路径无 LICENSE 文件，**未核实**。复制代码前逐仓库核实。

## 参考目的与范围
- 借鉴什么：
  - md4c：若替换自研 `md::render` 子集（缺表格/嵌套列表/任务清单时），首选解析层——单头文件 + 一个 .c、零依赖、回调式 renderer；本项目自写 SGR writer，`display_width/wrap/clip` 仍用自有终端几何（md4c 不负责）。
  - glamour：**样式架构**——语义 token → ANSI 样式表是数据不是代码；支持无色彩降级风格（notty），对应本项目"非 TTY 原样输出"的理论形态。
  - cmark：仅当未来需要 AST（如 /compact 结构分析）才优于 md4c。
  - glow/termimad：窄终端表格排版取舍的参考。
  - rich：终端能力探测（TERM=dumb、NO_COLOR）做法。
- 重点文件或章节：待读取后填写精确路径。
- 不参考什么：Go/Rust/Python 的实现代码（跨语言只取模式）；glamour 的 chroma 语法高亮栈（重依赖）。
- 何时阅读：`md` 模块扩展表格/复杂语法，或评估切换 md4c 时。
- 在任务中的角色：设计参考 + 依赖候选。切换 md4c 属新增 vendored 依赖，**必须先记 ADR**（模式同 [ADR 002](../decisions/002-http-json-dependency.md)）。

## 访问方式
在线读取；如需精读源码 clone 到本地 `.references/`（只读、记录 commit SHA），不放入第一方源码。无法访问时说明影响。

## 本地差异
本项目 v1 现状是**自研零依赖 `md` 子集渲染器**（[src/md.cpp](../../src/md.cpp)，已通过 pty 虚拟屏验证），与这些库的差异：行级解析、无标准合规承诺、样式硬编码。切换决策点已记录：表格等语法实际碍眼时再引 md4c，`md::render` 为唯一替换点，TUI 不动。

## 实际采用情况
2026-11-01（同日更新）：维护者采纳切换建议——**md4c v0.6.0 已 vendored**（third_party/md4c/，md4c.h/md4c.c/LICENSE.md + 记录），`md::render` 重写为 MD_PARSER 回调式 SGR writer（ADR 004）；md4c-html/entity 未引入。glamour 样式架构（token→样式表数据化、notty 降级）仍为未来改进参考。cmark/termimad/rich 未采用。
