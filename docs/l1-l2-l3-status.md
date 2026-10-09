# L1 / L2 / L3 开发完成度复核

2026-10-04 订阅切换性能：修复大 MRS 来源反复构造 YAML 节点、逐条 JSON 后再合并的
开销，改为有界字符串集合直接累计原生字段数组；固定快照未变化时不重复编译，
桌面没有 GEO 预取依赖时复用完整编译结果执行内核 check。没有跳过规则转换、
身份核验或候选检查，也没有修改数据库 schema、订阅映射和缓存提交边界。
新增回归覆盖 4096 条域名全部保留、原生 OR 分支数量有界，以及固定快照被内核
拒绝时仍完成检查且保留输入/旧缓存。
kitty 的独占离线副本（11 个规则集、2 份 ASN，当前 Debug 构建）准备耗时
**5.33s → 2.06s**，峰值 RSS **211904 → 64556 KiB**；两次生成的完整 JSON 相同，
49 条 fidelity 保留，生成配置通过固定 sing-box 1.14.2 check。
复制包含多来源及被引用文件的应用数据到独占 XDG_DATA_HOME，由应用 CLI
`profile use 7` 切换 kitty，停止内核状态下约 **4.50s** 完成；另一次应用运行重开后
确认五条订阅仍在且 kitty 为 main，所有复制的订阅文件与用户原文件相同。
未修改用户订阅或当前 main；该耗时不包含运行中内核重启或手机/其它桌面实机验收。
验证：`cmake --build build --target clash-flux test_singbox test_rule_provider_download -j2`、
`./run.sh --version`、`ctest --test-dir build -L clashflux-required --output-on-failure --no-tests=error`
（15/15）、隔离 `test_singbox --prepare-config` / 应用 CLI 与固定内核 check、`git diff --check`。


2026-10-04 L2 增量：基于 `7bbc149c81b728f9e1717440a29fff447ba13fb0` 加本地未提交改动，
修复 classical inline/file/HTTP provider 拒绝合法 `IP-CIDR,…,no-resolve` 的问题。
IPv4/IPv6 及嵌套 IP 条目保留匹配并记 Rule/Approx；未知/空/重复修饰符仍整份拒绝，
不改变订阅存储或候选提交边界。`singbox` 回归包含三条 provider 路径和失败位置，
`rule_provider_download` 回归通过；脱敏样本
`examples/classical-provider-no-resolve-2026-10-04.yaml` 编译后通过固定 1.14.2 内核 check。
这是本地转换与配置检查，尚未验收用户真实订阅的完整导入或手机数据面。

2026-10-04 L2 第二批增量（同一代码快照）：补 ASN 网段快照和 MRSv1 解码。
`IP-ASN` / `SRC-IP-ASN` 包括 classical 与逻辑子条件，完整双地址族数据转为原生
CIDR 并记 approx；MRSv1 domain/ipcidr 支持 ASCII 精确域名、子域、整层通配与
IPv4/IPv6 区间，损坏/未知格式、超限整份拒绝。HTTP 依赖分轮准备，完整检查前
不提交任何缓存，回归覆盖 ASN 失败与内核拒绝时旧缓存逐字节保留。
用户提供的真实订阅在独占临时目录准备 **11 个规则集 + 2 份 ASN 快照**，生成
配置通过固定 sing-box **1.14.2 check**；没有修改用户订阅、活动配置或数据库。
这是 Linux 本地转换和内核配置验收，不能写成 GUI 完整导入、数据面联网或手机实机验收。
命令：`cmake --build build --target clash-flux test_singbox test_rule_provider_download -j2`、
`ctest --test-dir build -R '^(singbox|rule_provider_download)$' --output-on-failure`（2/2），
`build/test_singbox --prepare-config <private-yaml> <isolated-fixtures> <private-json>`、
`build/engines/sing-box check -c <private-json>`、`./run.sh --version`、`git diff --check`。
补充检查：`cmake --build build -j2` 成功；`ctest --test-dir build --output-on-failure`
共 38 项，其中本项目必跑 15 项全部通过。HuxerUI 附加测试有 9 项因可执行文件缺失
未运行，`HuxerUIAndroidHttpTests` 与 `HuxerUIRuntimeDependencyTests` 失败（后者
报告 relocated runtime segmentation fault）；因此不能宣称全量 CTest 通过。
上述 HuxerUI 附加测试不等同于本批的 ASN/MRS 转换或 Android 实机验收。

MRS 固定 Zstandard 1.5.7 静态依赖已接入平台共用 CMake；Android 兼容 TU 跟踪新
解码片段。本批未执行 Android/Windows/macOS 编译，不把通用接入写成平台验收。

2026-10-04 产品修复：URL 导入后的按钮 loading 不结束。
现场 GUI 的本次订阅已持久化成功、相关缓存已提交、后台任务线程空闲；修复导入
结果与页面生命周期耦合的路径，使用应用级 `LaunchProfileImport` 完成回传，统一
先清除 loading 再调用 UI 完成反馈，异常变为失败结果，重复点击拒绝。桌面弹窗、
窄屏旧表单、独立表单与 URL 快捷入口已接入。没有删除/刷新用户已有订阅。
`page_transition` 生产封装回归验证 await 中卸载发起视图、完成后按钮恢复、异常反馈
和失败后重试通过；本批重新执行 HuxerUI codegen + clash-flux 构建。
验证：`cmake --build build --target clash-flux test_page_transition -j2` 成功；
`ctest --test-dir build -L clashflux-required --output-on-failure --no-tests=error` 15/15；
`./run.sh --version` 与 `git diff --check` 通过。
现场仍运行旧 GUI，不能把它写成新构建的真实链接 GUI 导入验收；需重启加载修复。

日期：2026-10-03。检查基线为 **v0.3.18**；后续 L2 工作已并入 **v0.3.19**。
本批代码快照为 `7bbc149c81b728f9e1717440a29fff447ba13fb0`（v0.3.19）加本地
未提交的 file/HTTP rule-provider、单值请求头、精确 hosts、加密 DNS 证书参数、策略通配、组节点展开、L3 目标目录及共用页面布局增量，应用版本仍为 0.3.19；本批未发布。设置页 Material 3 重构已于 2026-10-04 单独回退。
v0.3.18 包含凭据/TLS 修复及 VLESS encryption、旧 WS 字段兼容修复；随后补
L2 UDP 编码、VMess 填充、HY2 混淆/跳端口、SS 插件/UOT 版本，以及逻辑路由
嵌套 GEO/RULE-SET、Hysteria v1 UDP 子集与带宽单位修复、SSH 认证/公钥子集。
以下明确区分当前批次与历史验证，不把本地检查等同于线上 CI 或全平台实机验收。

固定内核为 sing-box **1.14.2**，revision
`af6e64c3b69e6132ebaee0e1a3d24e93903f6709`；职责和约束以
[分层契约](singbox-layers-and-fidelity.md)为准，桌面最终模型以
[订阅编排设计](desktop-subscription-orchestration.md)为准。

最近几批改动的合并视图、架构复审和下一步排序见
[2026-10-03 项目复审](project-review-2026-10-03.md)。复审发现的 **R01–R03 三项
订阅保护问题**（启动失败空缓存继续运行、异步清脏竞态、文件删除引用/落库边界）
与 R04 provider format 类型缺陷已在后续开发修复并补隔离回归，详见该文末更新。
成功迁移和本地回归不等于全平台实机或完整崩溃事务已经验收。

后续已补桌面 CI 必跑测试门禁：加入组展开实际运行回归后严格核对 15 项（已记录的
MSVC 缺陷仅豁免直接 ORM，保留 14 项），缺失 Python/固定内核/OpenSSL 或漏注册/禁用
均失败。Linux 本地 15/15 通过，门禁自身 22 场景通过；上一批实际项目 4 类缺失配置
验证已通过。本批组展开另有 8 个固定内核 API/实际 SOCKS 转发场景，11 份公开样本
编译及内核 check、Android Debug 构建通过；未执行线上 Windows/macOS CI 或手机
实机验收。实现及命令见复审文末。

## 判定口径与结论

“已接入”表示代码实现存在；“部分完成”表示仍缺字段、生命周期或产品入口；
“待实现”表示目标尚未接入；“不做”表示固定内核没有对应语义；iOS 单列为暂缓。
验证证据另分代码复核、构建、配置检查、交互/实机四种，不互相替代。

| 层 | 当前结论 | 可量化范围 | 距离完成的主要缺口 |
|---|---|---|---|
| L1 内核层 | 固定版本、资产校验、桌面/Android 控制链已接入；平台运行验收部分完成 | 官方桌面资产表 6 项；实际桌面 CI 4 项；本机验证 Linux x86_64 与 Android arm64 构建 | 其它平台实机、网络/权限/后台生命周期、故障恢复验收 |
| L2 翻译层 | 常用转换与结构化账本已接入；补齐协议子集后继续接入 file/HTTP provider、请求头、hosts 与 DNS 证书/通配，R04 已修复；整体仍部分完成 | Clash 代理协议类型 12/15；端点编辑转换 1/5；类型覆盖不表示字段完整 | provider 剩余生命周期/格式、更多 DNS/协议字段 |
| L3 产品层 | 常用客户端页面、桌面编排首阶段、订阅唤醒已接入；最终产品形态部分完成 | 桌面 7 个一级页、手机 4 个一级页；支持的原生端点编辑器仍只有 OpenVPN client | 完整编排恢复、来源/连接追溯、原生 DNS/端点/规则集编辑、跨平台交互验收 |

不计算总完成百分比：内核有某个协议、编译器能生成它、界面能编辑它、真机能稳定
运行它是四件不同的事。`unsupported` 的显式拒绝解决诊断边界，不算新增协议支持。

### 开发重心调整（2026-10-03）

按用户意图，从继续补齐所有 L2 字段转向 **L3 产品完善**。常用 L1/L2 路径已能支撑
当前客户端使用，不能据此标成全部完成；订阅保护、转换保真度与候选失败保留仍是门禁，
使用中发现的问题继续补 L1/L2，并保留上述未完成清单。

本批先完成桌面编排目标目录，后续优先来源定位/规则追溯、代理页搜索与浏览状态、
保真度明细的定位动作，再按固定内核能力建设原生 DNS/规则集/端点入口。
多订阅编排不扩展到手机；完整崩溃恢复与其它平台实机验收继续独立跟踪。

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
| 策略组 | selector/urltest、初始选择、间隔/容差、本来源 include-all-proxies 静态名称排序展开；过长间隔保留并延长 idle_timeout、记近似 | 非空筛选、代理集合/混合展开、空组回退未接入；fallback/load-balance 只能近似成 urltest；锁定、lazy/timeout 等不能伪装支持 |
| 拨号/依赖 | `dialer-proxy`→detour，组成员与 DNS resolver 共同检查循环；接口、mark、TFO、MPTCP 按平台处理 | detour 忽略物理选项、MPTCP IPv6 差异记 approx；跨来源原始代理链未开放 |
| DNS | local、UDP/TCP、DoT/DoH/DoQ/h3、bootstrap；加密 DNS 显式 skip-cert-verify；完整域名/整层 */前缀 . 与 +. 的普通与节点专用 policy；精确 hosts IP/完整列表用于 DNS、节点/端点及连接解析 | 更多 URL 参数/name-cert-verify、规则集 policy/其它模式、fallback-filter、hosts 通配/别名/系统开关、FakeIP、direct follow-policy 转换未完成；缓存/多 IP 选择、多服务器/fallback/直连解析时机有近似 |
| 普通路由 | 域名/正则、源/目标 CIDR、端口/范围、TCP/UDP、IP-VERSION、GEO、桌面进程；Linux UID、Android 包名/正则 | 平台不适用字段拒绝；Android owner 查询失败/共享 UID 的实际行为未验收 |
| 逻辑与 provider | AND/OR/NOT 有界子集，路由支持嵌套 GEO/已转换 RULE-SET；domain/ipcidr/classical inline、file、HTTP YAML/text 快照与单值 ASCII header；MRSv1 domain/ipcidr、ASN 快照；整份转换/拒绝重名 | HeadlessRule 不接受 route 的 UID/IP-VERSION/GEO/RULE-SET；HTTP 指定代理/多值或传输保留 header/MRS classical/未来版本、文件监听与其它未映射条件仍待实现 |
| 规则集下载 | 自有 GEO 清单、桌面按周刷新；Android 固定打包 CN；HTTP 直连任务下载/候选校验/缓存快照，失败留旧；file 只读 | HTTP 定时热更新、MRS classical/未来版本、文件监听、管理 UI 和多缓存崩溃事务未实现；SRS 文件头筛查不是完整解析 |
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
| 设置页 | 已恢复原布局 | 2026-10-04 单独回退 Material 3 重构，恢复通用／内核／关于与原控件；共用标题、顶部栏及侧栏间距改动保留，见 [回退与历史记录](settings-material3.md) |
| 应用模型/持久化 | 已接入，R01–R03 已修复；跨平台与崩溃恢复仍待验收 | 异步 SQLite、旧库多来源迁移重开；新增失败拒写、按版本确认、提交后引用检查清理及真实 SQLite 锁交错回归，见项目复审 |
| 分页/虚拟化 | 部分完成 | 二级 Pager 跟手、取消、反选、标签居中与有界大组已有生产回归；隐藏一级页保留 hook 状态并卸载重内容，其重新可见时的内层滚动恢复仍有已知代价 |
| 保真度呈现 | 已接入，受 L2 输出完整性限制 | 设置完整明细、用户动作后的摘要、组 `!`、CLI 明细；账本漏记时这些消费点无法自行发现问题 |
| 唯一主订阅 + Clash 次来源 | 首阶段已实现 | 主来源 Clash/原生 JSON；普通 Clash 次来源仅由启用规则参与；同名对象隔离，无引用不载入；手机保持单活动普通订阅 |
| 编排规则编辑 | 首阶段已实现 | UserOverride/SourcePolicy、priority/order、启停、Default/Group/Node、Reject/UseMain/Direct；桌面宽/窄编辑器已接入 Clash 来源的可搜索目录（含未参与来源），切源清空选择，失效旧引用保留；手机、原生连接/JSON 仍只开放默认出口 |
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

当批仍缺 ShadowTLS/Tor/Naive 的 Clash 转换、HTTP 高级下载字段/MRS 和更多 DNS/协议字段；2026-10-04 MRSv1 domain/ipcidr 增量见文首。
保真度分类与上游依据见[契约](singbox-layers-and-fidelity.md#ssh-认证与主机公钥子集2026-10-02)。

## 本批增量：本地 file rule-provider（2026-10-03）

基于 `7bbc149c81b728f9e1717440a29fff447ba13fb0`（v0.3.19）工作区，未提交、未发布；
L1 pin 与 L3 编辑器范围不变，协议类型覆盖仍为 **12/15**。

- `type: file` 的 YAML/text domain/ipcidr/classical 子集接入原生 inline rule_set，
  任意名称和逻辑 RULE-SET 引用共用现有转换与命名空间。来源整体 approx，因为
  不监听文件，不会热更新；再次应用配置才读新内容。
- `ruleProviderDir` 明确由应用数据目录传入，候选检查/启动/诊断共用根；禁止绝对
  路径、父目录跳转、越界符号链接和非普通文件，最多只读 8 MiB，不写源文件或
  纳入 GEO 下载/清理。YAML 仅接受唯一 payload 的单文档映射，text 忽略整行注释。
- 缺文件、坏 YAML、未映射字段、无效成员会让候选编译失败，保留结构化失败账本。
  同名 file/inline 无论声明顺序均拒绝，重复字段不取第一项；不提交有效前缀或换成
  同名 CN 别名。接入既有候选检查，不新增订阅数据库/schema 或修改订阅保护边界。
- 永久回归覆盖三个 behavior、两种格式、CRLF/无末尾换行、文件更新快照、路径/
  符号链接边界、整份转换失败、重复字段/来源、多文档、NUL 与大小上限。
  本地项目 `ctest` **9/9** 通过，包括多 profile 迁移后重开保护回归。
- [公开样本](examples/file-providers-2026-10-03.yaml)包含 3 个 file 集合、共 6 个
  headless 条件，另加 GEO 2 集合；编译及固定 sing-box 1.14.2 `check` 通过。
  原 12 协议、SSH、HY1、SS 插件和逻辑资源五份样本再次通过编译及内核 check。
- 三份私有订阅的独占临时副本保留 **32/32、30/30、34/34** 节点并通过 check，
  源文件摘要未变，临时目录已清理。只替换 rules、去除 rule-providers，不验证原
  订阅完整规则或远端握手，不入库私有凭据。

复现公开样本：

```bash
cmake --build build --target clash-flux test_singbox
./run.sh --version
ctest --test-dir build --output-on-failure \
  -R '^(smoke|profile_links|page_transition|singbox|vpn|routing|compensation|sqlite_orm|persistence)$'
build/test_singbox docs/examples/file-providers-2026-10-03.yaml \
  platform/android/app/build/generated/rule-set-assets/rules \
  /tmp/clash-flux-l2-file-audit.json
build/engines/sing-box check -c /tmp/clash-flux-l2-file-audit.json
git diff --check
```

以上本地检查通过，`run.sh --version` 返回 v0.3.19。Android 使用上一节相同环境
运行 `:app:assembleDebug --offline --no-daemon`，日志为
`/tmp/clash-flux-l2-file-android-build.log`。本批未新增运行中坏文件回滚的实机验收，
未验证真实 DNS/分流流量或 Android 私有目录文件管理。该 file 批次尚无 HTTP 下载，
后续直连子集见下一节；MRS、文件监听和规则集 UI 仍待实现，不能据此宣称完整生命周期。
保真度分类、路径约定与上游依据见[分层契约](singbox-layers-and-fidelity.md)。

## 本批后续增量：HTTP provider 下载与缓存（2026-10-03）

仍基于 v0.3.19 / `7bbc149c81b728f9e1717440a29fff447ba13fb0` 加未提交增量。
版本与内核 pin 不变，L2 协议仍为 **12/15**，L3 未新增规则集管理入口。

- HTTP(S) YAML/text domain/ipcidr/classical 子集支持直连或 `proxy: DIRECT`；
  `httpRuleProviders` 清单与 GEO 分开，编译器不联网。任务层下载到独占目录，
  全部规则转换、策略目标检查与桌面 check 后才提交自有缓存。
- 缓存核对完整来源身份和版本，缺失/坏内容不触发删除或重建订阅；无可用内容
  就拒绝候选，已有验证内容时坏更新/断网沿用旧值。运行 options 持有 raw 快照，
  新缓存不覆盖旧计划，回滚不重新下载。不修改数据库/schema/用户订阅文件。
- 来源整体 approx：默认下载直连、path 只读种子、自有缓存布局、默认 8 MiB 上限、
  size-limit 超限拒绝而非截断、interval 仅在应用配置时检查，未接入定时热更新。
  header、其它指定 proxy、MRS/未映射字段拒绝整份，不静默改成可跑的下载。
- 原子替换复用 `api::CommitFile`；默认验证证书。Android 使用有界 Java 下载桥接
  和显式直连参数，普通订阅默认参数保留；后台 libbox 原生核验边界不变。
- 永久回归覆盖首次准备、缓存重读、过期断网/坏更新留旧、完整候选失败零提交、
  多来源部分转换失败零提交、来源身份隔离、future version/身份冲突、限额、只读
  种子、raw 快照回滚、任务异常与独占目录清理。
- 新增 `rule_provider_download` 真实 curl 回归 **11 个场景**：正常下载、整份规则
  失败、200 错误页、已知/未知长度超限、截断响应、503、非 HTTP 重定向、正常
  重定向、断网、自签证书默认拒绝；请求失败时两个稳定来源文件保持原样。
  本地项目测试 **10/10** 通过（含多 profile 迁移后重开保护回归）。
- [HTTP 离线样本](examples/http-providers-2026-10-03.yaml)采用 3 个只读种子、独占
  临时缓存、禁用网络，生成 3 个集合/6 个 headless 条件；固定 1.14.2 check 通过。
  另外六份协议/SSH/HY1/SS/逻辑/file 样本再次通过编译与内核检查。
- 三份规范化私有订阅副本保留 32/32、30/30、34/34 节点并通过 check，源文件
  摘要不变，临时目录清理；只替换 rules/去除 provider，不验证原订阅全部规则或握手。

实际构建与检查：

```bash
cmake --build build --target clash-flux test_singbox test_rule_provider_download
./run.sh --version
ctest --test-dir build --output-on-failure \
  -R '^(smoke|profile_links|page_transition|singbox|rule_provider_download|vpn|routing|compensation|sqlite_orm|persistence)$'
build/test_singbox docs/examples/http-providers-2026-10-03.yaml \
  platform/android/app/build/generated/rule-set-assets/rules \
  /tmp/clash-flux-l2-http-audit.json
build/engines/sing-box check -c /tmp/clash-flux-l2-http-audit.json
git diff --check
```

上述检查通过，版本输出 v0.3.19；Android 同环境 `:app:assembleDebug --offline --no-daemon`
通过，日志 `/tmp/clash-flux-l2-http-android-build.log`。没有新增运行内核真实分流、
手机下载/TLS/断网恢复实机验收；多文件缓存提交中途故障没有统一崩溃事务，手机异步
原生失败恢复仍不完整。不能据此宣称 provider 生命周期或 L2 全部完成。本批未发布。

## 本批后续增量：HTTP 请求头（2026-10-03）

仍为 `7bbc149c81b728f9e1717440a29fff447ba13fb0` / v0.3.19 加未提交增量；
L2 协议覆盖保持 **12/15**，本轮推进 provider 字段，未新增 L3 UI。

- `header` 支持普通单值 ASCII 数组映射，如 `User-Agent: [my-client/1]`、
  Authorization、X-Token；保留原值和内部空格，总量上限 16 KiB。异常类型、多值、
  空值、大小写无关重名、控制字符、非 ASCII、首尾空格和传输保留字段整份拒绝。
- 桌面 curl 与 Android JNI/Java 传递同一编译器资源清单；系统证书验证保持开启。
  带头下载不跟随重定向，单独 Field/Approx 记账；无头下载的重定向行为不变。
- 非空 header 的规范化名称与原值进入完整缓存身份；更换鉴权不能复用旧缓存或
  raw pins，失败保留原缓存/调用方计划。空 header 与未声明保持原缓存身份，
  头名称大小写变化保持稳定。请求头值不写进原生 JSON 或保真度/错误诊断。
- `rule_provider_download` 扩至 **15 个真实 curl 场景**，新增实际 UA/鉴权/自定义头
  检查、无头访问鉴权端点失败、非法头联网前拒绝、带头重定向目标零访问；两个
  稳定来源文件和独占暂存清理均断言。编译器回归另覆盖身份变化、失败留旧与快照回滚。
- 本地 **10/10 项项目测试**、**7 份公开样本编译与固定 1.14.2 check**、Android
  `:app:assembleDebug --offline --no-daemon` 通过。请求头样本采用只读种子、禁用网络，
  真实 HTTP 验证来自独立 loopback 回归。没有声称 Android 请求头实机下载或分流验收。

实际执行：

```bash
cmake --build build --target clash-flux test_singbox test_rule_provider_download
./run.sh --version
ctest --test-dir build --output-on-failure \
  -R '^(smoke|profile_links|page_transition|singbox|rule_provider_download|vpn|routing|compensation|sqlite_orm|persistence)$'
# 七份 docs/examples 样本各用 build/test_singbox 输出到独占临时目录，随后：
build/engines/sing-box check -c <临时配置.json>
# platform/android，环境与前批相同：
./gradlew :app:assembleDebug --offline --no-daemon
git diff --check
```

版本验证输出 v0.3.19。构建日志 `/tmp/clash-flux-l2-headers-build.log` 与
`/tmp/clash-flux-l2-headers-android-build.log`。HTTP 指定代理、多值/传输保留头、
MRS、定时热更新与多文件崩溃事务仍未完成。iOS 保持暂缓，明确拒绝非空 header。
本轮未修改数据库/schema/用户订阅文件，未提交或发布。

## 本批后续增量：精确 hosts 与内核运行回归（2026-10-03）

代码快照仍为 v0.3.19 / `7bbc149c81b728f9e1717440a29fff447ba13fb0` 加未提交增量。
L2 协议覆盖保持 **12/15**；本轮补 DNS 字段，不新增 L3 编辑界面。

- 接入精确 hosts 的 IPv4/IPv6 scalar 与完整列表，域名大小写/尾点规范化，规范化
  重名全部拒绝；异常条目按整条 unsupported 记账，保留独立合法映射。通配、别名、
  lan、CIDR、带点分 IPv4 的 IPv6、系统 hosts 开关仍未转换。
- 原生 hosts DNS 用于 A/AAAA 应答（优先于 policy、TTL=10）、节点与 DNS 端点
  domain_resolver、主连接前置 resolve，保留原 server/SNI。false/no/0 的 use-hosts
  或 dns.enable:false 只关应答映射，全局连接解析仍有效。次来源只保留解析依赖。
- 真实内核回归发现普通解析覆盖 hosts 的问题：显式路径绕过 DNS 缓存，普通
  resolve 排除已映射域名；命名空间化后同步 hosts DNS tag，避免后置规则引用旧名。
  多 IP 选择及缓存路径差异记 approx；不冒充全部 hosts 语义保真。
- 新增 `dns_hosts_runtime`：生产编译器（含主来源 ID）+ 固定官方 1.14.2 在独占临时
  目录和 loopback DNS/HTTP/SOCKS 服务运行，**12 场景**实际验证 A/AAAA/多地址、
  缺少对应地址族的空应答、TTL、大小写、非地址查询、子域不扩大匹配、直连、
  节点/DNS 端点解析，以及关闭 hosts DNS 应答、普通 DNS 缓存已存在时连接仍走映射。
- 永久编译回归覆盖非法值/列表原子性、重复域名、布尔关闭/异常、未匹配节点保持
  原解析器、TLS 身份、前置顺序、次来源隔离和命名空间引用。
- **11/11 项项目 CTest** 通过，其中下载回归仍为 15 场景；**8 份公开配置**编译与
  固定内核 check 通过。三份规范化私有订阅副本保留 **32/32、30/30、34/34** 节点，
  通过 check，源摘要不变，临时目录清理；只替换规则/去 provider，不验证原订阅所有
  分流规则或远端握手。Android Debug 构建通过，不代表 Android hosts 实机流量验收。

实际执行：

```bash
cmake --build build --target clash-flux test_singbox test_rule_provider_download
./run.sh --version
ctest --test-dir build --output-on-failure \
  -R '^(smoke|profile_links|page_transition|singbox|rule_provider_download|dns_hosts_runtime|vpn|routing|compensation|sqlite_orm|persistence)$'
build/test_singbox --compile-config docs/examples/dns-hosts-2026-10-03.yaml <临时配置.json>
build/engines/sing-box check -c <临时配置.json>
# 另七份公开样本及三份私有副本使用原有 test_singbox <输入> <GEO目录> <临时输出> 路径
# platform/android，JAVA_HOME / ANDROID_HOME / HUXERUI_HOME 与前批相同：
./gradlew :app:assembleDebug --offline --no-daemon
git diff --check
```

版本输出 v0.3.19；日志 `/tmp/clash-flux-l2-hosts-build.log` 与
`/tmp/clash-flux-l2-hosts-android-build.log`。受控 Linux DNS/连接运行回归通过，
没有新增公网/TUN/Android 实机验收；未改数据库/schema/真实订阅文件，未提交或发布。

## 本批后续增量：加密 DNS 证书参数（2026-10-03）

代码快照为 v0.3.19 / `7bbc149c81b728f9e1717440a29fff447ba13fb0` 加上述未提交增量。
L2 协议覆盖保持 **12/15**；整体仍部分完成，不增加 L3 设置界面。

- DoT/DoH/DoQ、HTTPS 强制 H3 支持显式 skip-cert-verify=true/false；exact 映射
  单端点 tls.insecure，缺省保持证书验证，保留 server/SNI。支持 h3/出站组合，
  不改变 bootstrap/其它服务器或订阅下载的证书设置。
- 非法布尔、重复参数、未知组合与非加密传输整台拒绝并记 unsupported，不保留
  有效参数的前缀。修复非 HTTPS 的 h3=false 被忽略；name-cert-verify 继续拒绝，
  不能用会修改 SNI 的 server_name 代替仅校验证书名称。
- 编译回归覆盖传输/布尔值、缺省、端口/路径/H3/detour 组合、非法值与命名空间，
  并检查 policy、节点 resolver 和显式 IP bootstrap 的端点级隔离。
- 新增 `dns_tls_runtime`：生产编译器 + 固定 1.14.2 + 独占临时证书/loopback 服务，
  **12 个实际 DoT/DoH 场景**。默认与 false 拒绝不受信任证书且没有 DNS payload；
  true 成功。临时信任根仅在测试配置注入，名称正确时 false 成功，IP 名称不匹配
  时 false 仍失败；SNI、HTTP Host 和自定义路径保持原值。拒绝场景同时断言服务器
  收到证书类 TLS alert，不把任意查询超时算为证书校验成功。
- **12/12 项项目 CTest** 通过；保留 hosts 12 场景、curl 下载 15 场景。
  **9 份公开配置**编译与固定内核 check 通过。三份只读私有订阅的规范化临时副本
  保留 **32/32、30/30、34/34** 节点并通过 check，源摘要不变；只替换规则/去除
  provider，不能据此声称原订阅全部规则与远端握手通过。Android Debug 构建通过。

实际执行：

```bash
cmake --build build --target clash-flux test_singbox test_rule_provider_download
./run.sh --version
ctest --test-dir build --output-on-failure \
  -R '^(smoke|profile_links|page_transition|singbox|rule_provider_download|dns_hosts_runtime|dns_tls_runtime|vpn|routing|compensation|sqlite_orm|persistence)$'
build/test_singbox --compile-config docs/examples/dns-tls-parameters-2026-10-03.yaml <临时配置.json>
build/engines/sing-box check -c <临时配置.json>
# 其它八份公开样本与三份私有规范化副本也各自编译/check；临时目录自动清理。
# platform/android，JAVA_HOME / ANDROID_HOME / HUXERUI_HOME 同前批：
./gradlew :app:assembleDebug --offline --no-daemon
git diff --check
```

版本输出 v0.3.19；日志 `/tmp/clash-flux-l2-dns-tls-build.log` 与
`/tmp/clash-flux-l2-dns-tls-android-build.log`。CMake 的 TLS 运行回归要求 Python、
固定打包内核与 OpenSSL CLI，缺少 CLI 会明确报告未注册；本机已满足前提并实际执行。
没有 DoQ/H3 实际握手、公网/TUN/Android 真机流量验收；未改数据库/schema/真实
订阅文件，未提交或发布。证书参数边界已同步到 AGENTS.md 和保真度基线。

## 本批后续增量：DNS 策略通配（2026-10-03）

代码快照仍为 v0.3.19 / `7bbc149c81b728f9e1717440a29fff447ba13fb0` 加未提交增量。
L2 协议覆盖保持 **12/15**，整体仍部分完成；本批补翻译语义，不新增 L3 编辑器。

- nameserver-policy 与节点专用 proxy-server-nameserver-policy 共用完整 ASCII 域名、
  整层 *、前缀 . 与 +.。* 每次只匹配一个非空标签，. 匹配子域并排除根域，+.
  展开为独立根/子域分支；支持中间/连续 * 及前缀组合。
- 从右向左按固定标签 > * > . 排序，保持 Clash trie 选择；后声明只覆盖同一
  规范化分支，Exact 明细记录覆盖。不能用字符串总长度或全局「精确优先」替代。
- 普通 DNS 发出锚定原生 regex，节点编译期选择直接用同一模式元数据，不用另一个
  regex 引擎重解释、不把元数据写入 JSON。合法域名下划线已纳入 DNS 依赖，非法
  节点名称不会被通配选中；未知 geosite:/rule-set:、部分标签通配和非法模式整条记账。
- 永久编译回归覆盖正反声明次序、普通/节点专用策略、规范化覆盖、各层/根域边界、
  不扩大匹配、非法值与模式注入、wildcard 所选 resolver 与 detour 的循环失败。
- 新增 `dns_policy_runtime`，生产编译器（含主来源 ID）+ 固定官方 1.14.2 + 独占
  loopback DNS/SOCKS 服务，实际验证 **63 场景**。按真正收到请求的 DNS 服务断言
  选择，覆盖固定标签优先、根域/单层/多层、大小写/下划线、中间/连续通配及前缀
  组合；两次内核分别开启/关闭节点专用策略，普通查询保持一致，实际 SOCKS 节点
  连接改用对应 resolver；普通 DNS 已缓存节点同名应答时仍查询专用 resolver。
  不是只看生成 JSON 的字符串。
- **13/13 项项目 CTest**、**10 份公开配置**编译/固定内核 check、Android Debug
  构建通过。三份私有订阅规范化临时副本保留 **32/32、30/30、34/34** 节点，通过
  check，源摘要不变；只替换规则/去除 provider，不验证原规则全部语义或远程握手。

实际执行：

```bash
cmake --build build --target clash-flux test_singbox test_rule_provider_download
./run.sh --version
ctest --test-dir build --output-on-failure \
  -R '^(smoke|profile_links|page_transition|singbox|rule_provider_download|dns_hosts_runtime|dns_policy_runtime|dns_tls_runtime|vpn|routing|compensation|sqlite_orm|persistence)$'
build/test_singbox --compile-config docs/examples/dns-wildcard-policy-2026-10-03.yaml <临时配置.json>
build/engines/sing-box check -c <临时配置.json>
# 另九份公开样本及三份私有规范化副本各自编译/check，独占临时目录自动清理。
# platform/android，JAVA_HOME / ANDROID_HOME / HUXERUI_HOME 同前批：
./gradlew :app:assembleDebug --offline --no-daemon
git diff --check
```

版本输出 v0.3.19；日志 `/tmp/clash-flux-l2-dns-wildcards-build.log` 与
`/tmp/clash-flux-l2-dns-wildcards-android-build.log`。未新增公网/TUN/Android 真机
DNS 流量验收；hosts 通配/别名、DNS 规则集策略、fallback-filter/FakeIP/direct
follow-policy 仍未完成。未改数据库/schema/真实订阅文件，未提交或发布；约束已同步 AGENTS.md。

## L3 目标目录首批交付（2026-10-03）

- 桌面宽弹窗与窄窗口独立编辑页复用 `RuleTargetPicker`。先选来源，再选默认出口、
  策略组或节点；搜索为原名的字面子串匹配，每次最多展示 200 项并保留当前有效选择。
- 目录只读订阅文件（上限 8 MiB、限制在订阅根），复用来源导出编译；未参与编排的
  Clash 来源也可读取。不请求 provider、不生成候选文件、不发布运行/预览映射。
  读取/编译在任务线程，写 State 回 UI；用请求序号、来源身份与持久层修订号丢弃过期结果。
- 来源/类型改变后清空对象；搜索变化不改引用。失效旧名称保留且不自动选择第一项，
  原样引用可继续编辑不可用策略或禁用；新引用必须来自当前目录，最终保存仍走领域候选检查。
  对象身份仍为 source ID + kind + 原始唯一名称，没有对象 UUID 或改名追踪。
- 手机、PPTP/OpenVPN 和原生 JSON 保留默认出口。JSON 次来源编排仍不支持；目录不是
  内核运行状态或远端握手验收。读取失败保留订阅文件和数据库记录。
- 实际验证：Linux `cmake --build build --target clash-flux test_singbox test_page_transition`
  （含 UI codegen）、`./run.sh --version`（0.3.19）通过；`ctest --test-dir build
  --output-on-failure --no-tests=error -L clashflux-required` **15/15** 通过。
  singbox 增加未参与来源、同名隔离、失败不发布部分目录与不准备 provider 的回归；
  page_transition 增加生产下拉/文字输入、搜索超出 200 项、来源切换、失效保留、默认出口
  与类型/来源校验。异步结果隔离为代码复核，未声称完成整页异步乱序实机验收。
- Android `:app:assembleDebug --offline --no-daemon` 通过；只构建，未安装/真机验证。
  Windows/macOS 本批未执行；无本批真实 GUI/TUN/远端线路验收；`git diff --check` 通过。
  未提交、未发布。构建/测试日志前缀 `/tmp/clash-flux-l3-target-picker-`。

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
| P1 | 扩展订阅保护跨平台/崩溃验收 | R01–R03 已修复并通过本地失败/交错/重开回归；继续 Windows/macOS/Android 失败启动与生命周期验证，不把它当作完整 journal |
| P1 | 继续清点输入类型/默认语义 | R04 已修复；继续逐字段覆盖 DNS/协议/组输入类型、默认值及拒绝策略，补永久回归和固定内核检查 |
| P1 | 补桌面计划与手机生命周期验收 | 切主、启停规则、刷新/删除、失败回滚、进程重建有可重复场景；Windows/macOS/Android 各自留实际记录 |
| P1 | 外部 provider 与原生规则集 | 下载来源、原子提交、格式转换、顺序、失败留旧、刷新/删除均有定义与测试；再开放管理入口 |
| P1 | 完成桌面编排最终形态 | JSON 次来源、跨来源链、对象目录/身份迁移、rule ID 追溯、持久恢复 journal；完整验收，不扩展手机普通多订阅 |
| P2 | 原生能力产品化 | 优先原生 DNS/高级拨号和新的 endpoint；每个连接对象包含字段、状态、路由、启停与恢复，不以 JSON 可导入冒充编辑器完成 |

`urltest` 手动锁定、原生 fallback/load-balance、真实 providers 面板等固定内核没有的
能力归为“不做”，不放进待修 bug 或待完成百分比。iOS 是否恢复支持由后续项目决策
决定，本清单不产生 iOS 发布承诺。


## 2026-10-09：Windows 原生服务分离

代码快照：v0.3.29 发布候选，包含当前全部应用改动，排除 traces 调试文件。Windows 新增 SCM
`ClashFluxService` / LocalSystem 服务，通过受限命名管道控制 sing-box、PPTP/RAS
与 TUN 路由补偿；GUI/CLI 保留数据库、订阅和当前用户系统代理，TUN 不提权 GUI。
服务管理走 UAC 子命令、原始用户 SID 授权、受保护 Program Files 载荷与版本门禁；
正常更新失败尝试恢复旧载荷，不承诺完整多文件崩溃事务。详细契约见
[Windows 服务](windows-service.md)。

新增 `service_protocol`（所有桌面）与 `windows_service`（Windows 管理员环境）
回归，项目门禁现在为通用桌面 16 项、Windows 17 项；MSVC C4737 仅豁免直接 ORM，
Windows 仍必跑 16 项。原生服务测试使用隔离服务和假内核，不代替真实 VPN 流量验收。

本批实际验证：

- `cmake --build build --target clash-flux`（包含 HuxerUI codegen）与
  `./run.sh --version` 成功；发布候选版本为 0.3.29。
- 构建 `test_service_protocol`、`test_vpn`、`test_compensation`、`test_pptp_connect`
  及其余项目回归目标；`ctest --test-dir build --output-on-failure --no-tests=error
  -L clashflux-required`：16/16 通过。
- LLVM-MinGW 20261006：原生 `windows_service.cpp` 和 Windows SCM 测试程序以
  `-std=c++20 -Wall -Wextra -Werror` 编译、链接为 PE 成功；假内核 PE 编译成功。
  Windows 的 config/vpn/pptp/vpn_compensation/service/singbox/core 接口，以及
  `pptp.cpp` / `core.cpp` 实现交叉编译成功。
- 扩大到原有 `vpn_compensation_windows.cpp` 时，MinGW 的 `windns.h` 缺少
  `DNS_ADDR_ARRAY`、`DNS_QUERY_REQUEST`、`DnsQueryEx` 声明而失败；本批不改 DNS
  后端来规避 SDK 差异。因此不能宣称完整 Windows 构建已通过。
- `git diff --check` 通过。

完整发布候选的 [GitHub CI](https://github.com/FarnaHerry/Clash-Flux/actions/runs/37951900040)
已通过 Windows/MSVC Release 构建及全部项目回归（含原生 SCM 服务），Linux x64、
Linux ARM64、macOS ARM64 与 Android 也通过。标签发布再执行门禁及 WiX 安装器构建。
iOS 暂缓路径的现有 Xcode 工程解析失败仍为非阻塞，不生成 iOS 发布包。

未新增 Windows GUI/UAC/TUN 数据流/PPTP 实机验收；本机未安装真实服务，未更改
当前机器代理、数据库与订阅。
