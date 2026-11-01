# third_party/nlohmann — vendored nlohmann/json

按 [ADR 002](../../docs/decisions/002-http-json-dependency.md) 要求 vendor 单头文件 JSON 库。本目录不是第一方源码，审查时单独对待。

- 项目：https://github.com/nlohmann/json
- 来源 tag：v3.12.0（GitHub Releases API 核对存在）
- 文件：`json.hpp`（release asset `json.hpp`，直接下载）
  - sha256：`aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`
  - 内部版本宏：NLOHMANN_JSON_VERSION 3.12.0（已核对一致）
- 许可证：MIT（`LICENSE.MIT`，取自同 tag 仓库根 `LICENSE.MIT`，Copyright (c) 2013-2025 Niels Lohmann）
- 下载日期：2026-11-01
- 义务：MIT 要求分发时保留版权与许可声明——随头文件同目录保留 `LICENSE.MIT` 即满足。
- 更新方式：换 tag 重新下载，更新本文件的 sha256 与日期，并在 git 提交信息中注明版本跳变。
