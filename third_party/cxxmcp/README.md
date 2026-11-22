# cxxmcp (vendored client-only subset)

Vendored source subset of [caomengxuan666/cxxmcp](https://github.com/caomengxuan666/cxxmcp)
for the mini-agent MCP client migration (ADR 010, design 021).

- Upstream commit: `3c25a8afef0ae77031bcb7a3142bcca67d4cd3d6` (master snapshot,
  fetched as a codeload tarball on 2026-11-21; git clone is blocked in this
  environment)
- Upstream `VERSION`: 1.3.0
- License: MIT (see [LICENSE](LICENSE), Copyright (c) 2025 caomengxuan666)

## What is vendored

Only the client dependency closure, computed from the six compiled sources
(`sdk/client/src/{client,http_transport,process_stdio_transport,stdio_transport,transport_adapter}.cpp`
plus `sdk/protocol/src/serialization.cpp`) via a transitive `#include` walk:

- 42 closure files (6 `.cpp` + 36 headers) under `sdk/`, exactly as upstream
- Convenience headers `sdk/include/cxxmcp/client.hpp` (server-free umbrella),
  `sdk/client/include/cxxmcp/client/transport_adapter_fwd.hpp` and
  `sdk/transport/include/cxxmcp/transport/stdio_transport.hpp` to keep those
  directories whole; all of their includes are inside the closure
- Build support required at configure time by the unmodified upstream root
  `CMakeLists.txt`: `VERSION`, `cmake/cxxmcpConfig.cmake.in`,
  `cmake/cxxmcpInstallExports.cmake.in`
- Bundled header-only dependencies from upstream `third_party/`:
  - `tl/expected.hpp` — tl::expected, CC0 (notice in header)
  - `nlohmann/json.hpp` — nlohmann/json v3.12.0 (upstream-patched copy; MIT),
    kept self-contained. NOTE: the project root `third_party/nlohmann/` also
    carries v3.12.0 (unpatched). Both are v3.12.0 and layout-compatible; each
    target's include ordering keeps its TUs on one copy (SDK targets only see
    this directory; agent_core sees the project copy first).
  - `httplib/` — cpp-httplib v0.46.0, commit
    `008e107d0fcddee7cb96dc5ad3c3189fd090e40a`, MIT (Copyright (c) 2017 yhirose);
    provenance notes in [httplib/README.cxxmcp.md](third_party/httplib/README.cxxmcp.md)

Not vendored: server/auth/websocket sources and headers, tests, examples,
benchmarks, docs, packaging. This trims the design 021 §5.2 list (which
mentioned handler/peer/service); those targets require `mcp_server`, which we
do not build (recorded as a §9 deviation). The upstream `httplib/README.md`
(user-facing httplib documentation) is also omitted: it links sibling docs we
do not vendor, which the repository Markdown gate rejects. License and
provenance files are kept.

## Local modifications

None. Every file is byte-identical to the upstream snapshot. The root
`CMakeLists.txt` is included unmodified; the mini-agent root CMake forces the
client-only option set (`CXXMCP_BUILD_CLIENT=ON`, server/SDK/tests/examples
OFF, `CXXMCP_ENABLE_HTTP=ON`, `CXXMCP_ENABLE_OPENSSL=ON`, auth OFF,
`CXXMCP_AUTH_CRYPTO=NONE`, `CXXMCP_USE_SYSTEM_DEPS=OFF`,
`CXXMCP_SDK_CXX_STANDARD=17`) and adds the subdirectory with
`EXCLUDE_FROM_ALL`. The upstream install/export rules are inert because this
subset is never installed (only the `mini-agent` executable is).

## Updating

Treat updates as explicit dependency bumps: re-fetch the pinned commit,
recompute the closure, re-run the M1 gates (design 021 §8), and update the
SHA/version above. Do not edit vendored files in place.