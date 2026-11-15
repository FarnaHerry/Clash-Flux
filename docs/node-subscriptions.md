# 节点订阅：SS、VMess、VLESS、Trojan、HY2、TUIC 与 AnyTLS

远程订阅响应和本地文件按内容识别，支持逐行 `ss://` SIP002、`vmess://<Base64 JSON>`、`vless://`、`trojan://`、`hysteria2://` / `hy2://`、`tuic://` 和 `anytls://` 的下述子集，以及整份列表再包一层 Base64 的形式；七类节点可以混合。不通过 URL 后缀判断格式；订阅下载的 IP、自定义端口、路径和查询参数按原 URL 使用。
普通订阅地址输入仍用于 HTTP(S) 下载；本功能不新增扫码或直接粘贴任意节点 URI 的入口。

协议与格式适配器归入 [src/converter](../src/converter/README.md)，按[同仓转换子项目规划](converter-project.md)持续整理，当前使用既有模块和构建。本文维护输入支持边界，职责划分与后续接口收敛由该规划维护。

## 公共输入与边界

- 接受标准及 URL-safe Base64，有/无 padding、ASCII 空白、BOM、CRLF；拒绝非法字符、padding
  和非零尾位。原文与解码列表上限 8 MiB，最多 4096 节点，每条新式 URI 和解码后的 VMess JSON 各限 64 KiB。
- 名称在单份混合列表中唯一，不能与 DIRECT/REJECT/REJECT-DROP 重名；选择组避开节点名。坏条目、未知协议、重复项和不支持语义拒绝整份列表，不只保留合法前缀。

## SS SIP002 子集（P6 首批）

- 支持 `ss://userinfo@host:port[/][?plugin=...][#name]`，SS scheme 不区分大小写。Stream/AEAD 的 userinfo 可以是 Base64URL（有/无 padding），也可以是分别百分号编码的 `method:password`。SS 2022 必须使用后者，密钥本身的 Base64 内容原样保留，不能再包一层 Base64URL userinfo。
- 先按原始 URI 分隔组件，再各解码一次；`+` 保留，`%2B` 为 `+`，`%252F` 只变为 `%2F`。密码不裁空白，保留冒号、百分号和 Unicode；拒绝空密码、错误转义、无效 UTF-8 和 ASCII 控制字符。行两侧可有格式空白，URI 内部空白须编码。
- 服务器接受 ASCII 主机名/IPv4 和带方括号的 IPv6，必须显式给出 1–65535 十进制端口；IPv6 zone、Unicode 域名、非根路径尚未接入，域名可使用 punycode。fragment 单次解码并裁两侧显示空白；缺省/空名称生成 `Shadowsocks 序号`，序号按混合列表位置计算。
- method 限固定内核文档中的已支持方法；密钥有效性和实际运行能力由完整候选/固定内核检查确认。URI 不增加 TLS/Reality、UOT、拨号或路由参数；原生 SS 的 TCP/UDP 默认保持。
- SIP002 query 只开放 `plugin`，保留重复项后明确拒绝重复/未知参数；这比示例中忽略未知参数的行为更严格。SIP002 userinfo 不接受标准 Base64 的 `+/` 字母表；旧式整串包装走下述独立格式，不能混用语法。

| SIP003 插件 | 接受的 URI 选项与默认 |
| --- | --- |
| `obfs-local` | `obfs=http/tls`（缺省 http），可选非空 `obfs-host`；缺省 host 不输出该参数，不补 Clash 的 bing.com |
| `v2ray-plugin` | `mode=websocket`（缺省），可选非空 host、可选 path（显式空值保留）、`mux=0/1` 或裸 mux（缺省 1）、裸 tls（缺省关闭） |
| 限制 | TLS 仅接受裸标志；`tls=false` 会被原生解释为启用，故本子集明确拒绝。server、cert、HTTPUpgrade、QUIC、自定义头等其它选项拒绝；重复键/坏 SIP003 转义拒绝 |

plugin 先做 URI 解码，再做 SIP003 分隔符转义解析，最终由共同生成器编码一次；host/path 未提供与显式值分别建模。v2ray-plugin 缺省 host/path 由固定原生实现处理，不注入 Clash 默认值；TLS 继续默认验证证书。simple-obfs 的 tls 是伪装，SIP003 插件只承诺 TCP 伪装，不宣称给 UDP 增加同样包装。

依据：[SIP002](https://shadowsocks.org/doc/sip002.html)、[SIP003](https://shadowsocks.org/doc/sip003.html)、[固定 SS 方法](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/docs/configuration/outbound/shadowsocks.md)、[固定 v2ray-plugin 默认](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/transport/sip003/v2ray.go)。

## SS 历史整串 Base64（P6 收尾）

支持官方仍保留的 `ss://Base64(method:password@host:port)[#name]` 格式。以 authority 中是否有原始 @ 选择 SIP002 或历史分支，不在某条解析失败后猜测或回退。历史 payload 只接受标准 Base64 字母表（含 +/），可省略 padding；外部订阅列表仍可再包一层标准/URL-safe Base64。

解码内容不是 RFC3986 URI：先取首个冒号分开 method，再取最后一个 @ 分开服务器；密码中的 @、/、?、#、冒号、+、百分号、Unicode 和两侧空白都是原文字节，**不做百分号解码**，例如 %00/%2F 保持该字符串。服务器须为原始 ASCII 主机名/IPv4 或方括号 IPv6，以及显式 1–65535 端口；不接受编码主机、zone、路径、query 或重复 fragment。显示名按公共 fragment 规则解码一次，缺省 Shadowsocks 序号。

method/非空密码与现有 SS 子集共用校验，但旧格式不接受 SS 2022。外置 plugin/query、历史 payload 的 Base64URL 字母表、嵌套 ss:// 和其它猜测形式均拒绝；插件/2022 请使用 SIP002。每条 URI 64 KiB、整列表大小/数量边界保持。

无损包装转换为 exact，不增加节点近似；编译 warnings 记录一次脱敏的格式识别事件。provider 只对筛选后参与的历史节点记录该事件；混合坏条目仍整份拒绝、Prepare 失败留旧。该事件与原有默认选择策略的 Field/Approx、VMess net=tls 的 Node/Approx 分开。

依据及公开回归样例见 [Shadowsocks 配置格式](https://shadowsocks.org/doc/configs.html)。当前只接入有该公开样例依据的历史 SS 分支；旧 VMess authority/Shadowrocket/JSON v1 等没有纳入本轮支持，不通过猜测绕过原有拒绝。

## VMess JSON 既有子集

- VMess 分支只转换 JSON 分享格式，版本缺省/显式 2；其它尚未开放的协议、VMess URI authority 格式、
  旧版 JSON 1、未知字段或重复 JSON 字段明确拒绝，不只导入列表中的合法部分。
- 保留节点名称/顺序、服务器、端口、UUID、alterId、cipher（缺省 auto）；数字字段允许整数或
  十进制整数字符串。名称缺省生成 VMess 序号，重复名称及内置出口重名拒绝，避免节点消失或引用改向。
- 支持 TCP、WebSocket、HTTP/2（h2/http）、gRPC 的现有映射；保留 WS Host/path、HTTP host/path、
  gRPC serviceName（path）。gRPC authority、KCP/QUIC、TCP HTTP 伪装及其它模式不能等价表达时拒绝。
- TLS 必须显式 `tls=tls`；默认验证证书。仅显式 insecure/allowInsecure 才跳过验证，两者冲突拒绝。
  保留显式 SNI、ALPN 和指纹；无显式 SNI 时用首个 transport host，否则由内核按服务器处理。
  非空 vcn/pcs 证书约束尚未转换，拒绝候选，不忽略约束。
- 部分服务商使用非标准 `net=tls`；仅同时显式 `tls=tls`、无伪装且路径为空或 `/` 时，按 TCP+TLS
  兼容并记节点 approx。其它未知传输不降为 TCP。

## VLESS / Trojan URI 子集（P6 第二批）

支持 `vless://UUID@host:port[/][?query][#name]` 与 `trojan://password@host:port[/][?query][#name]`，scheme 不区分大小写，其它参数名和值区分大小写。共用 SS 的组件拆分、单次百分号解码、UTF-8/ASCII 控制字符、主机/IPv6/显式端口及 64 KiB 边界；query 最多 32 项，重复键（含编码后重名）、未知键和冲突别名整份拒绝，空 query 可省略。

VLESS 要求标准 36 字符 UUID；Trojan 的完整 userinfo 是密码，含冒号、+、百分号、Unicode 和两侧空白时原样保留，不拆账号、不 trim、不二次解码。未提供/空白 fragment 生成 `VLESS 序号` 或 `Trojan 序号`，统一名称唯一性按混合列表检查。authority 后仅可为空或根路径；实际传输路径使用 query 的 path。

| 参数 | 当前接受语义 |
| --- | --- |
| VLESS encryption / flow | encryption 缺省 none，只接受 none；不产生额外原生加密字段。flow 可缺省、显式空或 xtls-rprx-vision；Vision 仅 TLS/Reality + 原生 TCP |
| security | VLESS 缺省 none，接受 none/tls/reality；Trojan 缺省 tls 且必须为 tls。TLS 关闭时任何 TLS/Reality 参数（包括空值/false）均拒绝 |
| sni / fp | TLS 的 SNI 缺省远端 host，与传输 Host 独立；显式 sni 非空。本 URI 子集缺省 fp=chrome，显式值限固定内核支持集合；不继承外围 Clash 的 global-client-fingerprint，指纹近似按共用账本记录 |
| alpn | 可省略；显式值为非空逗号列表，各项非空、无两侧空白且最多 255 字节；不删除/trim 成员 |
| insecure / allowInsecure | 仅显式 true/false/1/0；缺省验证证书。两者同值可并用，冲突拒绝；例外只作用本节点 |
| Reality pbk / sid | 仅 VLESS + 原生 TCP，必须显式 fp/pbk；pbk 为无 padding 的 Base64URL，解码恰好 32 字节。sid 可省略/空或至多 8 字节偶数长度 hex。普通 TLS 不接受 pbk/sid；Reality 不接受普通 TLS insecure 例外，即使显式 false |
| type | 缺省 tcp；接受 tcp/raw、ws、grpc、http/h2、httpupgrade。raw 是 TCP 别名；其它传输拒绝，不降为 TCP |
| TCP headerType | 仅 none；TCP 不接受 host/path，拒绝 HTTP 头伪装 |
| WS / HTTPUpgrade | path 缺省 /，显式值须非空且以 / 开头；host 可省略，显式非空。Host 不代替 TLS SNI，不扩展自定义头或 early-data |
| HTTP/2 | http/h2 映射原生 HTTP 传输，必须 TLS，避免原生明文 HTTP/1.1 回落；显式 ALPN 须含 h2 且不能含 h3。path 同上；host 为非空逗号列表 |
| gRPC | serviceName 必须非空，原样保留；mode 缺省/显式 gun，authority 只接受省略或空。拒绝 multi/guna、非空 authority 和 host/path；其它传输也不接受 gRPC 参数 |

未列参数全部拒绝，包括 xhttp/KCP/QUIC、MLKEM encryption、ECH、证书 pin、Reality spx/pqv 和 URI UDP 编码扩展。VLESS 未提供 packet_encoding，保留固定原生 XUDP 默认；Trojan 保留原生 TCP/UDP 能力。不把 Trojan-Go scheme、Trojan Reality 或旧式 Base64 userinfo 当作已支持格式。

依据：[Xray 分享提案](https://github.com/XTLS/Xray-core/discussions/716)、[Trojan URL](https://github.com/trojan-gfw/trojan-url/blob/master/README.md)、[兼容客户端验证开关](https://github.com/2dust/v2rayN/blob/master/v2rayN/ServiceLib/Handler/Fmt/TrojanFmt.cs)。接受范围是本项目明确的固定内核子集，不承诺提案或其它客户端的全部参数。原生语义依据固定 [VLESS](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/protocol/vless/outbound.go)、[TLS/Reality](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/common/tls/reality_client.go) 和 [V2Ray 传输](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/docs/configuration/shared/v2ray-transport.md)。

## HY2 / TUIC / AnyTLS URI 子集（P6 第三批）

三种格式直接构造同一协议模型，保持组件单次百分号解码、+ 原文、UTF-8/ASCII 控制字符、主机/IPv6、重复键和 64 KiB 上限；scheme 不区分大小写，query 参数名和值区分大小写。authority 后仅允许省略路径或根路径；query 最多 32 项。未知参数、编码后重名、冲突别名或畸形条目整份拒绝。

| 格式 | 认证、端口及协议参数 |
| --- | --- |
| `hysteria2://[auth@]host[:port][/][?query][#name]`，别名 hy2 | 缺省端口 443；auth 整体解码一次，userpass 的冒号作为完整密码保留，可省略/为空。query 仅 sni、insecure、obfs、obfs-password；obfs 仅 salamander/gecko 且必须非空密码，没有 obfs 时不接受孤立的 obfs-password |
| HY2 端口集合 | 显式端口可为单端口、升序范围或逗号组合；范围两端均为 1–65535，集合最多 128 段，拒绝反向/空段/重叠/重复，不排序或截断。映射 server_ports，省略被覆盖的 server_port，hop_interval 与 hop_interval_max 均为原生默认 30s；不从 Clash 套用端口范围修正或时长调整 |
| `tuic://UUID:password@host:port[/][?query][#name]` | 仅 v5，必须标准 UUID、原始冒号分隔和显式单端口；两部分各解码一次，密码中后续冒号原样保留，显式空密码允许。旧 token 凭据、整段编码冒号代替结构分隔、缺省/多端口不接受 |
| TUIC 协议参数 | congestion_control=cubic/new_reno/bbr，缺省保留原生 cubic；udp_relay_mode=native/quic，缺省保留原生 native。zero_rtt_handshake 缺省 false；disable_sni 缺省 false。布尔值仅 0/1/true/false，按下述别名规则核对 |
| `anytls://password@host[:port][/][?query][#name]` | 缺省端口 443，完整 userinfo 解码一次且密码必须非空，不裁空白或拆账号；仅单端口。query 为 sni、insecure，以及明确支持的 fp/alpn/security 扩展；会话时间/数量/metadata 不从参考库注入，保留固定原生默认 |

三者必须启用 TLS，默认验证证书，缺省 SNI 使用远端 host，显式 sni 不能空。HY2/TUIC 使用原生 QUIC TLS，不支持 fp/uTLS/Reality；AnyTLS 是 TCP TLS，本 URI 子集缺省 fp=chrome，显式值限固定支持集合并按共同账本记近似，不继承外围 Clash 全局指纹。AnyTLS 的 SNI 为 IP 时，固定 uTLS 不发送 SNI，仍以该 IP 验证证书；已由实际 ClientHello 和转发回归确认。

HY2/AnyTLS 的 insecure 仅 0/1，默认 0。TUIC 的 insecure / allow_insecure / allow-insecure 可用 0/1/true/false；不同别名同值接受、冲突拒绝，重复同一参数仍拒绝。TUIC 参数还接受已核对的 congestion-controller、udp-relay-mode、zero-rtt-handshake / reduce_rtt / reduce-rtt、disable-sni 别名，适用相同冲突规则，不用先取某个值覆盖其它参数。

TUIC/AnyTLS 可显式 alpn，使用非空逗号列表，各项非空、无两侧空白且最多 255 字节；可省略或显式 security=tls，其它值拒绝。TUIC disable_sni 只改变发送行为，不取消独立的 server_name 身份校验。新 TUIC UDP 模式只扩展类型化模型和 URI 入口，既有 Clash 字段白名单未扩大。

尚未支持 pinSHA256（完整证书摘要不等于原生公钥 pin）、ECH、HY2 Realm/mport/带宽/跳转时长/自定义 ALPN、TUIC v4 token/udp_over_stream/heartbeat、AnyTLS 会话参数和其它未列 query，即使空值也拒绝，不省略连接约束。未提供/空白 fragment 生成 `Hysteria2 序号`、`TUIC 序号` 或 `AnyTLS 序号`，名称和序号按混合列表统一处理。TCP/UDP 保留各协议的原生能力。

格式依据：[HY2 URI](https://v2.hysteria.network/docs/developers/URI-Scheme/)、[端口跳转](https://v2.hysteria.network/docs/advanced/Port-Hopping/)、[AnyTLS URI](https://github.com/anytls/anytls-go/blob/main/docs/uri_scheme.md)。TUIC 的协议依据为 [v5 规范](https://github.com/tuic-protocol/tuic/blob/master/SPEC.md)，分享格式取明确子集，对照 [v2rayN TUIC](https://github.com/2dust/v2rayN/blob/master/v2rayN/ServiceLib/Handler/Fmt/TuicFmt.cs) 与固定参考库 [explodeTuic](https://github.com/Aethersailor/SubConverter-Extended/blob/144f7c6bf6f7dd3da3d280730914b77f9df54631/src/parser/subparser.cpp)，不将它们的全部字段、默认或宽松行为视为本项目规范。目标依据固定 [HY2](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/docs/configuration/outbound/hysteria2.md)、[TUIC](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/docs/configuration/outbound/tuic.md) 与 [uTLS](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/common/tls/utls_client.go)。

## 编译、数据和规则

`src/converter/input/subscription.inc` 和公共/各格式 URI 适配器（含 `src/converter/input/legacy_ss.inc`）将七类协议、八种输入格式直接解析为类型化 `NodeSpec`，与 Clash 共用协议/TLS/传输校验和 sing-box 生成器。单独的策略辅助函数只装配选择组与 MATCH；
规则投影可以读取该策略，不构造临时 Clash 协议节点。下载、订阅文件和数据库保持原内容，不落盘重写成生成的 YAML。
导入、更新、规则投影、激活前对象目录、预览及桌面次来源共用这条转换，手机保持单普通来源。

节点列表没有原始路由规则，因此生成包含全部节点的手动 selector 和 MATCH 默认出口；组名避开
已有节点名称，不自动增加 DIRECT 成员。这项默认策略记字段 approx；用户可使用个人/全局规则分流。
标准节点参数和包装解码按现有协议映射记账，不把新增输入格式当成新增内核能力。

任何坏条目、未知字段、转换失败或后续内核检查失败，均拒绝整份候选；仍沿用独占暂存、候选检查、
原子替换及旧订阅/引用/活动配置保护。新解析诊断仅含行号、字段或原因，不输出原始 URI、UUID 或 JSON。

格式依据：[VMess 分享说明](https://github.com/2dust/v2rayN/wiki/Description-of-VMess-share-link)、
[订阅说明](https://github.com/2dust/v2rayN/wiki/Description-of-subscription)。

## 能力清单与内核一致性

`NodeSubscriptionCapabilities()` 返回拥有型 `NodeInputCatalog`：格式 ID、scheme/大小写、协议/provider 筛选类型、原生类型、历史格式标志，以及经过审查的内核版本/revision。它来自 [catalog.inc](../src/converter/input/catalog.inc) 的同一注册表，scheme 分流、默认显示名和 provider 类型读取该表；没有第二套猜测分流。字段白名单、默认值和组合边界仍以上述章节为准，清单不表示某协议的全部参数已支持，也不代替运行时内核探测。

必跑 `node_input_catalog` 将清单与独立编写的九份样例、生产普通/provider/别名路径、真实生成字段和保真度核对，再用固定内核 check。审查版本/revision 必须同时匹配桌面 CMake、Android Gradle/脚本、CI pin 和实际打包内核，另核对 iOS 脚本 revision；后者只是来源检查，不是 iOS 验收。内核升级须先重新审查映射，再更新清单，不能只改版本字符串通过门禁。

## 验证

`test_singbox` 覆盖包装变体、字段/传输/TLS 保留、整份拒绝、大小/数量上限、规则投影和对象目录。
`test_singbox --protocol-entries` 另核对主来源、HTTP/file provider 和桌面次来源的模型、来源默认、账本与只读入口；provider 的 `net=tls` 近似仅为筛选后参与的节点发布，与主订阅说明一致。
`test_singbox --ss-uri` 核对 SS 编码/插件/上限及混合列表；HTTP/file URI provider 的 `exclude-type: ss` 按真实 SS 类型筛选。生产 Prepare 的格式/候选失败保留原缓存，本地双内核运行回归包含 SS URI、SS 2022 provider 和混合列表的 TCP/UDP 转发；详情见[P6 首批证据](evidence/2026-11-15-protocol-p6-ss.md)。
`test_singbox --v2ray-uri` 覆盖 VLESS/Trojan 的协议默认、TLS/Reality/传输、严格拒绝和混合入口；provider 的 vless/trojan 类型独立筛选。`v2ray_uri_runtime` 必跑本地固定双内核转发及 TLS 身份拒绝，包含 Vision/Reality，见[P6 第二批证据](evidence/2026-11-15-protocol-p6-vt.md)。
`test_singbox --secure-uri` 核对 HY2/TUIC/AnyTLS 的认证、端口集合、默认、TLS/参数、混合 provider/目录/次来源与 Prepare 失败留旧；provider 筛选类型为 hysteria2（含 hy2 scheme）、tuic、anytls。`secure_uri_runtime` 必跑真实转发及三种证书身份拒绝，含 AnyTLS IP 的 ClientHello 观察；范围见[P6 第三批证据](evidence/2026-11-15-protocol-p6-secure.md)。
`test_singbox --node-inputs` 核对历史 SS 和能力登记；`--node-input-fixtures <目录>` 导出清单及内核检查配置。必跑 `node_input_catalog` 检查真实内核身份，并拒绝版本/revision、缺项/重复、类型/scheme、大小写及历史标志漂移。历史 SS 另有两个普通/provider 本地双内核 TCP/UDP 场景；范围见[P6 收尾证据](evidence/2026-11-15-protocol-p6-final.md)。
服务商样本只在独占临时文件中核对，不提交订阅地址、节点凭据或生成配置；编译与内核 check
不等于节点连通性、VPN 或真机测试。实际证据见 [验收记录](l1-l2-l3-status.md)。

## 后续计划

多协议 URI 与历史分享格式兼容的研究及分期安排见[解析扩展计划](node-subscription-plan.md)。
按[协议中间模型重构计划](protocol-refactor-plan.md)，P0–P6 的本轮范围已完成本机 Linux/Android 验证：七类协议 URI、历史 SS 与能力清单一致性检查已接入。其它历史分享格式继续按实际样本独立评估；Windows/macOS、手机真机和全部参数远端互通仍需后续验收，不能把本机门禁当作全部平台通过。
