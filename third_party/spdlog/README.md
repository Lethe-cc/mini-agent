# third_party/spdlog — vendored spdlog（header-only）

按 [ADR 002](../../docs/decisions/002-http-json-dependency.md) 的 vendoring 模式引入，供独立日志文件功能使用（维护者 2026-11-01 指定 spdlog）。

- 项目：https://github.com/gabime/spdlog
- 来源 tag：v1.17.0（GitHub tags API 核对存在；tarball sha256 前缀 d8862955c6d74e58）
- 内容：`spdlog/` 头文件树（源自仓库 `include/spdlog/`，v1.17 起 single_include 与 include 合一布局；含 bundled fmt，header-only 模式，无外部 fmt 依赖）
- 许可证：MIT（`LICENSE`，"The MIT License (MIT), Copyright (c) 2016 - present, Gabi Melman and spdlog contributors"，同 tag 核对）
- 编译开关：使用方定义 `SPDLOG_HEADER_ONLY`；默认 `SPDLOG_ACTIVE_LEVEL` 提升到 info 级。
- 下载日期：2026-11-01
- 义务：MIT 分发保留版权与许可声明（本目录保留 LICENSE）。
- 更新方式：换 tag 重新拷贝 include/spdlog，更新本文件记录。
