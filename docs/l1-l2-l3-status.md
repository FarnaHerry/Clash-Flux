# L1 / L2 / L3 开发完成度复核

日期：2026-10-02。检查基线为 **v0.3.18**；后续 L2 工作已并入 **v0.3.19**。
v0.3.18 包含凭据/TLS 修复及 VLESS encryption、旧 WS 字段兼容修复；随后补
L2 UDP 编码、VMess 填充、HY2 混淆/跳端口、SS 插件/UOT 版本，以及逻辑路由
嵌套 GEO/RULE-SET、Hysteria v1 UDP 子集与带宽单位修复、SSH 认证/公钥子集。
以下明确区分当前批次与历史验证，不把本地检查等同于线上 CI 或全平台实机验收。

固定内核为 sing-box **1.14.2**，revision
`af6e64c3b69e6132ebaee0e1a3d24e93903f6709`；职责和约束以
[分层契约](singbox-layers-and-fidelity.md)为准，桌面最终模型以
[订阅编排设计](desktop-subscription-orchestration.md)为准。

## 判定口径与结论

“已接入”表示代码实现存在；“部分完成”表示仍缺字段、生命周期或产品入口；
“待实现”表示目标尚未接入；“不做”表示固定内核没有对应语义；iOS 单列为暂缓。
验证证据另分代码复核、构建、配置检查、交互/实机四种，不互相替代。

| 层 | 当前结论 | 可量化范围 | 距离完成的主要缺口 |
|---|---|---|---|
| L1 内核层 | 固定版本、资产校验、桌面/Android 控制链已接入；平台运行验收部分完成 | 官方桌面资产表 6 项；实际桌面 CI 4 项；本机验证 Linux x86_64 与 Android arm64 构建 | 其它平台实机、网络/权限/后台生命周期、故障恢复验收 |
| L2 翻译层 | 常用转换与结构化账本已接入，F01–F03 与兼容退化已修复；后续补 UDP/填充/HY2、SS 插件/UOT、嵌套 GEO/RULE-SET、Hysteria v1、SSH，整体仍部分完成 | Clash 代理协议类型 12/15；端点编辑转换 1/5；类型覆盖不表示字段完整 | 外部 provider、更多 DNS/协议字段及剩余取值边界审查 |
| L3 产品层 | 常用客户端页面、桌面编排首阶段、订阅唤醒已接入；最终产品形态部分完成 | 桌面 7 个一级页、手机 4 个一级页；支持的原生端点编辑器仍只有 OpenVPN client | 完整编排恢复、对象选择/追溯、原生 DNS/端点/规则集编辑、跨平台交互验收 |

不计算总完成百分比：内核有某个协议、编译器能生成它、界面能编辑它、真机能稳定
运行它是四件不同的事。`unsupported` 的显式拒绝解决诊断边界，不算新增协议支持。

## L1：内核层

| 项目 | 完成度与证据 | 边界 |
|---|---|---|
| 版本/revision 固定 | 已接入。CMake、Android Gradle、libbox 脚本、CI 使用相同 pin；本地 `sing-box version` 返回 1.14.2 和上述 revision | 本轮不查询或宣称它仍是线上最新版本 |
| 桌面资产与缓存 | 已接入。6 个官方归档分别固定 SHA256，缓存按版本/资产摘要隔离；本机 Linux 归档摘要匹配，缓存二进制与 `build/engines/sing-box` 一致 | 6 个资产映射不等于 6 个客户端发行目标均已构建 |
| Android libbox 来源 | 已接入。正式 tag/revision 核验，AAR 元数据核验版本、revision、ABI、SHA256；本地 arm64 AAR 摘要重新核对通过 | 元数据与构建来源核验不替代真机运行版本/行为验证 |
| 桌面运行与控制 | 已接入。官方进程 + `clash_api`；启停、模式、TUN、selector、测速、连接和日志有调用路径 | 网络权限、系统代理恢复和各平台退出清理仍需实机验收 |
| Android 所有权 | 已接入。`:background` 持有 VpnService/libbox，AIDL/Binder 回传快照；数据库由 UI 进程拥有，配置以原子文件共享 | 本轮只构建 APK，未重新验证 Doze、进程重建、切网、后台长时运行 |
| 候选配置检查 | 桌面已接入固定内核 `check`，通过后才停止旧实例；Android 由后台 libbox 启动流程检查 | Android 没有桌面 CLI 预检查；`check` 不证明远端握手或真实流量行为 |
| 恢复与事务 | 部分完成。有本进程上一份计划和失败恢复，旧原生资源失效时不恢复旧引用 | 无跨 SQLite/文件/路由的持久事务和崩溃 journal；手机异步失败回滚不完整 |

代码证据：[资产缓存](../cmake/singbox_bundle.cmake)、
[AAR 核验](../platform/android/app/build.gradle)、
[libbox 构建](../platform/android/build-singbox-libbox.sh)、
[桌面候选与启动](../src/store/core_store_lifecycle.inc)、
[候选检查/GEO 预取](../src/store/core_store_helpers.inc)、
[Android 服务](../platform/android/app/src/main/java/dev/farna/clashflux/ClashVpnService.java)。

实际工作流矩阵为 Linux x86_64、Linux arm64、Windows x86_64、macOS arm64、
Android arm64，另有 Windows 安装器门禁。Linux arm64 是非阻塞实验项；
Windows/macOS/Android 的构建门禁已启用。Windows arm64 和 macOS x86_64 当前没有
对应 job。以[工作流实际 matrix](../.github/workflows/build.yml)为准，不能从其历史
“六项全部入矩阵”注释推导当前支持范围。本次没有查询这些 job 的最近一次运行结果。

**iOS 暂缓**：Simulator/Packet Tunnel/Libbox 编译配方是非阻塞诊断，未签名、不生成
IPA、不进入 Release 门禁；不计为完成的平台支持。

## L2：翻译层

| 领域 | 已完成范围 | 未完成/近似范围 |
|---|---|---|
| 代理协议 | SS、VMess、VLESS、Trojan、Hysteria v1 UDP 子集、HY2、TUIC、HTTP、SOCKS、AnyTLS、Snell v4、SSH，共 12 类 | ShadowTLS、Tor、Naive 的 Clash 转换未接入；12 类仍非全部字段覆盖，HY1 窗口/快速打开/伪装等边界见本批说明 |
| TLS/传输 | TCP、WS、gRPC、HTTP/h2、HTTPUpgrade；SS simple-obfs HTTP/TLS 与 v2ray-plugin WebSocket/TLS/mux 子集；SNI、ALPN、客户端指纹及部分 Reality 字段；F02/F03 异常值/未启用字段显式拒绝 | 其它 SS 插件和插件自定义 headers/证书/ECH 等仍未映射；random 指纹分布及 PSK/PQ 别名折叠记 approx |
| 策略组 | selector/urltest、初始选择、间隔/容差；过长间隔保留并延长 idle_timeout、记近似 | fallback/load-balance 只能近似成 urltest；锁定、lazy/timeout 等不能伪装支持 |
| 拨号/依赖 | `dialer-proxy`→detour，组成员与 DNS resolver 共同检查循环；接口、mark、TFO、MPTCP 按平台处理 | detour 忽略物理选项、MPTCP IPv6 差异记 approx；跨来源原始代理链未开放 |
| DNS | local、UDP/TCP、DoT/DoH/DoQ/h3、bootstrap；完整域名/最长后缀 nameserver-policy 和节点专用 policy | 更多通配/规则集 policy、fallback-filter、hosts/FakeIP、direct follow-policy 转换未完成；多服务器/fallback/直连解析时机有近似 |
| 普通路由 | 域名/正则、源/目标 CIDR、端口/范围、TCP/UDP、IP-VERSION、GEO、桌面进程；Linux UID、Android 包名/正则 | 平台不适用字段拒绝；Android owner 查询失败/共享 UID 的实际行为未验收 |
| 逻辑与 inline provider | AND/OR/NOT 有界子集，路由支持嵌套 GEO/已转换 RULE-SET；domain/ipcidr/classical inline；失败子条件/整份 payload 原子拒绝，回滚新建资源，拒绝重名 provider | HeadlessRule 不接受 route 的 UID/IP-VERSION/GEO/RULE-SET；外部 provider 与其它未映射条件仍待实现 |
| 规则集下载 | 自有 GEO 显式资源清单、桌面按周刷新、独占临时文件与提交前筛查；Android 固定打包 CN 资产 | 外部 HTTP/file provider、YAML/text/MRS 全格式生命周期和管理 UI 未实现；SRS 文件头筛查不是完整解析 |
| 规则终止/失败 | 首个有效 MATCH 终止；DIRECT 不改成 selector；无效兜底失败；REJECT-DROP 使用原生 drop；失败仍返回已产生账本 | 保留原生内核语义，不提供停核后的系统 kill switch |
| 原生配置/连接 | 原生 JSON 保留用户字段并合并托管项；OpenVPN `.ovpn`→openvpn-client；PPTP 走平台补充路径 | JSON 不是完整可视化编辑器；原生 JSON 次来源不支持；WireGuard/Tailscale/OpenConnect/OpenVPN server 编辑转换未接入 |
| 保真度闭环 | `CompileResult.fidelity`→CoreSnapshot→设置明细、动作 toast、组角标、CLI；节点键名与本批 TLS/传输值均有拒绝边界 | 本批回归封住 F01–F03，不证明所有其它字段和原生配置合并都已完成审查 |

代码证据：[节点](../src/singbox_proxy.inc)、[DNS](../src/singbox_context_dns.inc)、
[规则/provider](../src/singbox_rules.inc)、[主配置/托管边界](../src/singbox_compile.inc)、
[多来源](../src/singbox_sources.inc)、[原生入口](../src/singbox.cpp)、
[OpenVPN](../src/singbox_yaml_openvpn.inc)。端点 1/5 指编辑和转换入口，PPTP 不计入
sing-box 端点分母；WireGuard 已从 outbound 移除，不能算作未接入的出站协议。

### 本轮已经关闭的问题

- 旧协议的 `fingerprint` / `shadow-tls-opts` 不再静默消失，逐字段记 unsupported，
  整条节点拒绝；引用被拒绝节点的 MATCH 失败，保留账本。
- 显式 `udp: false` 在有对应原生限制的协议上转 `network: tcp`；HTTP 的 UDP 意图记
  approx；无效布尔/HY2 带宽拒绝，HTTP 多 path 取首项记 approx。
- Trojan/HY2/TUIC 默认 TLS、QUIC 不注入 TCP uTLS、TUIC `tls.disable_sni` 与 ALPN/SNI
  共用转换已修复。SOCKS TLS 及 QUIC 不支持的 uTLS/Reality 明确拒绝。
- 固定 1.14.2 内核 `check` 已接受当前 **11 类协议**组成的配置；这没有证明服务器握手。
- F01–F03 修复及永久回归已接入，见下表；同时修复 OpenVPN inline 凭据按空格拆词的问题。

### F01–F03：初始复现与首批修复（2026-10-02）

下表保留修复前的生产编译器复现。新增永久回归在修复前得到 48 项失败；修复后
连同额外边界测试通过。所有样本使用占位凭据，诊断不输出凭据内容。

| ID / 状态 | 修复前输入与观察 | 修复后行为与回归 |
|---|---|---|
| F01 / 已修复 | Trojan、SS 的 `password: ' sample '` 输出变成 `"sample"`，编译成功、fidelity=0。旧分支凭据经 `ytext()` 被 trim | SS/Trojan/HY2/TUIC/HTTP/SOCKS 的 password、username 和 HY2 obfs-password 按 scalar 原文读取；保留 AnyTLS/Snell 原文行为。类型异常拒绝并记账；可选空 HTTP/SOCKS 凭据保留。OpenVPN inline 按两行读取，只去 CRLF 分隔，不拆词/去引号/trim；缺行或需交互的空凭据明确失败并记来源账本 |
| F02 / 已修复 | TLS VLESS 的 `alpn: {protocol: h2}` 被删；VMess WS 的 `headers: {Host: [example.test]}` 被删；均保留节点、编译成功、fidelity=0 | ALPN scalar/list 完整校验，协议名 1–255 字节，空数组保留；WS headers 必须为 scalar 键值对象，拒绝重复键和异常成员。HTTP/h2 path/host、gRPC/HTTPUpgrade 字符串及 WS early-data 参数也校验；异常整条拒绝，不保留半份传输配置 |
| F03 / 已修复 | 根级 `global-client-fingerprint: firefox` + 未设节点指纹的 Trojan 输出 chrome，fidelity=0。未启用 TLS 的 VLESS 显式 ALPN/client-fingerprint 也被省略且 fidelity=0 | 每个 Clash 来源独立继承全局指纹，节点优先，均省略时保留 chrome 基线；非法全局值拒绝整份来源并保留账本。未知节点指纹、未启用 TLS 的显式 TLS 字段拒绝；tls:false 不再因 SNI/Reality 偷开 TLS。QUIC 不继承 TCP uTLS；random 分布和内核折叠别名记 approx |

相关位置：`singbox_yaml_openvpn.inc::ytext/ylist`、
`singbox_proxy.inc::appendTls/appendTransport/convertProxy` 和 Clash 文档入口。
永久测试在 [test_singbox.cpp](../tests/test_singbox.cpp)；多来源同名节点分别继承
firefox/safari，并验证无全局值的次来源不会继承主来源指纹。不能因这批问题关闭
就宣布整个 L2 契约完成；更多协议/DNS/provider 的字段范围仍按上表继续推进。
对照样本：Trojan 的未映射证书/组合字段产生 2 条账本且节点被拒绝；根级 `hosts`
产生 1 条 unsupported，因此没有把 hosts 错记成另一个静默丢弃缺陷。

## L3：产品层

| 项目 | 完成度 | 当前行为与缺口 |
|---|---|---|
| 常用页面/控制 | 已接入 | 首页、订阅、代理、规则、连接、日志、设置；真实 selector 切换、测速、模式、TUN、系统代理有领域动作，手机使用自身能力限制 |
| 应用模型/持久化 | 已接入 | application service + 唯一数据泵、typed JSON codec、异步 SQLite hydrate/flush、旧库事务迁移；并非配置生成器的完整 typed IR |
| 分页/虚拟化 | 部分完成 | 二级 Pager 跟手、取消、反选、标签居中与有界大组已有生产回归；隐藏一级页保留 hook 状态并卸载重内容，其重新可见时的内层滚动恢复仍有已知代价 |
| 保真度呈现 | 已接入，受 L2 输出完整性限制 | 设置完整明细、用户动作后的摘要、组 `!`、CLI 明细；账本漏记时这些消费点无法自行发现问题 |
| 唯一主订阅 + Clash 次来源 | 首阶段已实现 | 主来源 Clash/原生 JSON；普通 Clash 次来源仅由启用规则参与；同名对象隔离，无引用不载入；手机保持单活动普通订阅 |
| 编排规则编辑 | 首阶段已实现 | UserOverride/SourcePolicy、priority/order、启停、Default/Group/Node、Reject/UseMain/Direct；对象名仍手工输入，目录选择器未完成 |
| 编排身份/持久化 | 部分完成 | policy v2、source ID + kind + 原始唯一名称；不是对象 UUID；对象改名会使引用失效，独立 binding 表待做 |
| 应用/回滚 | 部分完成 | 候选 check、成功应用运行序号、预览与运行来源映射分离、进程内失败恢复；无崩溃 journal、跨重开上一有效计划和手机异步事务恢复 |
| 来源/连接追溯 | 部分完成 | 代理、托盘、首页、连接链显示可读来源；连接有内核规则文本，尚不能定位到具体 L3 policy rule ID |
| 网页唤醒订阅 | 已接入，平台验收部分完成 | sing-box/FlClash/Clash 别名与专用协议；独立预填表单、粘贴/扫码、桌面单实例 IPC；Linux 冷启动预填和并发转发已验证，Android 构建/manifest 核对；其它系统与真机 intent 未验收 |
| sing-box 独有能力编辑 | 部分完成 | OpenVPN client 为原生连接；原生 JSON 可导入；WireGuard/Tailscale/OpenConnect/OpenVPN server、原生 DNS、通用 rule_set 编辑器未完成 |
| Android 应用路由 | 部分完成 | package_name/regex 输入及后台 owner 查询已实现；应用选择 UI 未做，API 29 前后、共享 UID、查询失败需真机验证 |

编排代码证据：[规则领域](../src/vpn.cppm)、[路由](../src/routing.cppm)、
[规则编辑器](../src/ui/rules_page.cpp)、[来源编译](../src/singbox_sources.inc)、
[订阅候选/激活](../src/store/profiles.cppm)、[来源映射](../src/store/core_store_state.inc)、
[连接展示](../src/ui/connections_page.cpp)。唤醒格式与接入见
[profile-links.md](profile-links.md)。

本次定向检查确认：同名双来源编译成功（参与来源 2 个）；禁用唯一引用后为 1 个；
次来源不可用时分别生成 reject、主默认出口或主 DIRECT，匹配条件仍保留；原生 JSON
次来源明确失败并记账；`a→b→a` detour 循环明确失败。不能再把首阶段写成“完全未实现”，
也不能把这些配置检查称作已完成切主、刷新、故障注入与真实流量的端到端验收。

## 前序 L2 批次：UDP / VMess 填充 / HY2（2026-10-02）

- VMess/VLESS `packet-encoding` 与历史 `packet-addr`/`xudp` 映射，保留各协议默认
  和输入优先级；VMess `global-padding`、`authenticated-length` 直接写原生字段。
- HY2 salamander/gecko 按实际类型映射，缺密码与未知混淆拒绝；密码单独出现不再
  擅自开启混淆，非活动参数记账。Gecko 包尺寸按固定内核默认和上限校验。
- HY2 ports 原子解析，单端口输出原生 singleton 范围，支持只有 ports 的节点；
  hop-interval 支持秒数/范围，缺省 30 秒、最小 5 秒限制和不生效字段均明确处理。
- 新永久回归在实现前复现 43 项失败；修复后 `test_singbox` 及项目 9 项测试通过。
  更新的 [配置样本](examples/layers-audit-2026-10-02.yaml) 当批包含 10 协议、11 个节点（后续 HY1 批扩至 11 协议、12 节点），
  生产编译器输出通过 sing-box 1.14.2 `check`（退出码 0）。
- 三份本地订阅的独立测试副本保留节点及代理组字段；为满足现有 harness 的国内
  规则断言，替换 rules 并去掉 rule-providers 声明。节点保留 32/32、30/30、34/34，
  三份输出均通过固定内核 check；这不验证原订阅全部规则/provider。私有凭据未入库。
- `cmake --build build --target clash-flux test_singbox`、`./run.sh --version`
  （v0.3.18）通过；Android `:app:assembleDebug --offline --no-daemon` 重新构建成功。
  `git diff --check` 通过。本批未提交、打标签、推送或发布；未做远程线路握手或真机安装。

字段及保真度细节见[分层契约](singbox-layers-and-fidelity.md)。L1 固定版本不变，
L3 没有新增编辑入口；类型覆盖仍为 10/15，不能把字段补齐计作新增协议或整体完成。

## 后续 L2 批次：SS 插件与 UOT 版本（2026-10-02）

- 接入内核内置 simple-obfs HTTP/TLS 和 v2ray-plugin WebSocket/TLS/mux 子集，
  保留 Clash 的 host/path/mux 默认，选项字符串按 SIP003 转义，TLS false 不输出启用键。
  未映射参数、重复键、异常类型和缺 mode 整条拒绝并记账；不调用外部插件进程。
- 修复 SS UOT 默认版本差异：此前 `udp-over-tcp:true` 写布尔值会走原生 v2，
  现在显式写 v1，与 Clash 缺省/0 的有效版本一致；输入显式 v1/v2 均支持。
- 永久回归实现前复现 16 项失败，修复后通过；`ctest` 指定的 9 项项目测试全部通过。
  并行样本检查还复现了 harness 共用 /tmp 目录互相覆盖的问题；规则集测试现在
  独占临时目录并由 RAII 清理，8 个并行测试进程全部通过。
  [8 节点样本](examples/ss-plugins-2026-10-02.yaml) 全部保留，生成配置通过固定
  sing-box 1.14.2 `check`。之前的 UDP/HY2 样本仍通过同一编译器回归。
- 三份订阅节点/组的规范化副本再次保留 32/32、30/30、34/34，输出均通过内核
  check；测试仅替换 rules、去除 rule-providers，未改源订阅或验证全部原规则，凭据未入库。
- Linux `cmake --build build --target clash-flux test_singbox`、`./run.sh --version`
  （v0.3.18）、Android `:app:assembleDebug --offline --no-daemon` 和 `git diff --check`
  通过；未进行远程握手/真机安装，未提交、打标签、推送或发布。

复现配置检查：

```bash
build/test_singbox docs/examples/ss-plugins-2026-10-02.yaml \
  platform/android/app/build/generated/rule-set-assets/rules \
  /tmp/clash-flux-l2-ss-audit.json
build/engines/sing-box check -c /tmp/clash-flux-l2-ss-audit.json
```

## 后续 L2 批次：逻辑路由嵌套 GEO / RULE-SET（2026-10-02）

- AND/OR/NOT 路由接入 GEOIP/GEOSITE、private/lan 和已转换 inline RULE-SET 引用；
  顶层和子条件共用资源转换，保持原生 logical/mode/invert，动作只位于最外层。
  未声明的国内别名仍记 approx；失败声明不能替换成同名国内别名。
- 子条件失败时整条逻辑规则拒绝，保留保真度明细，RAII 撤回本条新增 rule_set、
  自有 GEO 资源及 tag 缓存；既有资源保留，后续有效规则仍能生成。未知路由目标
  在解析子树前拒绝，不产生无用 GEO 资源。
- 子条件不推导全局 CN DNS 分流，避免 NOT/复合匹配扩大 DNS 策略。HeadlessRule
  继续拒绝 GEO/RULE-SET，不把路由字段写进 inline classical；Android 本地 GEO
  与手机单来源限制保持。桌面双来源回归确认嵌套引用正确改写，缓存 tag 保持原名。
- 永久回归在实现前复现 5 项失败，修复后全部通过；项目 `ctest` 9/9 通过。
  [公开逻辑样本](examples/logical-rules-2026-10-02.yaml) 的 6 条逻辑规则、3 份 inline
  provider 和 2 份 GEO 资源全部生成，输出通过固定 sing-box 1.14.2 `check`。
  UDP/HY2、SS 插件样本仍通过同一生产编译器和固定内核检查。
- 三份私有订阅再次使用独占临时副本，仅替换 rules 并去除 rule-providers；节点/组
  字段保留，32/32、30/30、34/34 节点均生成且配置 check 通过。副本已清理，
  不验证源订阅全部规则/provider 或远程握手，凭据未入库。
- Linux `cmake --build build --target clash-flux test_singbox`、`./run.sh --version`
  （v0.3.18）、Android `:app:assembleDebug --offline --no-daemon` 和 `git diff --check`
  通过。未进行真机安装，未提交、打标签、推送或发布。

复现配置检查：

```bash
build/test_singbox docs/examples/logical-rules-2026-10-02.yaml \
  platform/android/app/build/generated/rule-set-assets/rules \
  /tmp/clash-flux-l2-logical-audit.json
build/engines/sing-box check -c /tmp/clash-flux-l2-logical-audit.json
```

该逻辑批次时 L2 类型覆盖为 10/15；外部 provider、hosts/FakeIP、更多 DNS/TLS
字段及 L3 原生编辑入口不在此批交付范围。

## 后续 L2 批次：Hysteria v1 UDP 子集 / 带宽单位（2026-10-02）

- 新增 Hysteria v1 的 UDP 转换，协议类型覆盖为 **11/15**；认证、XPlus 混淆、
  正整数带宽、协议 TLS/SNI/ALPN、严格 ports 和 10 秒 hopping 默认已接入。
  auth 的 Base64 字节优先于 auth-str；凭据保留原文并验证 65535 字节线上长度。
- 接收窗口按实际输入生态的 stream/connection 方向映射；成对非零窗口 exact，
  默认窗口保留 15/64 MiB 上限但初始分配不同，明确记 approx；单边窗口组合尚未接入。
  非 UDP 伪装、fast-open:true、显式空 ALPN、证书/ECH/指纹约束及兼容 up-speed/
  down-speed 等字段拒绝整条节点，不省略后继续连接。
- HY1/HY2 共用带宽转换，修复旧 HY2 将 `3 MBps` 当作 `3 Mbps` 的问题，现在
  写 24 Mbps；大小写、类型、正数及乘法范围检查不允许静默回落自动带宽。
- 永久回归在实现前复现 16 项失败，修复后通过，并新增认证 padding/CRLF/二进制、
  16 位长度边界和失败 MATCH 保留账本回归。项目 `ctest` 9/9 通过。
  [HY1 样本](examples/hysteria1-2026-10-02.yaml) 5/5 节点保留；总协议样本扩充为
  11 类、12 个节点，全部生成。两者及 SS 插件、逻辑路由样本均通过固定 1.14.2
  内核 `check`；这些配置检查不验证远端握手或真实流量。
- 三份私有订阅独占临时副本继续保留 32/32、30/30、34/34 节点，配置 check
  通过；仅替换 rules 并去除 rule-providers，未改源订阅或验收全部原规则，副本已清理。
- `cmake --build build --target clash-flux test_singbox`、`./run.sh --version`
  （v0.3.18）、Android `:app:assembleDebug --offline --no-daemon`、`git diff --check`
  通过。本批未真机安装、提交、打标签、推送或发布，L1 固定内核不变，L3 无新增编辑器。

复现配置检查：

```bash
build/test_singbox docs/examples/hysteria1-2026-10-02.yaml \
  platform/android/app/build/generated/rule-set-assets/rules \
  /tmp/clash-flux-l2-hy1-audit.json
build/engines/sing-box check -c /tmp/clash-flux-l2-hy1-audit.json
```

本批结束时尚未完成 ShadowTLS/SSH/Tor/Naive 的 Clash 转换；更多 HY1/HY2 字段、外部
provider、DNS/hosts/FakeIP 与 L3 编辑入口继续按契约推进。

## 后续 L2 批次：SSH 认证 / 主机公钥子集（2026-10-02）

- 新增 SSH 的 Clash YAML 转换，协议类型覆盖为 **12/15**；密码、内联 PEM 私钥、
  加密私钥口令、主机公钥和固定原生库的 20 种主机算法偏好列表已接入。
  所有认证字段保留原文，列表完整校验，不删异常成员后保留部分配置。
- 双认证完整保留两种凭据，密码/私钥尝试顺序差异记 `Node/Approx`；无私钥的非空
  口令记不生效。显式缺失/空用户名拒绝，避免被原生默认 root 改写。
  外部私钥路径、显式空算法列表和其它未映射字段拒绝整条节点。
  SSH 不输出原生不存在的 TLS/network 字段，请求 UDP 则明确记 TCP-only 降级。
- 通用 dialer/platform 限制不变。永久回归覆盖缺失/循环 detour、失败 MATCH 保留
  账本、双来源同名 SSH 的认证字段和 detour 命名空间隔离；首组回归在旧实现复现
  9 项失败，修复后通过，另补所有算法、列表与来源边界。项目 `ctest` **9/9** 通过。
- [公开 SSH 样本](examples/ssh-2026-10-02.yaml) 5/5 节点保留，包含专门生成的公开
  测试私钥、加密私钥、固定公钥、双认证和代理链；固定 1.14.2 `check` 通过。
  内核还正确拒绝了错误解密口令与无效主机公钥两个临时候选。
  总协议样本为 12 类、13/13 节点，HY1 5/5、SS 插件 8/8 及逻辑规则样本再次通过 check。
- 三份规范化私有订阅副本保留 32/32、30/30、34/34 节点，并通过内核 check；只替换
  rules、去除 rule-providers，源订阅未改，独占临时副本已清理。这不验证全部原规则。
- `cmake --build build --target clash-flux test_singbox`、`./run.sh --version`
  （v0.3.19）、Android `:app:assembleDebug --offline --no-daemon`、`git diff --check`
  均通过；Android 构建日志为 `/tmp/clash-flux-l2-ssh-android-build.log`。
  未验收远端 SSH 握手、真实流量或本批真机生命周期；L1 pin 不变，L3 未新增编辑器。

复现配置检查与构建：

```bash
build/test_singbox docs/examples/ssh-2026-10-02.yaml \
  platform/android/app/build/generated/rule-set-assets/rules \
  /tmp/clash-flux-l2-ssh-audit.json
build/engines/sing-box check -c /tmp/clash-flux-l2-ssh-audit.json
# 在 platform/android 下运行：
JAVA_HOME=/opt/android-studio/jbr ANDROID_HOME=/home/farna/Android/Sdk \
  HUXERUI_HOME=/home/farna/.local/share/HuxerUI \
  ./gradlew :app:assembleDebug --offline --no-daemon
```

当前仍缺 ShadowTLS/Tor/Naive 的 Clash 转换、外部 provider 和更多 DNS/协议字段。
保真度分类与上游依据见[契约](singbox-layers-and-fidelity.md#ssh-认证与主机公钥子集2026-10-02)。

## 历史首批验证记录与复现（v0.3.16 工作区）

| 验证 | 结果/范围 |
|---|---|
| Linux 主目标 + HuxerUI codegen | `cmake --build build --target clash-flux` 通过 |
| 应用版本 | `./run.sh --version`：v0.3.16，退出码 0 |
| 应用测试 | smoke/profile_links/page_transition/singbox/vpn/routing/compensation/sqlite_orm/persistence：9/9 通过 |
| 编译产物内核检查 | 配套 10 协议样本扩充原文凭据、WS header、全局/节点指纹；生产编译器生成 JSON，固定 1.14.2 `check` 退出码 0 |
| 定向语义检查 | 修复前复现 F01–F03；修复后永久回归通过（包括 OpenVPN LF/CRLF 凭据和来源指纹隔离）；前次多来源/不可用策略/循环及 JSON 次来源拒绝符合上述边界 |
| Android 本地 Debug | `:app:assembleDebug --offline --no-daemon` 成功；versionName 0.3.16、minSdk 24；AAR 身份和摘要匹配，APK 含 GEOIP/GEOSITE CN |
| APK 签名 | `apksigner verify --verbose --min-sdk-version 23`：v1/v2 均为 true；这是 Debug 签名，不是发布密钥验收 |
| Linux 唤醒/IPC | 本轮会话中冷启动预填窗口正常，编码 token 保持原样；20 个并发转发退出码全为 0，串行保护后无二进制 footer 泄漏；未完成表单确认下载的端到端验收 |
| 工作区检查 | `git diff --check` 通过；未提交、打标签、推送或发布 |

当前 CTest 共注册 32 项（包含上游 HuxerUI），其中 10 项缺少可用命令/可执行文件。
本轮执行的是上述 **9 项项目测试**，没有宣称整个 32 项套件通过。F01–F03 已加入
永久回归；其它前次编排语义探针仍是本地审查样本。

可重复的正向配置检查：

```bash
cmake --build build --target clash-flux test_profile_links test_singbox
./run.sh --version
ctest --test-dir build -R '^(smoke|profile_links|page_transition|singbox|vpn|routing|compensation|sqlite_orm|persistence)$' --output-on-failure

# 先按 Android 构建文档生成固定 GEO 资产，或使用同名的已校验缓存目录。
build/test_singbox docs/examples/layers-audit-2026-10-02.yaml \
  platform/android/app/build/generated/rule-set-assets/rules \
  /tmp/clash-flux-layers-audit.json
build/engines/sing-box check -c /tmp/clash-flux-layers-audit.json
git diff --check
```

本次没有新增远程线路握手、实际 DNS/分流行为、吞吐/功耗/后台长期运行验证；
没有在 Windows/macOS 编译或运行最新工作区，也没有安装本次 APK 进行 Android
浏览器 intent、返回手势、VPN 重建验收。历史真机性能记录不等于本轮改动已实测。

## 下一步交付与完成条件

| 优先级 | 任务 | 可以标成完成的条件 |
|---|---|---|
| P0 | 继续清点剩余取值和默认语义边界 | 已关闭 F01–F03；后续逐字段覆盖 DNS/协议/组输入类型、默认值及拒绝策略，禁止新增静默改写，补永久回归和固定内核检查 |
| P1 | 补桌面计划与手机生命周期验收 | 切主、启停规则、刷新/删除、失败回滚、进程重建有可重复场景；Windows/macOS/Android 各自留实际记录 |
| P1 | 外部 provider 与原生规则集 | 下载来源、原子提交、格式转换、顺序、失败留旧、刷新/删除均有定义与测试；再开放管理入口 |
| P1 | 完成桌面编排最终形态 | JSON 次来源、跨来源链、对象目录/身份迁移、rule ID 追溯、持久恢复 journal；完整验收，不扩展手机普通多订阅 |
| P2 | 原生能力产品化 | 优先原生 DNS/高级拨号和新的 endpoint；每个连接对象包含字段、状态、路由、启停与恢复，不以 JSON 可导入冒充编辑器完成 |

`urltest` 手动锁定、原生 fallback/load-balance、真实 providers 面板等固定内核没有的
能力归为“不做”，不放进待修 bug 或待完成百分比。iOS 是否恢复支持由后续项目决策
决定，本清单不产生 iOS 发布承诺。
