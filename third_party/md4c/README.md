# third_party/md4c — vendored md4c Markdown 解析器

按 [ADR 004](../../docs/decisions/004-md4c-parser.md) vendored，用于 TUI Markdown 渲染的解析层（本项目自写 SGR writer，不使用 md4c-html）。

- 项目：https://github.com/mity/md4c
- 来源 tag：v0.6.0（GitHub tags API 核对存在；archive tarball）
- 文件：`md4c.h`、`md4c.c`（src/ 目录下 CommonMark 合规解析器本体；单 .c 无其它依赖）
- md4c.c sha256：b9102e0ddfdfd494…（前 16）
- md4c.h sha256：d0c14762f1eac3cd…（前 16）
- 许可证：MIT（`LICENSE.md`，"Copyright © 2016-2026 Martin Mitáš"，同 tag 核对）
- 下载日期：2026-11-01
- 义务：MIT 分发保留版权与许可声明（本目录保留 LICENSE.md）。
- 不引入：md4c-html.c/entity.c（HTML 渲染器与实体表，本项目不需要）。
- 更新方式：换 tag 重新拷贝 src/md4c.[hc] 与 LICENSE.md，更新本文件。
