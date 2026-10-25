---
name: clashflux-cpp
description: 修改 Clash-Flux C++ 命名模块接口、inline、跨模块性能或 JSON codec 边界时使用。
---

# C++ 模块与 codec

## 模块接口及性能

先读 [C++ 模块开发](../../docs/cpp-modules-development.md) 的接口与性能验证章节；其中历史构建参数仅是注明日期的证据，当前配置须重新检查。

- 命名模块中的普通类内函数不隐式 inline，导出类体包含的 .inc 同理。小访问器/判断/转发若要让导入方展开，显式 inline 且定义保留在接口定义域；不能只改声明、把定义藏在 .cpp。
- 普通头文件/全局模块片段的类内函数，以及 constexpr、consteval、首次声明即 = default（包括比较）仍隐式 inline；DTO 不需要补构造，不把模块规则推广到所有头文件或模板。
- inline 不保证展开，也不允许多个命名模块单元重复定义；不批量给 I/O、解析、JSON、锁或大对象复制加 inline，不改 composable 为 inline，不用 always_inline 或有 ABI 影响的 -fmodule-implicit-inline 取巧。
- 先核对真实 Debug/Release 优化和 LTO，再比较独立导入真实模块的消费单元汇编。调用消失和端到端提速分开记录；复制、分配、锁和系统调用独立评估，不用 Debug 推断 Release。

## JSON codec

改 codec 前读 [Glaze 迁移](../../docs/glaze-migration.md)。运行时 JSON/内部 policy 使用 wire_codec.h/.cpp；固定版本由依赖锁维护（当前 Glaze 9.0.0）。
头文件只提供拥有型普通 DTO/Result；Glaze 头、metadata、模板只在独立 clashflux_wire 普通 TU，不能导出到 modules/UI codegen 或并入 Android Legacy 超大 TU。
页面消费共享 typed 模型；连接解码/投影进任务线程，首页总量在 WebSocket 线程解码，UI 只读整数，持久键用显式 metadata 固定。
运行时 API 可投影未知字段；订阅/原生配置不得照搬宽松策略。保留 yaml-cpp、原生 DOM、专用协议解析和局部规则文本编辑。

## OS 资源所有权

本进程资源用既有包装：service.cppm 的 UniqueFd、win32_raii.h 的 UniqueHandle/UniqueHkey、api.cpp 的 CurlHandle/CurlHeaderList，不在多个返回分支手写释放。
只有需要检查释放返回值，或资源属于其它进程/进程级单例时手动管理，并注释理由；跨进程服务资源按协议管理。

完成后按 [测试技能](../clashflux-test/SKILL.md) 构建与检查；改映射还需 [编译器技能](../clashflux-compiler/SKILL.md)。
