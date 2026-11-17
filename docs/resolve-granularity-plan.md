# 路由解析精细化与 `no-resolve` 保真计划

2026-11-17。R0、R1a、R1b、R2 与 R3 的首批范围已落地。当前能力见 [节点订阅边界](node-subscriptions.md)
和 [内核分层与保真度契约](singbox-layers-and-fidelity.md)。实测与后续边界见 [本轮验收记录](#9-本轮验收记录)。

## 1. 实施前基线

2026-11-17 已完成的修复只覆盖裸节点订阅的一个明确场景：最终规则不含 `ip_cidr`、`geoip`、`ip_is_private` 或 `rule_set` 等 IP 匹配条件时，不再注入前置 `resolve`，代理目标域名交给服务器远端解析。

只要个人或全局规则引入上述 IP 条件，当前编译器仍在普通规则链前补回一条基本无条件的 `resolve`。另外，Clash `no-resolve` 修饰符目前只保留原 IP 匹配条件并记 `Rule/Approx`，前置解析仍可能执行，尚未复现“不为该规则触发 DNS”的语义。

因此，纯域名订阅的问题已经由现有修复覆盖；后续主要收益来自**少量 IP 规则与大量域名规则混合**的配置。

## 2. sing-box 能提供的精细度

[`resolve`](https://sing-box.sagernet.org/configuration/route/rule_action/#resolve) 是 `route.rules` 中的 non-final action，不是整个配置唯一的总开关：

- 语法上属于一条路由规则，可以携带域名、入站、端口、进程等匹配条件。
- 运行时按连接和规则顺序执行。固定版 sing-box 将查询结果写入 `DestinationAddresses`，同时保留 `Destination` 中的原域名。规则能用地址匹配，实际出站仍须单独观察收到的是域名还是 IP。
- 每条 `resolve` 可以分别指定 DNS server、地址族策略、缓存、超时和 ECS。
- 同一目标的后续规则可以使用已解析地址。`route-options.override_address` 会清空已有地址，普通 `resolve` 查询返回错误时会中止匹配。移动插入点会改变哪些连接到达该查询，验收须覆盖失败路径。

固定实现见 [sing-box 1.14.4 路由动作与地址覆盖](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/route/route.go#L627-L703)及[解析实现](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/route/route.go#L904-L929)。

需要区分另外两层：

- `dns.rules` 决定已经发起的本地 DNS 查询如何选择上游，不负责决定代理目标是否必须先解析。
- outbound 的 `domain_resolver` 属于拨号层；DIRECT 用它解析目标域名，代理出站还涉及代理服务器自身域名的解析。

Clash 的 `no-resolve` 是附在具体规则上的修饰符。sing-box 没有可直接输出的同名字段，需要通过 `resolve` 的条件和位置重建语义。

Mihomo 对照沿用仓库已引用的 `88dcbf7f1614a67c3b36b848ee3592dfa92ada36`。该版本的 CIDR 规则在带 `no-resolve` 时跳过触发查询，但仍读取已有的目标 IP。它的解析辅助函数写入单个 `DstIP`，失败时记录错误并继续匹配。sing-box 保留地址列表，且显式 `resolve` 查询错误会终止该连接的规则匹配。多地址和失败路径必须独立验证，再判定保真等级。 [Mihomo CIDR 匹配](https://github.com/MetaCubeX/mihomo/blob/88dcbf7f1614a67c3b36b848ee3592dfa92ada36/rules/common/ipcidr.go#L36-L45) · [Mihomo 解析辅助函数](https://github.com/MetaCubeX/mihomo/blob/88dcbf7f1614a67c3b36b848ee3592dfa92ada36/tunnel/tunnel.go#L299-L329)

## 3. 目标与非目标

目标：

1. 只对确实需要目标 IP 的连接执行本地解析，尽量让其他代理目标保留域名并交给远端解析。
2. 在不改变原规则顺序、匹配范围和最终出口的前提下，逐步复现 `no-resolve`。
3. 让必要解析继续使用内核 DNS 配置；应用远程 DoH 只负责提高必要解析的可信度。

非目标：

- 不一律删除 `resolve`，不把 GeoIP 等必须依赖 IP 的规则改成猜测匹配。
- 不增加第二份 DNS 数据库，不修改订阅原文，不增加 `resolve` / `default_domain_resolver` 用户开关。
- 不在本计划中引入 FakeIP、完整 DNS 分流编辑器或域名型 DoH bootstrap。
- 不用“少解析”作为唯一验收标准；规则结果和保真度优先于解析次数。

## 4. 预期收益与不可消除的情况

| 场景 | 精细化后的预期 |
| --- | --- |
| 裸节点订阅且现有扫描未发现目标 IP 或 `rule_set` 条件 | 继续省略普通 `resolve`，hosts 专用解析按来源配置执行 |
| 前面的域名规则已经决定最终出口，后面才有 IP 规则 | 将 `resolve` 延后后，前面已出站的连接不再解析 |
| IP 规则带 `no-resolve` | 不为该规则触发 DNS；未解析的域名跳过该 IP 条件，字面 IP 仍可匹配 |
| 域名条件与 IP 条件共同约束一条规则 | 可证明等价时，只对符合域名等前置条件的连接解析 |
| 链首就是无条件 `geoip` / `ip_cidr`，且没有 `no-resolve` | 仍必须解析任意到达该规则的域名，无法靠重排消除 |
| 只有来源 IP 条件，例如 `source_ip_cidr` | 读取连接已有的来源地址，目标域名无需为它查询 |
| 复杂逻辑、反向匹配或内容未知的混合 rule-set | 不能证明等价时继续保守解析并保留 approx |

所以该方向能显著减少不必要的本地解析，但不能保证所有网站都不再使用本地 DNS。必须解析的场景仍可能受到污染，远程 DoH 与精细化是互补关系。

## 5. 分阶段实施

### R0 冻结行为和测试输入

- 以 Clash-Flux `71ce44a` 加 2026-11-17 已有工作区为计划起点。开工时记录实际源码状态、sing-box 1.14.4 完整 revision、Mihomo 完整 revision 和 DNS 夹具配置。保留[裸节点远端解析与 DoH 的实测证据](evidence/2026-11-17-remote-resolution.md)及既有 target/protocol 冻结产物，单独记录计划允许的输出差分。
- 从生产编译入口采集裸节点、Clash YAML、原生 JSON、PPTP/OpenVPN 的最终规则顺序、出站、`rulePlan.origins` 和保真度。另核对一次有个人规则、全局覆盖、次来源和内核内网规则的组合，明确每种条件的来源。
- 构造同一探测请求的三份证据。记录实际 DNS 查询及得到的地址、内核命中的规则及出口、代理接收到的目标类型及地址。对双内核分别记录字面 IP、已有解析地址、A/AAAA 多答案、NXDOMAIN 和超时。使用独立工作目录、环回端口、合成订阅与可控 DNS，冷查询用不同测试域名隔离缓存。
- 退出条件为夹具能重现当前前置查询，也能识别当前裸节点远端解析的零本地查询。记录两内核的差异，再进入生成逻辑修改。

### R1：把 `resolve` 延后到首个真正需要 IP 的规则前

- R1a 先覆盖裸节点订阅加个人、全局规则的组合。沿最终生效规则顺序计算插入点，目标 IP 规则和内容无法证明的 rule-set 都可能需要解析。识别来源 IP、端口、域名和纯模式条件的独立语义。对 `no-resolve` 保持现有保守策略，直到 R2 接通修饰符的元数据。
- 把应用生成的普通 `resolve` 放到首个需要目标地址的规则前。前面已命中 final action 的连接可跳过查询，后续规则仍按原顺序执行。链首就是 IP 匹配时，插入点保持在链首。无目标 IP 依赖时保持省略。
- 遇到显式 `resolve`、会改写目标的 `route-options` 或语义未审查的动作，保留该来源已有的普通解析位置，直到对照验证覆盖其相互作用。嗅探、DNS 劫持、hosts 专用解析及 hosts 域名排除按原契约生成。
- R1b 根据 R0 和 R1a 的差分，将同一流程推广到已证明可安全运行的 Clash YAML 组合，作为独立验收点。原生 JSON 的用户动作保留完整原生顺序，PPTP/OpenVPN 的托管生成保持现有策略。
- 退出条件为域名优先规则和较晚的 IP 规则分别在真实内核走到预期出口，规则计数与索引同步。若前序域名规则能终结连接，验证本地 DNS 失败也不会阻断这个出口。若到达必须解析的规则，验证 DNS 失败仍按固定内核现有路径处理。

### R2：让 `no-resolve` 真正阻止该规则触发解析

- 首批覆盖 Clash YAML 顶层的正向 `IP-CIDR` / `IP-CIDR6`，先从实际规则解析入口传递其 `no-resolve` 意图。元数据随所属 IP 条件和来源定位保留，含嵌套时的子条件归属留给后续阶段。其余规则、provider 和规则内部的 AND/OR/NOT 继续按现有保真度记账。
- 仅为带 `no-resolve` 的 IP 条件不生成新的 `resolve`；保留 `ip_cidr` 等原匹配字段，不输出 sing-box 不存在的 `no-resolve` 键。
- 未解析的域名不能为了匹配该条件临时查询 DNS；目标本来就是 IP 时仍按原规则匹配。
- 用同一条带修饰符的 CIDR 验证字面 IP、从未解析的域名、已有解析结果的域名三种输入。加入先后顺序调换、IPv4/IPv6 混合答案、只命中非首选地址、hosts 和解析失败等用例。分别记录“该规则没有发起查询”和“该条件最终匹配了同一连接”。两者按明确适用范围成立后，再调整相应的保真度记录。
- classical provider 和嵌套逻辑留到后续扩展，届时修饰符按具体条件归属。子条件的 `no-resolve` 只作用于该子条件。

### R3：为简单组合规则生成条件化 `resolve`

- 对可明确分解的正向 AND 规则，可把域名、端口等非 IP 前置条件复制到 `resolve`，原规则保持不变。
- 只有全部条件都属于同一条正向合取、复制不会改变匹配范围时才允许该转换。
- `OR` / `NOT`、`invert`、跨层逻辑和混合 rule-set 不能简单删除 IP 条件后复制；无法证明等价时保留 R1/R2 的保守行为。
- rule-set 只有在能确定其内容全部不需要或确实需要目标 IP 时才进一步分类；外部内容不可用或混合时不得猜测。
- 使用生产逻辑树确定合取关系。sing-box 默认规则会将域名与目标 IP 等字段归入同一 OR 组，字段同时出现不足以证明 AND。保留原逻辑树和同组字段语义。 [sing-box 路由匹配](https://sing-box.sagernet.org/configuration/route/rule/#default-fields)
- 单条规则的前置条件为假时，后面的 IP 规则仍可能需要解析，必须验证跳过当前动作后仍在必要位置查询。多个位置可能被同一连接经过时，先证明重复查询、服务器选择、缓存和失败行为符合现有契约。证据不足的组合沿用 R1/R2。
- 规则集分类先使用明确标明类型的内置集合或同一编译候选提供的不可变快照，并以资源版本绑定结论。域名集、目标 IP 集、来源 IP 集各自验证。混合集、嵌套引用、可独立刷新的远程 rule-set 在证明范围之外继续保守解析。

### 生成与追溯的交接点

- [同仓转换器](converter-project.md)负责在转换时保留目标地址依赖及修饰符。Clash-Flux 现有装配负责结合生效的个人/全局规则、来源命名空间、主来源 DNS 和实际模式。解析计划是最终路由生成的一步，消费这些已准备好的输入。资源读取和发布继续通过既有候选路径。
- R1 从 [`applyConnectionRules`](../src/singbox_rules.inc) 的普通解析插入逻辑切入。R2 从同文件的 `convertRule` 保留顶层 CIDR 修饰符，并在目标快照捕获、已接受快照重放和冷编译三条路径对照输出。元数据随规则身份传递，最终数组索引由装配阶段派生。
- 主来源共享目标快照可以携带规则本身的元数据，合并个人规则、重放替换/禁用后再确定插入点。运行候选按本次的真实环境生成。用于浏览和编辑的来源规则身份继续对应来源对象。 [目标工作模型](rule-document-plan.md)
- 以最终数组位置写入 `rulePlan.origins`，生成的解析动作占用真实索引。核对 `rulePlan.ruleCount`、来源 ID 和出站标签。对比内核 `/rules` 的完整顺序和 `/connections` 的命中描述，确认解析动作前后的已标注规则仍可唯一关联。 [连接规则追溯](connection-rule-tracing.md)

## 6. 必须保持的不变量

- 原规则的先后顺序、匹配条件、final action 和出口不变，不能为了减少 DNS 查询改变分流结果。
- UserOverride、SourcePolicy、DNS 劫持、嗅探和 hosts 专用解析的既有优先级不变。
- 解析结果属于连接和当前目标。前面规则查得的结果可以复用，目标被改写时按固定内核的地址失效行为处理。
- 所有生成字段必须是固定 sing-box 1.14.4 的真实 schema；候选继续通过固定内核检查。
- 来源文件与数据库原文不变；失败保留旧配置、引用和活动状态。
- `rulePlan` 的规则索引、来源名称和出站追溯必须对应最终 JSON，不能因插入多条 action 错位。
- `reverse_mapping`、DIRECT `domain_resolver`、hosts 和已有 DNS policy 的交互必须单独回归。
- 只有实际行为通过验收后，才把对应 `no-resolve` 条目从 approx 改为 exact；其余组合继续明确记账。

## 7. 验证与验收

编译回归至少覆盖：

1. 域名规则在 IP 规则之前时，`resolve` 位于正确的后续位置，origins 对齐。
2. 带 `no-resolve` 的单独 IP 规则不触发新增 `resolve`，字面 IP 仍匹配。
3. 未命中前序域名规则、随后到达无条件 GeoIP 的域名仍执行解析。
4. 简单“域名 + IP”规则只生成限定范围的 `resolve`；复杂逻辑保持保守结果和 approx。
5. hosts、多来源命名空间、rule-set、个人 `reject ip_cidr` 和默认出口不回退。

除 JSON 断言外，使用固定内核和本地受控 DNS/代理服务观察实际行为：DNS 查询次数、最终出口、字面 IP 匹配及未解析域名是否跳过对应条件。仅检查生成 JSON 不能证明 `no-resolve` 已保真。

| 验收案例 | 来自实际运行的证据 |
| --- | --- |
| 域名规则先匹配或拒绝，后面有目标 IP 规则 | 出口或拒绝结果保持预期。DNS 服务收到的目标查询为零，成功转发时代理看到域名 |
| 携带域名到达必需解析点 | 受控 DNS 查询发生在该规则之前。响应地址命中 CIDR，目标出口和代理收到的地址类型符合观察。NXDOMAIN 或超时按内核行为记录 |
| `no-resolve` 三种输入 | 字面 IP 条件仍可命中，未解析域名无新增查询，已有地址命中范围对照固定 Mihomo。分别检查单地址、混合 A/AAAA 和同族多地址 |
| hosts、DIRECT 和来源自带 DNS | hosts 映射仍优先，普通解析绕过 hosts 专用域名。DIRECT 和代理服务器的引导查询分别计数。远程 DoH 只参与现有覆写契约允许的候选 |
| 复杂逻辑和规则集 | 覆盖来源 IP、正向合取、混合/未知 rule-set、命名空间同名和版本替换。对照 DNS、出口及账本，避免把一部分条件扩成整条规则 |
| 连接追溯和候选失败 | 解析动作插入后，完整 `/rules` 顺序与活动计划吻合。候选检查失败保留原活动规则和引用，历史连接不改绑 |
| mixed 入站与 TUN | 分别记录接收到的域名或字面 IP，覆盖已有 `reverse_mapping` 和嗅探的路径。受控 mixed 结果与 Windows/Android 真机结果单独记录 |

每个阶段先完成项目的主目标、版本和 diff 检查，再按修改范围运行 `test_singbox`、target/protocol baselines、`dns_hosts_runtime`、`dns_policy_runtime`、`dns_tls_runtime` 和 `connection_trace_runtime`。涉及规则集时还运行 `personal_rule_sets_runtime`。候选用固定内核执行 `check` 后仍需实际发起探测。逐项记录预期差分、未覆盖分支和对应保真度。

真实老 VMess 订阅至少复测 Google、双栈站点和含个人 IP 规则的配置。Windows TUN、Android 真机和公网结果分别记录。

## 8. 推荐落地顺序

先完成 **R0 → R1a**，取得裸节点混合规则的可重复证据，再交付 Clash YAML 安全场景的 R1b 和顶层 CIDR 的 R2。R2 依赖规则解析、目标快照与运行装配都能保留同一条规则的修饰符。R3 按可证明的组合规则逐项开放。

每个阶段独立更新当前行为、保真度账本和验证证据。远程 DoH 继续作为必要解析的独立保障，不与本计划混成一次大范围 DNS 重构。

## 9. 本轮验收记录

源码以 `71ce44a` 加既有工作区为基线。WSL2 Fedora 45、Clang 23.1.5/libc++、CMake 4.4.2 Debug。
固定对照为 sing-box 1.14.4 `c005f35300ce76427dc285481ec5695f5b179cd2` 与 Mihomo
`88dcbf7f1614a67c3b36b848ee3592dfa92ada36`。独立旧编译器保留两份生成配置，裸节点 SHA-256
`79b0b042ccf41f8f4a0e84c952bd67a385173ec26d2f8b3d372c72b962a09060`，混合配置 SHA-256
`1a6d195900e624cce7aa7b6b1fc4195cf125c8d20f065b5b0d238f69d4e9816f`。

| 范围 | 生成与固定内核结果 |
| --- | --- |
| R0/R1 裸节点与混合 Clash YAML | 使用 loopback DNS、SOCKS 目标记录和 DIRECT 回显。旧混合配置将污染应答 `192.0.2.123` 送到代理。新配置中，先命中域名规则的请求将原域名交给代理，目标 DNS 查询为零。后面的 CIDR 仍查询并直连。必需查询的 NXDOMAIN 或超时继续中断 sing-box 连接，实测 Mihomo 会进入后续兜底。 |
| R2 正向顶层 CIDR | 域名无已有地址时跳过，字面 IP 和 hosts 地址可命中。已有解析时两个地址族都能用非首个地址匹配。Mihomo 的同一组 2 个 A 加 2 个 AAAA 答案中，12 次请求有 6 次进入兜底、6 次命中拒绝。保真度仍为 approx。目标快照重放与冷编译在个人 CIDR 前插后仍生成相同配置和诊断。 |
| R3 简单 AND 与内联规则集 | 只对符合域名和端口的请求解析，原分流规则保持。后面还有需要地址的规则时保留首个无条件查询。OR、混合集仍保守解析。域名集和来源 IP 集验证零目标查询，目标 IP 集查询后直连。 |
| DNS 与连接链路 | hosts 命中不走普通 resolver。较早的 DIRECT 域名规则使用自己的 `direct-nameserver`，随后的 IP 路由复用已查地址。域名规则集的 DNS 优先级及 IP-only 入站 `reverse_mapping` 使用独立固定内核运行夹具验证。 |

42 份目标冻结样例中，39 份完整观察一致。另三份分别校对普通解析移位、唯一顶层 `no-resolve`
省略查询及诊断更新、内联域名集合省略查询。变更只更新涉及的规则动作、计数及对应诊断。
协议冻结样例通过。实际旧 VMess 订阅含 51 个 TLS 节点。用同一可用节点在 WSL mixed 入站复测，
裸节点和加入前置域名规则、后置 CIDR 的配置均取得 Google HTTP 302、Cloudflare HTTP 200，
两份内核日志中的这两个目标查询均为 0。订阅与临时内核配置未纳入仓库。

原生 JSON、OpenVPN/PPTP 原生来源和纯域名 Clash YAML 保留原解析策略。混入 VPN 补偿时默认
保留前置解析，带顶层 CIDR `no-resolve` 的 Clash 规则链仍按修饰符规划。多个依赖沿用一次保守解析。
规则内部与 classical provider 的 `no-resolve`、不透明资源及跨层条件仍待扩展。
Windows TUN 和 Android 真机转发留待实机验证。
