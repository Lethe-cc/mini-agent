# 002：LLM 传输依赖 libcurl，JSON 使用 nlohmann/json

- 状态：已接受（2026-11-01，见决策人栏）。
- 日期与关联任务：2026-10-31；[007：mini-agent 第一版技术方案](../design/007-agent-v1.md)。
- 决策人或审批依据：维护者 2026-11-01 指示"开始第一步的开发"，据其批准按本 ADR 建议方案实施；未单独逐条批复。

## 背景

Agent 内核必须通过 HTTPS 调用 LLM API 并解析 JSON（请求体、响应体、SSE 分片）。模板约束"不无故新增依赖；新增第三方库前必须核实许可证、署名及分发义务并记录 ADR"，同时冷启动预算要求避免重量级运行时。

## 备选方案

- 方案 A：libcurl（系统包，curl license）+ nlohmann/json（MIT，单头文件）。成熟、覆盖 TLS/代理/超时；引入两个外部依赖点；SSE 需在 curl 写回调上自行分行。
- 方案 B：纯 POSIX socket + OpenSSL 自实现 HTTP/1.1 chunked + 手写 JSON 子集解析。依赖面最小、启动路径最短；但正确性与安全（TLS 校验、边界情况）成本高，v1 时间预算内风险不成比例。
- 方案 C：cpp-httplib（MIT）+ 手写或第三方 JSON。比 A 少一个依赖，但 TLS 后端选择与流式回调模型较弱。

## 决策

建议方案 A：CMake `find_package(CURL)` 硬依赖系统 libcurl；nlohmann/json 以 vendoring 单头文件方式置于 `third_party/nlohmann/`，落地时**必须**先核实其 LICENSE 文本并记录来源 release tag 与 commit SHA，未核实前不得引入。若维护者倾向零第三方源码，降级讨论方案 C。

注：本 ADR 登记的是依赖策略；OpenAI 兼容接口的协议细节属于"必须核对"事实，见 [references/agent-protocols.md](../references/agent-protocols.md)，实现前对照选定服务方当天文档。

## 后果

- 收益：安全传输与 JSON 正确性外包给成熟实现；代码集中在循环与渲染。
- 代价：运行期依赖系统 libcurl；`third_party/` 进入审查与许可证合规范围。
- 风险：头文件 vendoring 版本漂移——以固定 tag + SHA 记录缓解。
- 后续验证：四组合 CI（GCC/Clang × Debug/Release）确认无警告引入；`--help` 冷启动计时不含网络。

## 关联
[007](../design/007-agent-v1.md) 第 4、7 节；[references/agent-protocols.md](../references/agent-protocols.md)。
