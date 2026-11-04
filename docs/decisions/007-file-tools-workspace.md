# 007：文件编辑工具形态与软性工作区边界（edit_file 精确替换 + session 级 workspace 根）

- 状态：提议（2026-11-05，待维护者评审；AI 起草，无审批依据）。
- 日期与关联任务：2026-11-05；[009：mini-agent v3 技术方案](../design/009-coding-v3.md)。
- 决策人或审批依据：无——AI 起草，未获人工批复。

## 背景

v2 只有 `run_bash`：模型改代码要靠 shell 转义与 heredoc，token 成本高、易错，审批界面看不到"改了哪个文件哪几行"（用户 2026-11-05 明确要求"diff 显示修订"）。同时全仓库没有工作区概念：`run_bash` 裸继承进程目录，没有"这个 session 在改哪个项目"的边界。约束：零新增依赖、无沙箱（architecture 既有声明：审批即控制）、Linux 为主。

## 备选方案

- 编辑形态：
  - 方案 A（`edit_file` 精确 old→new 唯一匹配替换 + `write_file` 整文件写 + `read_file` 行号读）：唯一匹配天然 fail-closed（0 处或多处即报错回灌模型自纠）；审批块只含一个 hunk 级 diff，最小可审；实现是纯字符串操作 + 自写行级 LCS diff，百行级 DP 足够。代价：跨多点大改需要多次调用（模型可接受，主流编码 agent 同款形态）。
  - 方案 B（apply_patch 多文件补丁）：一次改多文件省 round-trip；但补丁格式解析器 + 行号漂移 + 审批块巨大的问题面成倍增长，且模型对补丁格式的马虎率高于精确匹配。
  - 方案 C（维持纯 shell 编辑）：零新代码；已被 v2 实战痛点否定（转义、审批不可读）。
- 边界强度：
  - 方案 D（软边界：`weakly_canonical` 前缀校验 + 符号链接解析后复查，越界即 is_error 回灌）：防误操作与提示注入的"无意越界"；实现 20 行，无特权要求。不防恶意（模型仍可用 `run_bash cat /etc/passwd`——该风险与 v2 同级，由 ADR 006 审批承担）。
  - 方案 E（强隔离：openat 逐级描述符、Landlock/namespace）：真防越界；引入内核特性依赖、run_bash 语义被破坏（子进程逃逸不可能全拦）、复杂度与项目"最小实现"取向冲突。
- 工作区归属：session 级属性（随 session 持久化，恢复即恢复边界）而非全局 config——与 ADR 001"一进程一会话"模型一致，多开不同仓库天然支持。

## 决策

建议 A + D + session 级 workspace：

1. 内置 `FileRuntime` 五工具（`read_file`/`write_file`/`edit_file`/`list_dir`/`search_text`），`edit_file` 默认唯一匹配（`replace_all` 显式开）；
2. `AppConfig`/session 增 `workspace`，CLI `--cwd` 与 TUI `/cwd` 设定；文件与 git 工具路径规范化后必须位于根内（双向：读也受限）；`run_bash` 子进程 `chdir` 至根；
3. `write_file`/`edit_file` 执行前计算 unified diff 进审批块，并在执行前重读目标文件做预览-执行一致性校验（不一致放弃本次执行）；
4. diff 自写（行级 LCS，纯函数入 `src/diff.cpp`），不引库、不借 `git diff --no-index`。

## 后果

- 收益：模型改码路径从"shell 杂技"变一等工具；审批所见即所写；多仓库多 session 边界清晰；老配置行为不变（workspace 空 = 进程 cwd）。
- 代价：新增约 500–700 行实现 + 测试面；session/config schema 各升一版（读兼容）。
- 风险：软边界给用户"已隔离"错觉——文档与 TUI 警示行明示"审批仍是唯一控制"（与 ADR 006 措辞一致）；LCS 对超大文件退化——行数上限 + 降级提示。
- 后续验证：009 第 8 节 `test_file_tools`/`test_unified_diff`/竞态用例；pty smoke 增 diff 审批场景。

## 关联
[009](../design/009-coding-v3.md) 第 5.1/5.3/5.4 节；[ADR 006](006-tool-approval-policy.md)（审批框架不变）；[ADR 008](008-git-tool-policy.md)（git 侧复用同一边界校验）。
