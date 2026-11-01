# agent-benchmarks：评测基准与环境

## 来源与版本
- 类型：第三方仓库 / 论文附带的评测定义。
- URL 或本地路径：
  - SWE-bench：https://github.com/SWE-bench/SWE-bench
  - AgentBench：https://github.com/thunlp/AgentBench
  - WebArena：https://github.com/web-arena-x/webarena
- 固定 commit SHA / tag / 产品版本：未固定。基准数据集有版本号（如 SWE-bench Verified 子集），引用时注明具体 split 与发布日期。
- 最近核对日期与阅读状态：**未读**。仅登记来源。
- 许可证及核实依据：均未核实；注意数据集中 issue 内容来自真实开源仓库，再分发义务复杂，**不复制任何数据集内容进本仓库**。

## 参考目的与范围
- 借鉴什么：
  - SWE-bench：任务→环境→验证的契约设计（如何用一个可自动执行的判据界定"完成"），可映射到本模板 `scripts/check.py` 与质量门禁的思路。
  - AgentBench / WebArena：多环境评测维度划分，仅作参考。
- 重点文件或章节：待读取后填写精确路径（如 SWE-bench harness 的评测入口与镜像构建定义）。
- 不参考什么：各基准的模型打分结果（时效性差）；Docker 镜像构建细节（本模板不提供容器化执行器）。
- 何时阅读：设计 mini_agent 的验收标准、任务拆分模板、"验证"环节的可执行判据时。
- 在任务中的角色：设计参考。本模板不做模型评测，不承诺跑通任何基准。

## 访问方式
在线读取。如需精读 harness 代码，clone 到本地 `.references/` 并记录 commit SHA；不执行第三方基准脚本，不拉取数据集镜像。

## 本地差异
本模板的验证是"仓库内静态检查 + 项目声明的质量命令"，不模拟外部任务环境，不做成功率统计。借鉴点仅限于"完成判据必须可自动执行"这一原则。

## 实际采用情况
尚未核对，无采用结论。
