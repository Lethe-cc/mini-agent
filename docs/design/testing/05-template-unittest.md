# 05 · 模板维护 unittest 设计

载体：`tests/test_profiles.py`（32）、`tests/test_template.py`（20），共 52。
大纲见 [README](README.md)。验证**模板工具层**（scripts/profiles/workflows/
templates）规则，与业务代码无关；由 `check.py` 的 tooling 段运行。

## 1. 目标与边界

- 覆盖：profile 装配（cpp/python/ts/go/rust 骨架文件齐全）、初始化流程
  （`init_project.py` 写文件/占位符移除）、`project.json` 质量命令规则、
  文档占位符/链接审计、语言 profile 约束。
- 不验证 mini-agent 业务行为（业务验收以 `--strict-project` 的 quality_commands
  为准）；模板通过 ≠ 业务通过。

## 2. 机制

- 标准 `unittest`，`python -m unittest discover -s tests -p "test_*.py" -v`。
- 含**预期负例**：如 `test_rust_missing_lockfile_fails_before_running_cargo`
  断言"缺 Cargo.lock 必须先失败"，其输出里的 `FAIL: …` 是被测脚本的**预期**
   stderr，不代表 unittest 失败；判绿以 unittest 退出码为准。
- 部分用例真实调用 profile 工具链（go/rust/cargo 等），工具链缺失时按
  "实际工具链限制"记录跳过/失败原因（见设计 002–005 验证记录）。

## 3. 运行

```bash
python -m unittest discover -s tests -p "test_*.py" -v   # 单独
python scripts/check.py --tooling-only                  # 仅模板+文档
```

## 4. 扩展约定

新增 profile/模板规则 → 补 unittest（含负例）；保持与业务测试分离，不进
`quality_commands`；同步 [case-inventory](case-inventory.md) 计数。