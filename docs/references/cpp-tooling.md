# cpp-tooling：C++、CMake 与分析工具参考

本清单登记来源，不代表本次实现中已经在线读取。使用前按实际工具版本核对相关章节，并记录阅读状态。默认只核对事实和借鉴设计，不复制第三方代码。

| 来源 | 入口与版本 | 适用范围 |
| --- | --- | --- |
| CMake 官方手册 | <https://cmake.org/cmake/help/v3.23/> | 最低 CMake 版本的 targets、presets、CTest 与编译器配置 |
| C++ Core Guidelines | <https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines> | 所有权、生命周期、资源和接口设计；动态页面，使用时记录核对日期或固定仓库修订 |
| cppreference | <https://en.cppreference.com/w/> | 查询标准库 API 和引入版本；非 ISO 标准原文，关键兼容性需结合目标实现核对 |
| Clang 文档 | <https://clang.llvm.org/docs/> | clang-format、clang-tidy、sanitizers；入口随版本演进，按已安装版本选择说明 |

不适用：将最新标准特性自动带入 C++17 项目；将通用指南覆盖公司的 ABI、异常、实时性或平台约束。

特别注意：不能因 CMake 配置项存在就宣称分析工具实际运行。以构建日志、工具版本和实际测试结果为准。
