# 节点订阅解析扩展计划

日期：2026-11-15。状态：**本轮 P6 范围已完成本机 Linux/Android 验证：七类 URI、官方样例证明的历史 SS 与能力清单一致性门禁已接入**。前置协议重构 P0–P5 已完成本机验证；其它历史分享格式未纳入本轮支持，Windows/macOS 与真机验收仍待补齐。当前接受范围见节点订阅边界。
当前已实现范围仍以[节点订阅边界](node-subscriptions.md)为准；表中尚未实施的格式不能视为已支持。
实施路线按[协议中间模型重构计划](protocol-refactor-plan.md)调整：先迁移已有协议到中立模型，再按本计划扩展 URI。下文描述格式适配器的研究，不再以临时 Clash 协议节点作为新增格式的接入方案。

后续格式扩展属于[同仓转换子项目](converter-project.md)，协议/输入实现已归拢到 [src/converter](../src/converter/README.md)。当前继续使用既有模块和构建；本计划维护格式扩展顺序，子项目规划维护职责与接口边界。

## 参考与结论

参考 [Aethersailor/SubConverter-Extended](https://github.com/Aethersailor/SubConverter-Extended/tree/144f7c6bf6f7dd3da3d280730914b77f9df54631)，研究快照固定为 `144f7c6bf6f7dd3da3d280730914b77f9df54631`。
重点借鉴多协议 URI、编码与历史分享格式兼容、能力清单生成及边界案例。协议规范和本项目固定 sing-box 能力仍是字段映射的依据。

- [协议清单](https://github.com/Aethersailor/SubConverter-Extended/blob/144f7c6bf6f7dd3da3d280730914b77f9df54631/src/parser/mihomo_schemes.h)：用于梳理输入格式，不能将 Mihomo 支持视为 sing-box 支持。
- [预处理](https://github.com/Aethersailor/SubConverter-Extended/blob/144f7c6bf6f7dd3da3d280730914b77f9df54631/bridge/preprocess.go)：参考 Base64 包装、SS userinfo、URL 转义及旧 Shadowrocket VMess；避免重复解码、凭据中 `+` 变空格和失败后使用部分解码结果。
- [桥接设计](https://github.com/Aethersailor/SubConverter-Extended/blob/144f7c6bf6f7dd3da3d280730914b77f9df54631/bridge/README.md)：其 Mihomo 桥仅用于 clash/clashr，其它目标（包括 singbox）走旧解析器；不能直接接入 singbox 输出并宣称获得新桥能力。
- [解析实现](https://github.com/Aethersailor/SubConverter-Extended/blob/144f7c6bf6f7dd3da3d280730914b77f9df54631/bridge/parser.go)：URI 订阅可跳过坏节点并按名称去重；本项目保持整份候选检查和明确拒绝，不沿用静默丢条目行为。

## 源码对照与接入位置

现有 `src/converter/input/subscription.inc` 已分离 `nodeSubscriptionText` 的明文/外层 Base64 识别与 `parseNodeSubscription` 的逐行转换，后者在 P2 已将 VMess 直接适配到 NodeSpec，并将默认选择策略装配分离。
P6 首批新增 `src/converter/input/uri.inc` 的拥有型 authority/query/fragment 拆分、单次百分号解码、UTF-8/控制字符/主机/端口检查及 SS SIP002 适配；SS 不经过临时 Clash 节点。后续格式继续保留有界输入、既有策略/目录和整份候选行为。协议重构和默认值/生成职责以[主计划](protocol-refactor-plan.md)为准：

P6 第二批新增 `src/converter/input/v2ray.inc` 的 VLESS/Trojan 适配，复用公共 URI 语法并直接构造协议/TLS/传输模型；独立处理 TLS 缺省、SNI/Host、完整密码和 Reality/Vision。字段边界只维护在[节点订阅](node-subscriptions.md#vless--trojan-uri-子集p6-第二批)，不从 Clash 或参考库套用默认值。

P6 第三批新增 `src/converter/input/secure.inc` 的 HY2/TUIC/AnyTLS 适配；公共语法显式选择可选认证/默认端口/HY2 端口集合，旧入口仍使用原检查。TUIC UDP 中继模式进入类型化 enum，Clash 白名单保持；QUIC 与 TCP TLS 分开，格式边界见[节点订阅](node-subscriptions.md)。

P6 收尾依据 [Shadowsocks 官方公开样例](https://shadowsocks.org/doc/configs.html)接入 `src/converter/input/legacy_ss.inc`：整串 Base64 内的密码保留原文，不按 URI 二次解码；与 SIP002 独立分流，不回退猜测。`src/converter/input/catalog.inc` 的协议/scheme/格式登记驱动入口、默认名与 provider 类型，拥有型只读 API 导出审查快照，由必跑门禁对照独立样例、账本及实际固定内核。旧 VMess 无新增必要样本，继续保持拒绝，不为了覆盖名称而添加宽松分支。

```text
原始订阅 → 有界包装识别 → 逐行 URI 拆分/字段适配 → NodeSpec → 协议/目标校验 → sing-box 生成与保真度 → 候选检查
```

| 参考代码 | 借鉴内容 | 本项目的落点与差异 |
| --- | --- | --- |
| `bridge/parser.go::decodeSubscriptionEnvelope` | 明文 URI 与外层 Base64 分开识别 | 延用 `nodeSubscriptionText`，不对整份 YAML/JSON 或 Base64 先做 URL 解码，不增加反复猜测/多层解码 |
| `src/parser/subparser.cpp::parseShareAuthority`、`parseShareUri` | 分离 userinfo、host、port、query、fragment；处理方括号 IPv6 | 提取拥有型 URI 解析结果，保留原始 query 条目和行号；严格检查百分号转义、端口、主机与控制字符，不照搬宽松分支 |
| `bridge/preprocess.go::normalizeSubscriptionUserinfo` | 区分 Base64 userinfo 与明文、避免重复解码 | 凭据按 URI 组件解码一次；`+` 保留，`%2B` 变 `+`，`%252B` 只变 `%2B`；不能用表单解码函数统一处理凭据 |
| `src/parser/subparser.cpp::explodeSS` | SIP002 与旧式整串 Base64、plugin 的分层处理 | 首批只开放有明确规范和测试的子集；SS 2022、插件转义与本项目插件白名单单独校验，不将 plugin 原样当作内核参数 |
| `parseXrayTransport`、`explodeStdVless`、`explodeTrojan` | 公共传输字段和协议专有字段分离 | 填充中立传输/TLS/协议结构，复用迁移后的校验与生成；Clash 的 `network`、`ws-opts` 等键仅属于 Clash 适配器，未知传输不改成 TCP |
| `bridge/parser.go::parseSubscriptionWithMihomo` | 每个节点独立解析、统一聚合 | 不沿用 `continue` 跳坏条目/重名的行为；保留整份失败、原文不改写，统一报告脱敏行号/字段原因 |
| `scripts/generate_proxy_validation.go` 与能力 manifest | 从固定版本生成后续校验清单 | 未来可做输入解析器、映射与测试的一致性检查；不能从 Mihomo 字段列表自动推导 sing-box 等价语义，首批不引入生成工具链 |

### 协议参数如何进入中立模型

- SS：method、原文密码、插件名与选项分别进入 Shadowsocks 专用结构；当前 `cipher` / `plugin-opts` 的支持子集与默认语义由协议重构迁移保留。IPv6 服务器去外层方括号后独立保存，不能用冒号拆整个链接。
- VLESS：身份、flow、encryption 和 UDP 编码进入协议结构；security、sni、fp、pbk、sid 分别进入中立 TLS/Reality 结构。传输 Host 与 TLS SNI 分开处理，缺省值按格式规范逐项决定，不继承 Clash 适配器默认。
- Trojan：URI userinfo 作为完整密码处理；TLS 默认、SNI 与 WS/gRPC 参数按协议规范转换，不能把密码中的冒号拆成另一套账号字段。
- HY2 / TUIC / AnyTLS：复用迁移后的协议映射，但 URI 的默认端口、认证、ALPN、混淆、跳端口等需独立验收；URI 能被拆开不代表所有字段已等价支持。

SS 必须区分 [SIP002](https://shadowsocks.org/doc/sip002.html) 的两种 userinfo 形式：Stream/AEAD 可使用 Base64URL，AEAD-2022 不得使用该包装，明文 method/password 按组件百分号编码；plugin 选项还有自身的分隔符转义规则。
VLESS 对照 [Xray 分享链接提案](https://github.com/XTLS/Xray-core/discussions/716) 及固定内核能力，HY2 对照[官方 URI 规范](https://v2.hysteria.network/docs/developers/URI-Scheme/)；兼容别名不能取代原规范。SS 历史整串 Base64 与非标准 VMess 单独列为兼容分支，不混入规范路径。

### 需要先定清的边界和回归

1. query 保留重复项再校验：同一键重复、别名冲突、未知参数与格式错误分别给出原因，不能先塞进单值 map 丢掉冲突。明确允许的显示元数据与影响连接的参数分开记账；尚未评估的未知字段保持拒绝策略。
2. 为凭据/名称/path 分别验证 `+`、`%2B`、`%252F`、`%40`、`%3A`、非法 `%`、嵌入 NUL、Unicode 和空白；不能 trim 密码，也不能把解码后的分隔符再当 URI 结构拆一次。
3. TLS 参数组合覆盖 TLS 关闭仍携带证书选项、验证开关别名冲突、Reality 依赖缺失、Host 与 SNI 不同；未经显式配置不关闭证书验证。
4. SS 覆盖普通与 2022 userinfo、旧包装、插件转义、未知插件；VLESS/Trojan 覆盖 WS/gRPC、默认参数、未知传输与未知加密模式。
5. `src/singbox_compile.inc::compileClashDocument` 已对节点列表中的任一转换失败拒绝部分结果，扩展时保留该分支；全部节点成功才装配选择组，保持名称、顺序、行号与原始输入一致。
6. `tests/test_singbox.cpp::testNodeSubscriptions` 已有包装、TLS、混合坏条目、重名、上限、对象目录和规则投影案例。新增协议沿同一生产入口验证，并比较等价手写 Clash 输入的编译结果和保真度；仅比较两个解析器输出一致不足以证明协议语义正确。

实际开工顺序服从协议重构 P0–P5；现有 VMess URI 在该重构中迁移而不扩展格式。完成后再进入 URI 扩展阶段，先做公共语法与单次解码，单独交付 SS，随后 VLESS/Trojan；HY2/TUIC/AnyTLS 与历史格式延后分批。

## 实施顺序与完成范围

| 阶段 | 范围 | 完成条件 |
| --- | --- | --- |
| 前置 | 完成协议重构 P0–P5 | 已有协议和现有 VMess URI 进入中立模型，移除旧协议生成路径，等价与平台回归有证据 |
| 1a（首批已接入） | 公共 URI 语法、SS SIP002、明文及 Base64 SS/VMess 混合列表 | SS 2022/plain/Base64URL 边界、密码原文、IPv6、query/插件转义及默认、provider 类型筛选、只读目录与 Prepare 失败保护有证据；见[P6 首批](evidence/2026-11-15-protocol-p6-ss.md) |
| 1b（已接入） | VLESS、Trojan URI | 已定义严格字段子集、凭据/默认/TLS/Reality/传输和混合入口；固定内核配置检查、真实本地转发与 Prepare 失败保护见[P6 第二批](evidence/2026-11-15-protocol-p6-vt.md) |
| 2（已接入） | Hysteria2/hy2、TUIC、AnyTLS URI | 已核对认证、端口/集合、TLS/ALPN/验证、QUIC/uTLS 与协议参数；模型/混合入口、固定内核检查与真实转发见[P6 第三批](evidence/2026-11-15-protocol-p6-secure.md) |
| 3（本轮已完成） | 官方样例证明的历史 SS 整串 Base64；输入能力清单一致性检查 | 历史格式有独立案例/识别诊断和普通/provider 实际转发；清单与入口/原生字段/账本/固定版本及 revision 对照，漂移拒绝；见[P6 收尾](evidence/2026-11-15-protocol-p6-final.md) |

## 实施时必须保持

- 继续使用本地共享编译器，协议输入经中立模型进入 sing-box 生成，集成既有规则投影、对象目录和保真度账本；不新增公共转换服务依赖，不把订阅凭据发送到第三方。
- 保留下载/导入原文；未知或坏条目、重复名称和不支持语义明确失败。默认 TLS 校验保持，近似映射按 exact/approx/unsupported 契约记账。
- 沿用大小/数量限制、脱敏行号诊断、独占暂存、候选检查与原子提交；失败保留旧订阅、引用及活动配置。
- 输入格式扩展与扫码/直接粘贴节点入口分开评估，不因增加解析支持自动扩大 UI 入口或手机多来源能力。
- 引入源码或依赖前核对许可证、工具链、跨平台体积和维护成本；当前不引入 Go/CGO/Mihomo 解析桥。

## 后续验证范围

实施时添加合法/非法混合、编码变体、重复字段、未知参数、资源上限和秘密不入诊断的生产回归；覆盖导入、刷新、规则投影、对象目录及失败留旧。
每项映射通过固定内核检查；实际连通性另行验收。按项目测试技能执行构建、版本检查、相关回归和 `git diff --check`，完成后更新支持边界与验收证据。
