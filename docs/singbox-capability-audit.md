# sing-box 能力接入审查

审查日期：2026-10-01。基线为项目固定的 sing-box **1.14.2**，revision
`af6e64c3b69e6132ebaee0e1a3d24e93903f6709`，本次与官方最新稳定版对齐；不跟随 1.15 alpha。
桌面打包二进制的 `version` 输出已核对；Android/libbox 使用同一固定 revision，但
不同平台的编译标签与系统接口仍须分别验证。历史 1.14.0 检查结果保留在下方，
本次升级的检查见 1.14.2 升级文档。分层与保真度规则以
[契约文档](singbox-layers-and-fidelity.md) 为准。

本文保留 2026-10-01 的开发和验证历史；2026-10-03 当前工作区的逐层完成度、
语义探针及未关闭问题见 [L1 / L2 / L3 开发复核](l1-l2-l3-status.md)。
最近增量的统一整理与新发现的订阅保护/输入类型缺陷见
[2026-10-03 项目复审](project-review-2026-10-03.md)；R01–R04 后续已修复并补本地
失败/交错/重开回归，跨平台实机与完整崩溃事务仍待验收。

## 结论与计数口径

内核控制已经覆盖日常使用：启停、模式、TUN、代理组选择、测速、连接与日志。
主要缺口是 **Clash 输入转换和原生能力的编辑入口**，尤其是 DNS 策略、通用规则集、
高级路由、更多拨号字段及端点。现阶段不能把原生 JSON 能导入等同于完整产品支持。

早期 Clash YAML 覆盖 8 个代理协议类型，首轮增加到 10/15；当前工作区接入
Hysteria v1 UDP 子集与 SSH 认证/公钥子集后为 **12/15**。该数字只统计
固定内核的代理协议家族，不含 direct/bridge/block/selector/urltest，也不代表每个
家族所有字段都已支持。原文的 8/16 把已经移除的 WireGuard 出站计入了分母，本轮纠正。
各领域投入、平台限制和使用价值不同，不用一个“总开发百分比”概括。

| 领域 | 当前应用接入 | 尚未充分接入的内核能力 |
|---|---|---|
| 代理协议 | Clash 转换 12/15：SS、VMess、VLESS、Trojan、Hysteria v1 UDP 子集、Hysteria2、TUIC、HTTP、SOCKS、AnyTLS、Snell v4、SSH 认证/公钥子集；SS 内置 simple-obfs/v2ray-plugin 子集 | ShadowTLS、Tor、Naive 的 Clash 转换；SSH 外部私钥路径及更多字段；HY1 单边窗口/快速打开/非 UDP 伪装/兼容速率字段、其它 SS 插件、插件 headers/证书/ECH 与更多 TLS/拨号字段 |
| 策略组 | selector/urltest 两种原生组；原生测速容差、间隔；detour 代理链、selector 初始选择 | idle_timeout 和 interrupt_exist_connections 的专门设置入口；不能模拟 fallback/load-balance 或 urltest 手动锁定 |
| 路由 | 域名、目标/源 CIDR、目标/源端口及范围、TCP/UDP、IP-VERSION、地理规则集、桌面进程路径、Linux UID、Android 包名/正则、AND/OR/NOT（含嵌套 GEO/已转换 RULE-SET） | 更多逻辑条件、外部 provider、应用选择入口、Wi-Fi/接口等原生网络匹配、更多原生路由动作 |
| DNS | local、UDP/TCP/DoT/DoH/DoQ/强制 HTTP3；加密端点显式 skip-cert-verify；完整域名/整层 */前缀 . 与 +. policy；节点专用 proxy-server-nameserver-policy；bootstrap、节点解析器与依赖图检查；精确 hosts IP/列表用于 DNS、节点/端点及连接解析 | 更多 URL 参数/name-cert-verify、规则集策略/其它模式、fallback-filter、hosts 通配/别名/系统开关、FakeIP、direct follow-policy、缓存及原生设置入口；多 DNS/fallback/多 IP hosts 选择有近似 |
| 端点 | 原生连接编辑器支持 openvpn-client（1/5）；PPTP 是系统补充路径，不计入内核端点 | WireGuard、Tailscale、OpenConnect、OpenVPN server 的连接对象、字段编辑及生命周期管理 |
| 规则集 | 自有 GEO 显式资源清单、桌面按周刷新/失败留旧、国内别名及 inline；file YAML/text 只读快照、HTTP 直连任务下载/校验/缓存快照与单值 ASCII header 子集 | HTTP 指定代理/多值或传输保留 header/MRS、定时热更新、文件监听、local/remote rule_set 管理界面 |
| 控制与可见性 | 桌面 clash_api / Android CommandClient；保真度报告与 CLI | 独立 DNS 诊断、更多内核状态展示；不依赖空壳 providers API 或不存在的 selectable/testUrl 字段 |

原生 JSON 路径会保留 outbounds、endpoints、DNS 和路由等配置，并合并托管项。
应用仍管理日志、控制 API、缓存启用、混合入站及 TUN；例如关掉 TUN 时会删除其入站，
Linux 托管 TUN 会关闭 auto_redirect。它不是原样执行任意 JSON 的通道。保留字段也
不保证对应功能在每个平台有编译支持、权限或完整状态展示。

## 本轮已接入

### file rule-provider（2026-10-03 工作区增量）

- 本地 YAML/text 与 inline 共用 domain/ipcidr/classical 原子转换；路径相对应用数据
  目录、根内校验、只读最多 8 MiB，不纳入 GEO 下载/清理资源。
- 来源整体记 approx：生成原生 inline 快照，缺少 Mihomo 文件监听，重新应用才读新文件。
- 缺文件、越界、坏 YAML、无效成员、未映射字段让候选编译失败并保留账本，
  不提交部分规则或替换成同名 CN 别名；MRS 与文件管理 UI 尚未接入。
- 上游依据、样本和验证范围见[分层契约](singbox-layers-and-fidelity.md)及
  [L2 完成度](l1-l2-l3-status.md)。协议计数仍为 12/15。

### 精确 hosts（2026-10-03 后续工作区增量）

- 精确域名到单 IP/完整 IP 数组用于原生 hosts DNS、节点/DNS 端点和主连接解析；
  保留节点原 server/SNI。A/AAAA DNS rule 优先于 policy，TTL=10；非地址查询不拦截。
- use-hosts/enable 关闭 DNS 应答映射，不关闭全局连接 hosts；显式路径绕过 DNS 缓存，
  普通 resolve 排除映射域名，来源 tag 随命名空间同步。次来源只导出所需解析依赖。
- 异常条目整条 unsupported，独立合法映射继续；规范化重名全部拒绝。通配、别名、
  lan、CIDR、带点分 IPv4 的 IPv6、系统 hosts 开关仍未接入。整体缓存/多 IP 选择记 approx。
- `dns_hosts_runtime` 用生产编译器与固定 1.14.2 在 loopback 实际验证 DNS、直连、
  SOCKS 节点和 DNS 端点解析，共 12 个场景；不是公网或 Android 实机验收。

### 加密 DNS 证书参数（2026-10-03 后续工作区增量）

- DoT/DoH/DoQ 与 HTTPS 强制 H3 接入显式 skip-cert-verify=true/false，端点级 exact；
  保留原地址、SNI 和默认证书验证，可组合 h3 与已编译出站。非法/重复/未知组合
  整台服务器 unsupported；非 HTTPS 的 h3=false 也不再静默忽略。
- name-cert-verify 仍拒绝，不能用修改 SNI 的 server_name 代替其仅校验证书名的语义。
- 新增 `dns_tls_runtime`，固定官方内核与 loopback DoT/DoH 服务共 12 场景，验证
  证书信任、显式 true/false、SNI/HTTP Host/路径和独立的名称校验；临时信任根仅测试注入。
  DoQ/H3 只有配置 check，Android 只有本地构建，不算 QUIC/真机流量验收。
- 样本、分类、上游依据与检查命令见[分层契约](singbox-layers-and-fidelity.md#43a-dns2026-10-03-更新)
  与[完成度记录](l1-l2-l3-status.md)。协议类型覆盖仍为 12/15。

### DNS 策略通配（2026-10-03 后续工作区增量）

- 普通与节点专用 policy 共用 ASCII 完整域名/整层 */前缀 . 与 +. 模式；每个 *
  只匹配一层，. 排除根域，+. 拆为独立根/子域分支，保持 trie 从右向左的标签优先级。
  中间/连续 * 及与前缀组合已接入，不是把所有形式宽化为 domain_suffix。
- 后声明覆盖同一规范化分支，Exact 账本追踪覆盖。节点选择使用同一模式元数据，
  原生 DNS 发出锚定 regex；元数据不入 JSON。节点域名下划线已进入 DNS 依赖。
- 永久回归验证声明顺序、覆盖与边界、拒绝非法模式/regex 注入和 DNS/detour 循环。
  `dns_policy_runtime` 在固定官方内核实际验证 **63 场景**，包括实际 SOCKS 节点连接
  与普通/节点专用 resolver 隔离、同名普通 DNS 缓存不替代节点策略；Android 仅构建，未验收真机流量。
- geosite:/rule-set:、更多策略/URL 参数、hosts 通配/别名仍未接入；协议覆盖仍 12/15。
  分类、样本、依据与命令见[分层契约](singbox-layers-and-fidelity.md)和[完成度记录](l1-l2-l3-status.md)。

### HTTP rule-provider（2026-10-03 后续工作区增量）

- YAML/text 的三个 behavior 复用整份转换器；编译器输出显式资源清单，任务线程
  直连下载、默认验证证书，独占暂存全部内容，候选编译/目标/桌面 check 后原子提交。
- 自有缓存核对完整身份/版本，失败保留旧内容，计划持有 raw 快照，回滚不重下载。
  `path` 只作只读种子，`interval` 在应用配置时检查，缺少定时热更新等差异记 approx。
- 默认 8 MiB 或更小 size-limit 超限拒绝；Android 走有界 Java TLS 桥接，不用无 TLS
  curl。后续接入单值 ASCII header，值纳入缓存身份；带头下载拒绝重定向并记 approx。
  指定代理/多值或传输保留 header/MRS 仍拒绝，不宣称多文件崩溃事务或实机异步恢复已完成。

### AnyTLS

- `password`、必需的 TLS、SNI、ALPN、uTLS 指纹；默认保持证书校验。
- `idle-session-check-interval` / `idle-session-timeout`：整数秒数转原生 duration。
- `min-idle-session`：非负数量；`client-metadata` 原样传入，包括空字符串。
- 使用字段白名单。组合 ShadowTLS/Restls/JLS、证书约束、ECH 等未映射字段
  会使整条节点被拒绝并记账，避免连接线路悄悄改变。
- 内核 AnyTLS 固定支持 TCP/UDP，没有出站 `network` 开关。订阅没有启用 `udp`
  时会明确记为 approx；如需禁止 UDP，应显式写路由规则。

### Snell v4

- 只接受显式 `version: 4`；PSK、reuse、UDP 开关和 http/tls obfs 直接映射。
- 未开启 UDP 时以原生 `network: tcp` 限制；obfs 未指定 host 时保留 Clash 的
  `bing.com` 默认值。
- 旧版本、缺省版本、v5 或组合伪装不自动替换。固定内核接受 v4/v6，v6 可通过
  原生 JSON 使用，本轮没有把它当成 Clash v5 的替代品。

### 路由与测速

- `include-all-proxies` 已接入静态来源子集：本来源成功节点按名称排序追加，显式成员
  在前；跨来源分开展开后再命名空间化。非空筛选与代理集合组合拒绝候选，空组不
  隐式补 DIRECT；详见[保真度基线](singbox-layers-and-fidelity.md)。

- `SRC-IP-CIDR` 支持 IPv4/IPv6；`SRC-PORT`、`DST-PORT` 支持单端口、`/` 列表和
  `-` 范围，转换成原生的端口数组与 `:` 范围。混合列表保留 OR 匹配。
- `NETWORK,TCP/UDP` 归一为小写内核值；规则顺序不变。
- 非法新匹配条件整条跳过并记 unsupported，不把有效的半条条件留下来。
- `tolerance` 的 1–65535ms 原样传入。显式 0 或无效值记 approx，使用内核默认 50ms。
- `interval > 1800s` 保留原间隔，把 `idle_timeout` 延长到该值，记 approx 说明空闲
  巡检时间变化；无效间隔记账并使用内核默认值，不替换测速 URL。
- Android 仅测速实例仍使用 selector，避免自动巡检与手动测速重复。
- 原生 JSON 的 `cache_file` 改为只管理 `enabled`，保留 `path`、`cache_id`、FakeIP/RDRC
  持久化等用户字段，避免整对象覆盖导致高级缓存配置丢失。

可导入的字段示例见 [kernel-capabilities.yaml](examples/kernel-capabilities.yaml)。服务器
和密码均为占位值，须替换后才能实际连接。

## 后续 DNS 适配（同日继续开发）

- `nameserver-policy` 的完整域名与 `+.域名` 转为原生 DNS route rules，按精确匹配和
  后缀长度安排优先级；不要求必须存在 nameserver。其它通配/规则集键不扩大匹配范围。
- HTTPS `#h3=true` 转为内核实际的 `h3` 类型，支持与已编译的出站绑定组合；`system`
  使用 local。节点在 DNS 之后声明也能绑定，未知出站不静默改直连。
- 修正 DoT/DoQ 的端口比较逻辑：默认 853，保留显式 443/53 等非默认端口；裸地址
  支持端口，非法端口拒绝。删除 `http://` 自动改成 HTTPS 的语义改写。
- 多服务器不等于原生故障切换：默认和策略使用首个可转换服务器，记 approx。
  fallback、未映射 DNS 字段、hosts、未知附加参数均进入账本。DNS approx 摘要单独计数。
- 手机共享这些编译器改进；不增加手机多订阅编排。桌面最终模型见
  [编排设计](desktop-subscription-orchestration.md)，官方交互见
  [GUI 研究](singbox-official-gui-review.md)。两份文档明确区分规划与实际实现。

## 稳定版升级与后续核心适配

固定版本升级到 1.14.2，同时补上桌面缓存隔离及 Android AAR 来源检查。
`dialer-proxy` 转 native detour，出站图支持向后引用并拒绝缺失目标和循环；
selector 可配置 `default-selected`。未映射组字段、provider 数组引用与根级
provider 定义显式记账；规则 `no-resolve` 记 approx，未知修饰符不再静默接受。
实际构建和平台边界见[升级记录](singbox-stable-upgrade.md)。该升级批次协议覆盖为 10/15，
这些新增项是现有协议的字段与依赖语义，不能重复算作新协议。

## 2026-10-01 后续实现

- Glaze 正式版 9.0.0 接入共享 JSON codec；运行时快照、API 与内部路由策略已迁移，
  连接解析移至任务线程，代理页消费唯一数据泵的 typed 数据。配置 DOM/YAML 暂保留。
- 拨号字段：interface-name、Linux routing-mark、tfo；mptcp 原生映射带 IPv6 路径差异
  记账。detour 组合不假装物理字段仍生效，手机平台限制与 uint32/布尔校验明确。
- DNS：显式 IP bootstrap、节点专用解析，默认节点解析沿已有 policy/final；
  direct-nameserver 以原生字段接入但明确记为 approx，前置 resolve 时机尚有差异。
  DNS transport 与出站组共同检查循环，不改直连绕过启动依赖问题。
- AND/OR/NOT 有界递归：域名、TCP/UDP、源/目标端口与 CIDR 子集，未支持子条件
  拒绝整条。inline provider 接入 domain/ipcidr/classical 子集，整份 payload 原子转换。
  同名失败 provider 不再变成内置 CN；历史未声明国内别名明确记 approx。
- Android PROCESS-NAME/正则转 package_name/package_name_regex；Linux UID 转 int32
  user_id。平台不适用时记 unsupported；真机 owner 查询与应用选择界面仍未验证/实现。
- 示例、两份 README 和分层基线已同步；具体失败契约与范围见对应文档。

以上已通过 Linux 与 Android arm64 C++ 主目标编译，均包含 UI codegen；
固定 1.14.2 AAR 与完整 Android Debug APK 也已构建，国内规则集已打包，v1/v2
签名核验通过。没有运行新增功能的测试、远程线路握手或真机运行验证。
功能数量不能据此算成一个整体完成百分比。Glaze 工程验证见
[迁移记录](glaze-migration.md)，需要产品决策的事项见 [待决策记录](pending-decisions.md)。

## 接下来优先补齐什么

### 基础继续补齐（2026-10-01）

- 普通规则、逻辑子条件和 inline classical 共用条件转换。新增 DOMAIN-REGEX、
  平台进程/包名子条件；路由支持 IP-VERSION 与逻辑 UID。规则集遵守独立
  HeadlessRule schema，不能写入 user_id/ip_version。
- proxy-server-nameserver-policy 支持完整域名/+.后缀，只影响节点解析；
  仅在存在可转换的 proxy-server-nameserver 时生效，共用策略优先级与依赖检查。
- 修正 MATCH 默认出口、规则终止位置和 REJECT-DROP；无效 MATCH 目标现在明确
  编译失败。目标 CIDR、rules 类型错误及非字符串条目不会再静默进入/离开配置。
- 重名 provider 的所有同名声明均拒绝；payload 失败定位到具体索引与条目。
- 下载增加独占临时文件、写入检查和提交前校验入口；GEO 预取接入文件头筛查。
  原生 JSON 不再进入应用预取，Android 国内资产不在线刷新或删除。
- 缓存命中也进入桌面按周刷新清单；只读编译不删文件。致命编译失败保留已有账本。
  SRS 完整解压/校验仍依赖内核。后续已接入桌面普通订阅刷新/编辑的候选校验；
  本地首次导入可保存为未参与来源，启用时检查；Android 由后台 libbox 检查。
  外部 provider 下载仍未接入。

上述“外部 provider 下载仍未接入”是 2026-10-01 的历史状态；2026-10-03 已接入
HTTP 直连 YAML/text 子集与单值请求头，当前剩余指定代理、MRS、定时更新等边界。
实现与当时验证见 [基础补齐记录](foundation-progress.md)。
完整配置 IR、hosts 通配/别名/FakeIP 与完整编排恢复仍未实现；桌面 Clash 多来源首阶段已实现，
原生 JSON 次来源和跨来源 detour 未开放，手机约束保持。

1. **原生 DNS 设置与剩余策略**：扩展现有节点专用 policy，补直连策略与解析时机、
   规则集策略、hosts 通配/别名/FakeIP 与过滤模式；再做原生编辑入口。已接入的完整域名/
   整层通配策略与强制 HTTP3 不等于支持全部 Clash DNS；fallback-filter 仍未实现。
2. **规则集与逻辑路由**：通用本地/远程 .srs、规则集管理，补齐已接入 AND/OR/NOT
   子集的其余条件；下载来源、失败处理和规则顺序必须明确，不能继续只按国内别名替换。
3. **Android 应用路由**：后台已经实现连接 owner/包名查询与包名/正则规则输入，
   继续补齐应用选择入口；
   Android 版本、UID 查询失败与共享 UID 场景要分别验证。
4. **WireGuard/Tailscale/OpenConnect 端点**：以连接对象建模，包含凭据、状态、启停与
   路由目标，不放进旧的 WireGuard outbound；平台支持与失败回滚先于 UI 承诺。
5. **剩余拨号与协议字段**：更多连接选项、TLS/ECH、其它 SS 插件及插件未映射参数，
   逐项核对固定版本；detour、引用图检查与桌面 Clash 来源命名空间已经接入，
   原生 JSON 次来源及跨来源 detour 仍待实现。

这些条目是后续工作，未记为本轮已完成的功能。iOS 仍按项目约定暂缓。

## 核对依据

- 固定 revision 的 [出站/端点注册表](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/include/registry.go)
  和 [QUIC 注册表](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/include/quic.go)：核对协议分类及 WireGuard 出站移除。
- [AnyTLS option](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/option/anytls.go)
  与 [实现](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/protocol/anytls/outbound.go)：TLS 必需、会话参数与 UDP 边界。
- [Snell option](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/option/snell.go)
  与 [实现](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/protocol/snell/outbound.go)：v4/v6 及其字段。
- [路由 option](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/option/rule.go)
  与 [URLTest 实现](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/protocol/group/urltest.go)：匹配字段、容差默认值和间隔约束。
- Mihomo 的 [AnyTLS](https://wiki.metacubex.one/config/proxies/anytls/)、
  [Snell](https://wiki.metacubex.one/config/proxies/snell/)、
  [规则语义](https://wiki.metacubex.one/config/rules/) 与
  [端口列表语法](https://wiki.metacubex.one/handbook/syntax/)：核对 Clash 输入语义。

此前 DNS/AnyTLS/Snell 适配使用实际生产编译器生成配置，并交给当时打包的 1.14.0 二进制执行 `sing-box check`。
配置检查只说明字段可解码、构造配置成功，不代表已完成远端握手、真实吞吐或所有平台
的运行验证。

1.14.0 阶段核对结果（不是本轮新增代理链的检查）：

| 检查 | 结果 |
|---|---|
| 示例 YAML 编译产物交给 `sing-box check` | 退出码 0，长间隔变化进入保真度账本 |
| 版本、端口/CIDR、布尔值、容差边界及不支持字段的配置产物 | 退出码 0，非法条目拒绝并记账；无 SNI 的 AnyTLS 仍有 TLS，凭据空白未丢失 |
| 原生 hosts DNS、逻辑路由与自定义缓存合并产物 | 退出码 0，用户字段保留；内核提示旧 `store_rdrc` 在 1.14 弃用，暂仍接受 |
| `cmake --build build --target clash-flux` / `./run.sh --version` | 成功；版本输出 v0.3.10 |
| `git diff --check` | 成功 |

DNS 核对依据：固定 revision 的
[DNS 类型常量](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/constant/dns.go)、
[DNS rules](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/option/rule_dns.go)、
[TLS](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/dns/transport/tls.go)、
[QUIC](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/dns/transport/quic/quic.go)、
[HTTP3](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/dns/transport/quic/http3.go)，
以及 Mihomo 的 [DNS 配置](https://wiki.metacubex.one/config/dns/)与
[域名通配语义](https://wiki.metacubex.one/handbook/syntax/)。

1.14.0 阶段 DNS 继续适配的生产配置检查：HTTP3/节点与组 detour、精确域名与嵌套后缀、
显式 DoT 443/DoQ 53、非法端口/未知参数/未支持策略、无 nameserver 的策略、
`enable: false` 及当时的文档示例，均已生成配置并通过 1.14.0 打包内核的 `sing-box check`。
读取产物核对了规则排列和端口值。这是配置检查，没有进行远端 DNS 查询或握手；
没有新增或运行仓库测试套件，也没有运行官方 GUI。

1.14.2 本轮已重新编译应用并核对最终打包内核版本、revision；Gradle 配置加载、
libbox 脚本语法和 diff 检查通过。新增代理链和默认选择未在真实网络与手机包中验证，
没有重跑上述历史配置检查。完整结果见[升级记录](singbox-stable-upgrade.md)。

## L3 编排重构补充（2026-10-01）

桌面 Clash 次来源现在能经启用规则加入同一内核；固定 tier + 同层 priority/order、
来源命名空间、目标默认/组/节点、缺失目标失败策略和候选 check 已接入。
原生 JSON 次来源、跨来源代理链、UUID 对象跟踪和崩溃恢复仍未完成。
手机未增加普通多订阅。具体交互与能力边界见
[桌面编排 §7](desktop-subscription-orchestration.md#7-已实现的-l3-重构)。

## L1 / L2 / L3 再复核（2026-10-02）

- L1 固定版本和校验链已接入；实际 CI 不含 Windows arm64/macOS Intel，iOS 暂缓。
- L2 旧协议未知字段、UDP/TLS 和传输子字段的若干静默降级已关闭；10 类协议配置
  通过固定内核 check；初次复核发现凭据 trim、异常 ALPN/WS header 值、根级/不适用
  指纹漏记，列为 F01–F03，后续首批 L2 修复已关闭，见下节。
- L3 Clash 来源隔离和启用规则参与已通过定向编译检查；唤醒链接已接入独立导入表单，
  Linux 冷启动/IPC 与 Android 构建已验证，完整下载、其它系统与真机交互未验收。
- Linux 构建和版本 v0.3.16、9 项项目测试、10 协议配置检查、Android Debug 构建及
  v1/v2 签名核验通过。测试和平台范围的完整记录见 [开发复核](l1-l2-l3-status.md)。

## L2 首批交付：凭据与 TLS/传输取值（2026-10-02）

F01–F03 已修复并加入永久 `test_singbox` 回归。凭据保留原文，异常 ALPN/headers
及相邻传输取值整条拒绝并记账；全局指纹按来源独立继承，节点优先，不适用 TLS 字段
不再消失或触发隐式启用。OpenVPN inline auth-user-pass 一并改成两行原文读取，
LF/CRLF 均保留凭据空格/引号/制表符；缺行或空凭据需要交互时拒绝并保留来源账本。
random 指纹分布和内核折叠的 chrome PSK/PQ 别名按 approx 记账。

新增回归在修复前复现 48 项失败；修复及扩展回归后，Linux 9 项项目测试通过。
扩充的 10 协议样本包含带空白的凭据、原文 header 和全局/节点指纹，生产转换结果
通过固定 1.14.2 `check`。Android arm64 Debug 已重新构建，未安装或验收远程握手。
类型覆盖仍为 10/15；这批交付不包含新的协议、外部 provider 或完整 DNS 适配。

## L2 后续字段交付：UDP / VMess 填充 / HY2（2026-10-02）

v0.3.18 后的工作区补 VMess/VLESS UDP 编码及历史布尔开关、VMess 填充选项，
HY2 salamander/gecko、Gecko 包尺寸、严格端口范围与 hop-interval 秒数/范围。
不再只凭 obfs-password 强制转 salamander，不再把缺混淆密码的线路降为普通连接。
固定内核要求单个 hopping 端口也输出 `n:n`，已通过真实 `check` 核对；ports-only
节点可省略单个 port。默认、限制、不适用字段与非法输入按保真度边界处理，见
[字段基线](singbox-layers-and-fidelity.md)。

Linux 主目标、永久转换回归和 9 项项目测试、扩充的 10 协议样本内核检查、Android
Debug 构建通过。三份本地订阅的节点/组回归分别保留 32/32、30/30、34/34 节点；
规则/provider 使用独立测试副本规范化，未验收原规则与远程连接。
协议类型覆盖仍为 10/15，外部 provider、更多 DNS/TLS/QUIC 字段继续待实现。

## L2 后续字段交付：SS 插件 / UOT 版本（2026-10-02）

内置 simple-obfs HTTP/TLS 与 v2ray-plugin WebSocket/TLS/mux 子集已接入。
host/path 按原文转义到 SIP003，补 Clash 的 host/mux 默认，显式 TLS false 不再
有机会被原生的键存在语义开启。非空自定义 headers、跳过证书校验、HTTPUpgrade、
证书/ECH 与其它插件仍拒绝并记账。UOT 写对象并显式选择 Clash 的 legacy v1 默认，
支持输入 v1/v2；不把未知整数窄化后继续运行。保真度边界见[契约](singbox-layers-and-fidelity.md)。

新增永久回归实现前复现 16 项失败，修复后通过；项目 9 项测试、Linux 主目标、
8 节点插件/UOT 样本的固定内核检查、Android Debug 构建通过。规范化的三份订阅
副本保留全部 32/30/34 个节点，原订阅和凭据未改动；未验收远程握手。
类型覆盖仍为 10/15，本批没有增加新的协议家族或 L3 编辑入口。

## L2 后续路由交付：嵌套 GEO / RULE-SET（2026-10-02）

AND/OR/NOT 路由子条件新增 GEOIP/GEOSITE、private/lan 和已转换 inline RULE-SET。
共用顶层资源转换，保持 outer action 与 NOT；不从逻辑中的 CN 条件推导全局 DNS。
未知子条件、非法资源或失败 provider 原子拒绝整条规则，并撤回本条新建资源及
tag 缓存，保留前后有效规则；国内别名仍 approx。HeadlessRule 继续拒绝 GEO/RULE-SET，
外部下载和 Android 缺失本地 GEO 的行为没有扩大支持范围。

实现前永久回归复现 5 项失败，修复后通过；双来源嵌套运行 tag 与原始缓存身份
回归通过。公开样本的 6 条逻辑规则、3 份 inline provider、2 份 GEO 资源全部生成，
固定 1.14.2 内核 check 通过。Linux 主目标、项目 9 项测试、Android Debug 构建通过；
之前两个字段样本仍通过内核检查，三份规范化订阅副本保留全部 32/30/34 节点。
验证范围及命令见[开发复核](l1-l2-l3-status.md)，保真度分类见[契约](singbox-layers-and-fidelity.md)。
协议类型覆盖仍为 10/15，hosts/FakeIP、外部 provider 与其它 DNS/协议字段仍待实现。

## L2 后续协议交付：Hysteria v1 UDP 子集（2026-10-02）

Clash 转换新增 Hysteria v1，类型覆盖为 **11/15**。保留原文认证、Base64 auth 的
优先级、XPlus 密码、必需 QUIC TLS、UDP 限制、严格端口范围和 10 秒跳端口默认；
使用原生 QUIC 窗口字段，成对非零 exact，默认初始分配差异记 approx。
单边窗口、非 UDP 伪装、fast-open:true、空 ALPN 与未映射证书/ECH/兼容字段仍拒绝。
共享带宽修复 HY2 的 MBps/Mbps 大小写折叠错误，字节单位乘 8 并检查整数范围。

实现前回归复现 16 项失败，修复后通过；认证解码/长度、窗口、TLS、hopping、
单位和拒绝节点 MATCH 的永久回归通过。项目测试 9/9、Linux 主目标、Android
Debug 构建通过；HY1 的 5 节点样本、扩充的 11 协议/12 节点样本以及之前 SS/逻辑
样本均通过固定内核 check。三份规范化私有订阅副本保留全部 32/30/34 节点。
未验收远端握手、原订阅全部规则或真机生命周期；该批随后并入 v0.3.19。
边界与复现命令见[开发复核](l1-l2-l3-status.md)及[保真度契约](singbox-layers-and-fidelity.md)。

## L2 后续协议交付：SSH 子集（2026-10-02）

Clash 转换增加 SSH，当前类型覆盖为 **12/15**。密码、内联/加密 PEM 私钥、主机
公钥和原生库算法偏好列表原文保留；双认证的尝试顺序与请求 UDP 的差异明确记账。
外部私钥路径、空用户名、异常/未知算法及未映射字段拒绝整条节点；不补假 TLS/network。
通用 detour 依赖检查和双来源同名对象隔离回归通过，失败 MATCH 保留已有诊断。

Linux 主目标、版本校验、项目测试 9/9、Android Debug 构建和 diff 检查通过。
5 节点公开 SSH 样本以及总协议/HY1/SS/逻辑样本通过固定 1.14.2 check；内核拒绝
错误私钥解密口令与无效主机公钥的两个候选。三份规范化订阅副本保留 32/30/34 节点。
这些检查不等于 SSH 远端握手、全部原规则或真机生命周期验收；该批随后并入 v0.3.19。
SSH 详细分类和来源见[保真度契约](singbox-layers-and-fidelity.md)，命令与完成度见
[开发复核](l1-l2-l3-status.md)。ShadowTLS/Tor/Naive、更多协议字段、外部 provider
和 L3 原生编辑入口继续待实现。
