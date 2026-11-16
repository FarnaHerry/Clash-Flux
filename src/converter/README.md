# 转换子项目

转换层按可复用、可独立维护的项目发展，当前与 Clash-Flux 同仓、同构建发布。
方向、职责边界及后续步骤以 [转换子项目规划](../../docs/converter-project.md) 为准；
支持字段和保真度只维护在 [保真度契约](../../docs/singbox-layers-and-fidelity.md) 与
[节点订阅](../../docs/node-subscriptions.md)，这里不复制能力矩阵。

[目标工作模型](../../docs/rule-document-plan.md)以 sing-box 规则和最小出口目录为基础。R3 通过共享目标快照提供编辑列表、兼容绑定和可复用的规则分支，完整候选继续在应用的实际上下文中生成。重启加载与磁盘产物按证据评估。

| 位置 | 当前职责 |
| --- | --- |
| `target/model.inc` / `target/capture.inc` | 显式捕获未叠加个人修改的主来源目标规则、默认动作、目录与出处；原生载荷私有、管理信息不进内核 JSON |
| `target/project.inc` | 从已转换载荷生成只读和编辑视图，重放旧个人修改，显示未应用的修改记录 |
| `target/edit.inc` | 核对唯一来源锚点，绑定快照内目标 ID 与旧格式的替换、禁用和默认出口操作 |
| `protocol/model.inc` | 拥有型节点、协议/TLS/传输及目标上下文；不保留原始 YAML/JSON 节点 |
| `protocol/clash.inc` | Clash 节点字段、别名及格式默认适配 |
| `protocol/validate.inc` | 协议/目标组合校验与结构化诊断 |
| `protocol/singbox.inc` | 已验证节点到 sing-box 出站生成 |
| `input/subscription.inc` | 列表有界解包、分流、名称和默认选择策略装配 |
| `input/catalog.inc` | 格式/scheme/协议登记及经审查的内核身份 |
| `input/uri.inc` | 公共 URI 语法、单次解码、SIP002 与类型识别 |
| `input/legacy_ss.inc` | 历史 SS 整串 Base64 的原文凭据适配 |
| `input/v2ray.inc` | VLESS/Trojan 的格式默认及传输/TLS 适配 |
| `input/secure.inc` | HY2/TUIC/AnyTLS 的认证、TLS 和协议参数适配 |

这些 `.inc` 仍由 [singbox.cpp](../singbox.cpp) 按原顺序在同一私有实现域编译；
[singbox.cppm](../singbox.cppm) 继续提供现有应用接口。目录归拢未形成独立库或稳定 SDK，
诊断类型、YAML 辅助函数与部分策略装配仍需后续收敛。

新增通用转换逻辑优先放在本目录，避免引入 UI、数据库、Runtime 或下载器依赖。
应用的来源选择、缓存提交和内核启停通过现有适配入口处理，不放入协议模型。
移动或新增片段时同步 `cmake/AndroidLegacy.cmake` 的依赖登记；验证和门禁沿用
[项目测试技能](../../project-skills/clashflux-test/SKILL.md)，不因目录归拢增加第二条生产转换路径。

应用缓存归 `src/target_source_cache.h` / `CoreStore` 与 `ProfilesModel`；转换子项目只提供目标模型和投影，不查询应用单例。当前拥有者、失效与闲置预算见[个人规则缓存契约](../../docs/subscription-personal-rules.md#规则页加载与缓存)。
