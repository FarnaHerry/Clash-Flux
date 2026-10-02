# 内核分层与 Clash → sing-box 保真度契约

本文规定 Clash-Flux 的架构分层，以及「Clash 生态里的东西怎么进 sing-box、UI 允许承诺
什么」。`AGENTS.md` 的「架构分层与保真度契约」是本文的强制摘要；两者冲突时以本文为准，
并回头修订摘要。

实际完成度与本轮验证见 [2026-10-02 L1 / L2 / L3 开发复核](l1-l2-l3-status.md)。
本文是能力和保真度契约；复核中的 F01–F03 首批修复已完成，完整字段与产品覆盖仍未完成。

一句话定位：**内核贴 sing-box，输入贴 Clash 生态，产品层用 Clash 的词汇只暴露内核真有的
能力，并把 sing-box 独有能力产品化成 Clash 客户端给不了的差异点。**

## 1. 三层与各自铁律

| 层 | 职责 | 代码位置 | 铁律 |
|---|---|---|---|
| L1 内核层 | 运行 sing-box，提供控制面 | 官方二进制（桌面 spawn）/ libbox（Android）、`clashflux.core`、`clashflux.api`、`clashflux.stream` | 不 fork、不臆造字段、能力以上游文档/源码为准 |
| L2 翻译层 | 把 Clash 生态翻译成 sing-box 形态 | `clashflux.singbox`、`clashflux.model`、`clashflux.routing`、`clashflux.openvpn`、`clashflux.pptp` | 每条映射必须落在 exact / approx / unsupported 之一，禁止静默丢弃 |
| L3 产品层 | 交互、状态编排、CLI、托盘 | `src/ui/*`、`clashflux.store.*`、`clashflux.cli` | 只用内核真有的能力，不做假 UI |

### L1 内核层（紧贴 sing-box）

- sing-box 对未知字段**直接拒绝启动**，因此配置里出现的每个键都必须能在上游文档或源码里
  找到出处；不要靠「看起来像」猜字段。
- 版本固定：桌面二进制与 Android libbox 必须是同一版本、同一源码 revision（当前 1.14.2）；平台资产分别校验 SHA256；升级
  时按上游逐项复核，而不是就地改内核行为。
- 控制面走内核自带的 `clash_api`（sing-box 官方提供的兼容层，不是本项目的发明）；内核
  没有的概念（endpoint、rule_set、selector/urltest 语义）不在这一层做兼容包装。

### L2 翻译层（处理 Clash 生态）

- 输入有三类：Clash YAML（远程/本地订阅）、sing-box 原生 JSON（直通并合并托管设置）、
  原生连接订阅（PPTP、OpenVPN `.ovpn`）。三者都经 `clashflux.singbox` 出口。
- 每条条目——协议、组、规则、单个字段——必须归入三档之一，并写入保真度账本（§2）：
  - **exact**：与 sing-box 语义 1:1；
  - **approx**：语义近似或字段被忽略（例：`fallback`/`load-balance` → `urltest`）；
  - **unsupported**：跳过或拒绝编译（例：未支持的协议、外部证书文件路径）。
- **禁止静默丢弃**；也**禁止为「能跑起来」改写用户语义**（替换测速 URL、悄悄关掉证书
  校验、把用户的路由规则丢掉）。
- 平台差异收束在平台函数内（例：Android 订阅下载走 `HttpClient`，桌面走 vendored
  curl），不下沉成通用层的能力矩阵。

### L3 产品层（Clash 词汇 + sing-box 能力 + 自有差异化）

- 词汇与信息架构跟 Clash 生态（策略组、订阅、规则、出站模式、连接、日志、托盘），因为
  用户和内容都在这个生态里。
- **只暴露内核真有的能力**：没有假锁定按钮、没有假 `fallback` 组、没有只为「像 Verge」
  而存在的控件。做不到就不出现，或者出现但明确标注「不支持 / 已近似」。
- 反过来，**sing-box 独有能力要主动暴露**（§6），否则产品上限永远只是「功能更少的
  Clash 客户端」。

## 2. 保真度账本（fidelity ledger）

保真度账本是 L2 的产物、L3 的输入：没有它，产品层只能靠自由文本文案猜，最终必然开始
「假装支持」。

### 2.1 支持级别与 UI 义务

| 级别 | 含义 | 产品层义务 |
|---|---|---|
| exact | 1:1 映射 | 正常渲染，无标记 |
| approx | 近似或字段被忽略 | 能看到「已近似 / 哪些字段被忽略」 |
| unsupported | 跳过或拒绝 | 灰显或计数 + 原因，必要时给处置建议 |

### 2.2 现状

已落地：

- `singbox::Fidelity` / `singbox::FidelityScope` / `singbox::FidelityNote` 与
  `CompileResult.fidelity`（`src/singbox.cppm`）。`warnings` 保留为**自由文本投影**
  （每条 = 对应 `fidelity[i].detail`），历史消费方与内核启动诊断不受影响。
- `Context::note(scope, level, subject, detail, action)`（`src/singbox_context_dns.inc`）
  是产出账本的唯一入口；`warn()` 保留给「不是映射保真度」的消息（输入非法、
  运行期不可用、平台限制等）。
- 已结构化的站点（判定标准见下）：协议不支持 / 传输层 / SS 插件 / 无名称或类型 /
  重名 / 缺少 server·port / obfs 缺 password（`Node`）、组类型不支持 / provider
  （`use`、`include-all`）被忽略 / 组编译失败（`Group` + `Unsupported`）、
  `fallback`+`load-balance` 降级与组成员被移除（`Group` + `Approx`，组本身还在，
  只是成员变少）、规则类型 / 规则目标 / `RULE-SET` 格式 / 非法 GEOIP·GEOSITE /
  Android 缺本地规则集（`Rule`）、DNS 格式或协议不支持（`Dns` + `Unsupported`）、
  DNS 引用未知出站（`Dns` + `Unsupported`）、`MATCH` 目标缺失（`Rule` + `Unsupported`，编译失败）。
- **判定标准**：用户订阅里的条目**消失或语义改变** → 进账本（`note`）；**运行期事件、
  语义不变** → 留在 `warn`。目前只剩 4 处 `warn`，都是后者：托管 TUN 关掉
  `auto_redirect`、规则集缓存不可用改走在线拉取（×2）、目标未连接导致执行规则声明的不可用策略。
- `store::CoreSnapshot.fidelity`（`src/store/core_store.cppm`）：`startCore` 编译后赋值，
  **预览编译**（`core_store_state.inc::proxyGroupsSnapshot()`）同样留下账本——所以报告
  在内核没跑时也有内容。
- `singbox::FidelitySummary()`：把账本折成一行「跳过 3 个节点 · 降级 1 个组」，
  只报计数不报明细。
- 消费点：
  1. **设置页**「内核」段的「配置保真度」报告（`CoreFidelityReport`，空报告渲染为
     空占位，不做常驻提示）——可回看的完整明细；
  2. **toast 通知**：导入订阅 / 手动刷新 / 启用订阅成功后，`ProfileFidelitySummary()`
     （`src/ui/profiles_page_support.cpp`）给出一行摘要，非空才补一条 6 秒 toast
     （`配置已导入 · 跳过 3 个节点 · 降级 1 个组`）。**只在用户动作后提示**：内核重启
     （启停 / TUN 切换 / 模式切换）会重新编译，但不再重复提示；
  3. **代理页分组标签角标**：组级条目让对应分组标签多一个 `!`（`SectionTab.badge`），
     只做定位、不展开明细；
  4. **CLI `profile check [<id>]`**：打印摘要 + 逐条明细（detail + action），有
     `Unsupported` 条目时退出码 1，便于脚本与机场作者自查。
- 回归测试：`tests/test_singbox.cpp` 断言结构化条目（未知协议 / REJECT 成员移除 /
  fallback / load-balance）、`warnings` 投影、以及 `FidelitySummary` 的计数与空账本。

仍缺（下一批）：

1. **协议与端点补面**：Clash 出站类型已覆盖 10/15、端点编辑入口仍为 1/5
   （见 §4.1 / §4.2；类型覆盖不代表所有字段均已映射）；
2. 外部 `RULE-SET` / rule-providers 下载与完整格式转换（已有 inline 子集与国内别名）；
3. Android 应用选择入口及 owner 查询失败/共享 UID 等运行边界（包名/正则输入已接入）。

（进程匹配已补齐：`profile check` 实测某真实订阅的 24 条被跳过规则在补齐后只剩 1 条，
剩下那条是订阅自己写错的 `GEOIP,telegram`。）

### 2.3 形态（已实现的部分）

```cpp
enum class Fidelity { Exact, Approx, Unsupported };
enum class FidelityScope { Node, Group, Rule, Dns, Field };

struct FidelityNote {
    FidelityScope scope = FidelityScope::Field;
    Fidelity level = Fidelity::Approx;
    std::string subject;  // 订阅里出现的名字：协议 / 组名 / 规则类型 / 字段
    std::string detail;   // 近似说明或拒绝原因，可直接展示给用户
    std::string action;   // 可选：建议动作（改 type、内联证书、移除字段）
    std::string sourceId; // 来源定位，空表示无 profile 身份的单配置调用
};

struct CompileResult {
    std::string json;
    std::string error;
    std::vector<std::string> warnings;   // 自由文本投影（历史消费方）
    std::vector<FidelityNote> fidelity;  // 结构化账本
};

std::string FidelitySummary(const std::vector<FidelityNote>& notes);
```

消费点（✅ 已实现 / ⬜ 待做）：

- ✅ 设置页：完整明细「配置保真度」（`CoreFidelityReport`）；
- ✅ 订阅页动作后的 toast 摘要（导入 / 刷新 / 启用，非空才提示）；
- ✅ 代理页分组标签角标（`SectionTab.badge`，只标组级条目）；
- ✅ CLI `profile check [<id>]`（明细 + 退出码）；
- ⬜ 订阅卡常驻计数：**刻意不做**——用 toast 代替，避免页面常驻噪音；
- ✅ 回归测试：断言关键条目的级别与摘要，升级 sing-box 后级别变化即测试失败。

### 2.4 何时必须更新账本

- 修改 `clashflux.singbox` 的任何映射时；
- 升级 sing-box（或 libbox）版本时；
- 新增订阅类型、协议、规则类型或平台时。

## 3. 决策流程

新增能力或映射时按顺序判断：

1. sing-box 有原生等价 → **exact 映射**。
2. 语义不同但可表达 → **approx + 账本条目**。
3. sing-box 没有：
   a. 默认：**降级 + 账本条目**（如 `fallback`/`load-balance` → `urltest`）；
   b. 仅当「体验损失大且实现可控」时才允许**应用层补齐**（候选：`urltest` 选路与锁定）。
      应用层补齐必须自证：内核在跑 / 未跑两态都有定义、失败可回滚、不引入第二个真相
      来源（不得出现「UI 认为选的 A、内核实际走 B」）；
   c. 否则**不做**。
4. **任何情况下不得在 UI 假装支持。**

## 4. 当前基线（sing-box 1.14.2）

原生 JSON 合并托管项时，`experimental.cache_file` 仅强制 `enabled: true`，保留
`path`、`cache_id`、`store_fakeip`、`store_rdrc` 等用户配置，不整对象重建。

### 4.1 出站协议（Clash 转换覆盖 10 / 15 个代理协议类型）

已映射（`src/singbox_proxy.inc`；按协议类型计，不计 direct/bridge/block 和策略组）：

| 订阅类型 | sing-box 出站 |
|---|---|
| `ss` | `shadowsocks` |
| `vmess` | `vmess` |
| `vless` | `vless` |
| `trojan` | `trojan` |
| `hysteria2` / `hy2` | `hysteria2` |
| `tuic` | `tuic` |
| `http` | `http` |
| `socks5` | `socks` |
| `anytls` | `anytls`：密码、强制 TLS、SNI/ALPN/uTLS、会话池参数、客户端元数据 |
| `snell` | `snell`：仅显式 v4；PSK、reuse、UDP 开关、http/tls 伪装 |

sing-box 1.14 官方还提供、但尚未从 Clash YAML 映射：`hysteria`(v1)、`shadowtls`、
`ssh`、`tor`、`naive`。**WireGuard 出站在 1.13 已移除，应使用 endpoint**，不能再放进
出站覆盖率的分母。依据为固定 revision 的
[出站注册表](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/include/registry.go)
与 [QUIC 注册表](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/include/quic.go)。

全部已接入节点使用明确的字段白名单（含传输层子字段）；未映射的组合伪装、证书约束
或其它字段会使整条节点被跳过并记 `Node/Unsupported`，不会悄悄改成普通线路。
旧协议分支的 `fingerprint` / `shadow-tls-opts` 同样逐字段记账，不与
`client-fingerprint`（uTLS 客户端指纹）混用。节点拒绝后，引用它的 MATCH / detour
仍按缺失目标失败，保留已产生的账本。
F01–F03 已修复：凭据按 literal scalar 原文读取；ALPN、WS headers 和相邻传输字段
完整校验类型，异常成员导致整条节点拒绝并记 unsupported，不保留部分列表。
订阅生态仍会同时输出 `ws-path`/`ws-headers` 与 `ws-opts`；两套值相同按 exact
合并，冲突时采用显式 `ws-opts` 并记 `Node/Approx`，异常旧字段仍拒绝整条节点。
VLESS 的空 `encryption` 或 `none` 是 mihomo 的兼容默认，省略后按 exact 保留节点；
其它 VLESS 加密字符串在固定 sing-box 版本没有等价实现，记 `Node/Unsupported` 并拒绝节点。
凭据诊断不回显输入值。HTTP/SOCKS 可选空凭据保留；AnyTLS/Snell 仍要求非空凭据。
TLS ALPN 接受 scalar/字符串数组，保持原文和顺序，每项 1–255 字节，允许空数组；
WS header 只接受字符串键值对象，拒绝重复键，path/header 不 trim。
其它未审查字段不能仅凭键名白名单就宣称取值完整。
SS/VMess/VLESS/Trojan/HY2/TUIC/SOCKS 显式 `udp: false` → `network: tcp`（exact）；
HTTP 出站仅支持 TCP，`udp: true` 记 approx。无效布尔值拒绝；HY2 仅映射正整数
Mbps（整数或 `30 Mbps` 字符串），其它单位、小数与无效值拒绝并记 unsupported，
不截断后改成默认自适应带宽。HTTP 传输多路径只取首项并记 approx。
Trojan/HY2/TUIC 保留协议自带 TLS，不要求订阅额外声明 `tls: true`；显式关闭 TLS
拒绝。HY2/TUIC 使用原生 QUIC TLS，不注入 TCP uTLS；显式 uTLS/Reality 与 SOCKS
TLS 字段拒绝并记 unsupported。TUIC 的 `disable-sni` 映射到 `tls.disable_sni`，
ALPN 与 servername/SNI 共用 TLS 转换，不因 ALPN 分支丢失其它设置。
`global-client-fingerprint` 是来源内的历史兼容输入：节点 `client-fingerprint` 优先，
否则继承该来源值，两者均省略时保留 chrome 基线；主/次来源互不继承。
全局指纹类型不符、为空或内核不认识时整份来源失败并记 `Field/Unsupported`；
节点无效指纹拒绝整条节点。未启用 TLS 的显式 SNI/ALPN/指纹/Reality 等字段也拒绝，
显式 `tls: false` 不会因 SNI/Reality 被重新开启；省略 tls 的历史 SNI/Reality 隐式启用
行为仍保留。QUIC 不使用这个 TCP uTLS 默认。`random` 指纹分布与 Clash 不保证相同，
`chrome_psk*` / `chrome_padding_psk_shuffle` / `chrome_pq*` 在固定内核里折成普通 chrome，
均记 `Node/Approx`。依据：[固定 uTLS 实现](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/common/tls/utls_client.go)、
[Clash TLS 输入](https://wiki.metacubex.one/config/proxies/tls/)；Mihomo 已弃用全局指纹，
新配置应优先指定节点值。

本批取值保真度基线：

| 输入边界 | 级别与处理 |
|---|---|
| 凭据 scalar 原文、可选空 HTTP/SOCKS 凭据 | exact；原文写入，不裁空白或丢空字段 |
| ALPN scalar/字符串数组、WS 字符串 headers、传输字符串 | exact；保持内容与顺序，完整校验；不接受异常成员或重复 header 键 |
| 旧版 `ws-path`/`ws-headers` 与 `ws-opts` | 相同值 exact 合并；冲突使用 `ws-opts` 并记 approx；异常类型 unsupported |
| VLESS `encryption: ""` / `none` | exact 兼容默认，省略后使用 sing-box 原生 VLESS |
| 其它 VLESS `encryption` | unsupported；固定内核没有等价实现，拒绝整条节点 |
| 来源级全局指纹继承、节点覆盖 | exact 历史兼容映射；不同来源不共享默认值 |
| 凭据/TLS/传输类型异常或不适用的 TLS 字段 | unsupported；拒绝整条节点，记录字段；引用其 MATCH/依赖仍失败 |
| 全局指纹为空、类型异常或内核不认识 | unsupported；拒绝整份来源并保留字段账本 |
| random 分布、固定内核折叠的 PSK/PQ 指纹别名 | approx；保留内核行为并记语义差异 |
AnyTLS 的时间字段使用整数秒数，最少空闲会话为非负整数；未启用 `udp` 时记
`Node/Approx`，因为内核 AnyTLS 出站固定支持 TCP/UDP，没有 `network` 限制。
Snell v1/v2/v3/v5、缺省版本与未映射的伪装均拒绝，不自动改成 v4；内核 v6 可由原生
JSON 使用，本轮不把它当成 Clash 版本转换。字段边界及后续计划见
[内核能力审查](singbox-capability-audit.md)。

### 4.1a 代理链

上述已接入节点的 `dialer-proxy` 映射为原生 `detour`（exact），可引用当前配置内
已编译的节点或策略组，支持向后声明。完成出站图后检查目标、重复 tag 和循环，
包括策略组的全部候选成员：即使当前未选中会形成环的成员，也拒绝该配置。
缺失目标或循环记 unsupported 并返回编译错误，不删除 detour 后改成直连。
此处代理链仍只接受来源内引用；桌面来源隔离见 §4.1c，手机仍是单活动普通订阅。

### 4.1b 原生拨号字段（2026-10-01）

| Clash 字段 | 原生字段与级别 |
|---|---|
| `interface-name` | `bind_interface`，桌面 exact；手机非空值且无 detour 时拒绝节点并记 unsupported |
| `routing-mark` | `routing_mark`，uint32 十进制；仅 Linux 桌面非零值 exact，其它平台无 detour 时拒绝 |
| `tfo` | `tcp_fast_open`，布尔值直接映射；实际 TCP Fast Open 依赖系统与对端 |
| `mptcp` | `tcp_multi_path`；启用记 approx，固定内核普通 IPv6 dialer 没有同步开启 MPTCP |
| 上述非默认选项 + `dialer-proxy` | 字段保留但记 approx：原生 detour 忽略物理拨号选项，不自动迁到上游节点 |

非法值整条节点拒绝，不把字符串或溢出整数转成默认值。Android 编译器的 `__linux__`
不能作为 GoOS Linux 能力判断，routing_mark 明确排除 Android。
接口存在性、权限和对端 TCP 支持由实际运行环境决定，本轮未作线路握手验证。
依据：[固定拨号文档](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/docs/configuration/shared/dial.md)、
[DefaultDialer](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/common/dialer/default.go)。

### 4.1c L3 桌面来源编排基线（2026-10-01）

| 能力 | 当前契约 |
|---|---|
| Clash 主/次来源命名空间 | 已生成字段的 tag、组成员/default、detour、DNS resolver/detour、规则集及规则引用一并改写；原始名称保留在 sourceObjects |
| 次来源导出闭包 | 仅加载启用规则的目标及其出站/DNS 依赖；不以其它来源的同名对象补齐 |
| 次来源规则/providers | 不导入全局，source.rules 记 Field/Approx；MATCH 只定义默认导出 |
| 次来源全局 DNS | 不覆盖主 DNS，仅保留出站引用依赖；source.dns 记 Dns/Approx |
| 原生 JSON 次来源 | 尚未开放，显式 Unsupported + 候选拒绝；主 JSON 未知字段继续保留 |
| 目标缺失/未连接 | 匹配条件保留，Reject / 主默认出口 / Direct 由显式 unavailable 决定；运行事件进 warning |
| 原生内部 CIDR | SourcePolicy 层，最长前缀作为同权重顺序；全局覆盖始终在前 |
| 手机 | 不载入普通次来源；所有可写入口保持单活动普通订阅限制 |

运行预览不覆盖实际来源映射；保真度角标按 sourceId + 原始对象名关联，避免同名组误标。
桌面应用前用固定内核 check，失败恢复的范围与缺口见
[编排实现记录](desktop-subscription-orchestration.md#7-已实现的-l3-重构)。

### 4.2 端点（1 / 5）

已映射：`openvpn-client`，由 `.ovpn` 原文编译（`src/singbox_yaml_openvpn.inc:226`），支持
多 `remote` / `remote-random` / static-key / TLS 内联 `ca`/`cert`/`key` / `peer-fingerprint` /
`tls-auth`/`tls-crypt`/`tls-crypt-v2` / 内联 `auth-user-pass`；**外部文件路径一律拒绝并提示
改为内联**。
内联 auth-user-pass 按用户名/密码两行读取，保留空格、制表符和引号，只移除
CRLF 分隔符，不使用选项拆词器。缺行或空凭据需要交互输入时拒绝转换，失败带来源
账本；无法伪装成已有凭据。依据：[OpenVPN 凭据读取](https://github.com/OpenVPN/openvpn/blob/master/src/openvpn/misc.c)。

sing-box 1.14 另有：`openvpn-server`、`openconnect`、`wireguard`、`tailscale`。

### 4.3 组类型（2 / 2，另两类降级）

| 订阅类型 | 结果 |
|---|---|
| `select` | `selector`（exact） |
| `url-test` | `urltest`（exact） |
| `fallback` | `urltest` + 降级账本条目（approx） |
| `load-balance` | `urltest` + 降级账本条目（approx） |

见 `src/singbox_proxy.inc`。`tolerance` 为 1–65535ms 时原样映射；0 会被内核替换成
50ms，不能宣称零容差，显式 0 / 无效值记 `Group/Approx` 并使用 50ms。未配置时沿用
内核默认值。Android「仅测速」实例仍把 `urltest` 转为 `selector`，避免重复扫描。

`select.default-selected` 对应原生 selector 的 `default`（exact），必须是编译后
仍存在的成员；无效值或用于 urltest 时记 approx，不伪造手动锁定。缓存中已经保存
的选择可能优先于初始 default，这不是一次强制切换命令。
未映射的组字段逐字段记 `Group/Approx`，包括 lazy、timeout、expected-status、
filter 与界面字段。`use` 数组及 include-all 系列无法展开时记 `Group/Unsupported`，
只保留显式成员；proxy-providers 当前不下载或展开。rule-providers 的 inline 子集已接入，
外部 http/file/MRS 来源逐声明记 unsupported，未成功转换的声明不会被同名国内别名替换。

### 4.3a DNS（2026-10-01 更新）

| Clash 输入 | 当前映射与保真度 |
|---|---|
| UDP/TCP/DoT/DoH/DoQ 地址 | typed server；DoT/DoQ 默认 853、DoH 默认 443，显式其它端口保留 |
| HTTPS `#h3=true` | `type: h3`（内核字段不是 `http3`），证书校验保持开启；`h3=false` 保留普通 DoH |
| `#出站名` / `#出站名&h3=true` | 绑定已实际编译的节点或组；未知出站/未接入的接口绑定拒绝服务器并记 unsupported |
| `system` / `dns.enable: false` | 使用托管 local 系统解析；禁用时不激活源 DNS 自定义策略 |
| 单服务器 `nameserver-policy`：完整域名 / `+.域名` | `dns.rules` 的 domain / domain_suffix + route server；精确匹配在先、后缀由长到短，保留最佳匹配优先级 |
| `default-nameserver` | IP DNS bootstrap，允许已接入的加密 IP DNS；域名和 detour 依赖拒绝并记 unsupported，数组仍记 approx |
| `proxy-server-nameserver` | 域名代理节点的原生 domain_resolver；未指定时按已有精确/后缀 policy 或 DNS final 选择；显式策略尚未覆盖更多通配 |
| `proxy-server-nameserver-policy` | 完整域名/+.后缀按同一优先级匹配，仅在 proxy-server-nameserver 有可转换默认服务器时生效；只设置节点解析器，不进入普通 DNS 查询规则 |
| `direct-nameserver` | 写入 DIRECT domain_resolver，approx：托管前置 resolve 已解析的请求不会重查此解析器 |
| DNS 地址数组 | 首个可转换服务器作为默认/策略目标，不执行并发或后备，记 approx |
| `fallback` | nameserver 存在时不参与查询，缺省时作为默认来源；两者都记 approx，不冒充污染筛选 |
| `http://` DNS | unsupported；内核 HTTPS transport 强制 TLS，不改写用户 HTTP 地址 |
| 其它 DNS 字段 / hosts / 模式、规则集通配及未映射 URL 参数 | 显式 unsupported；不把 Clash 转换缺口描述成内核缺少 FakeIP/DHCP/hosts 等能力 |

策略只在无 nameserver 时也会生成，默认路径继续为 local。完整 DNS 策略可通过原生
JSON 配置保留；Clash 的 `*.域名`（一级子域）和 `.域名`（不含根域）暂不转换，
不能都改写成包含根域的 suffix。DNS 服务域名使用显式 bootstrap 或 protected local。
DNS server resolver、DNS detour、节点 resolver 与组候选共同检查依赖，缺失目标或循环
返回编译错误；不去掉 detour 后直连。`direct-nameserver-follow-policy`、
fallback-filter、FakeIP 与 hosts 输入转换仍待实现。
DNS approx 和 unsupported 在摘要中分别计为“近似”和“忽略”。原生 JSON 不经过此
Clash 图检查，仍保留原始字段并由固定内核校验。

### 4.4 规则

已映射：`DOMAIN` / `DOMAIN-SUFFIX` / `DOMAIN-KEYWORD` / `DOMAIN-REGEX` / `IP-CIDR` /
`IP-CIDR6` / `GEOIP`（`PRIVATE` → `ip_is_private`；国家码 → MetaCubeX `.srs`
rule_set）/ `GEOSITE` / `RULE-SET` 国内别名 / `MATCH` / `PROCESS-NAME` → `process_name` / `PROCESS-PATH` →
`process_path` / `PROCESS-PATH-REGEX` → `process_path_regex`，以及 `action:reject`、
sniff、DNS 劫持等前置规则。

新增 exact 匹配：`SRC-IP-CIDR` → `source_ip_cidr`（IPv4/IPv6）、`SRC-PORT` →
`source_port` / `source_port_range`、`DST-PORT` → `port` / `port_range`、
`NETWORK` → `network`（TCP/UDP，大小写归一）。端口列表用 `/` 分隔，范围的 `-`
转换为 `:`，单端口与范围在同一规则中保留 OR 语义；非法 CIDR、端口、范围或网络类型
整条跳过并记 `Rule/Unsupported`，不保留半条匹配条件。

`no-resolve` 修饰符目前保留匹配条件但记 `Rule/Approx`：托管路由的前置 resolve
仍可能解析域名，不能宣称复现 Clash 的“不解析”语义。未知附加修饰符和缺少字段的
规则整条跳过并记 unsupported，不再截断第三个字段以后的内容后静默接受。

`process_name` / `process_path` / `process_path_regex` **只在 Linux/Windows/macOS 生效**。
Android 的 `PROCESS-NAME` → `package_name`、`PROCESS-NAME-REGEX` →
`package_name_regex`（1.14 新字段），保留包名/正则匹配语义。Android 的路径规则
继续 unsupported；桌面的进程名正则无等价，仍拒绝，不能借 Android 字段假装支持。
应用 owner 查询沿现有后台平台接口：API 29+ 使用系统 socket UID 查询，旧版本用
libbox procfs 路径；查不到 owner、共享 UID 与旧系统的权限边界尚需真机验证。

`UID` → `user_id` 只在 Linux 桌面 exact，接受 0–2147483647 的单个整数；其它平台
或非法值整条拒绝。Android app UID 不是 Linux user_id 匹配入口，按包名分流。
普通规则、逻辑子条件与 inline classical 共用 match-only 转换。进程/包名及正则
按上述平台范围也可作子条件；UID 可作路由逻辑子条件，但不能进入 inline provider：
固定版本 HeadlessRule 没有 user_id。`IP-VERSION,4/6` → ip_version 支持普通/逻辑
路由规则；HeadlessRule 同样没有 ip_version，不能把路由字段直接写入规则集。

`AND/OR/NOT` 的明确子集已递归转换到原生 logical + mode/invert。子条件支持
DOMAIN、DOMAIN-SUFFIX、DOMAIN-KEYWORD、DOMAIN-REGEX、NETWORK、源/目标端口与 CIDR，
以及上述平台进程/包名条件；路由还可使用 UID/IP-VERSION。最多
24 层、256 个条件；括号扫描最多 32 层。只有外层携带 action/outbound。
未支持子条件、非法括号或子条件修饰符会拒绝整条规则，不删除条件后扩大匹配。
嵌套 GEO/RULE-SET 暂不在此子集中，原生 JSON 可表达更多逻辑。无效普通目标 CIDR
也由共用转换器拒绝，不再原样写入启动配置。

inline rule-providers 支持任意名称：domain 的完整域名/+.后缀、ipcidr 的 IPv4/IPv6，
classical 复用以上逻辑条件子集。整份 payload 必须可转换才生成原生 inline rule_set，
按用户 RULE-SET 引用的原位置匹配。未知字段、无效条目和其它来源明确记 unsupported。
同名 provider 的所有声明均拒绝，不保留第一个成功声明；失败定位包含 payload 索引、
原条目与原因，整份 payload 不会部分提交。
显式声明优先于国内别名：声明转换失败时引用规则拒绝，不替换成 GEOSITE/GEOIP CN。
未声明的历史国内别名仍保留，但记 approx，因为无法证明与原 provider 内容相同。
外部 YAML/text/MRS 下载、文件来源与规则集管理界面仍未接入；不改变 Android 国内
GEO 规则集的固定构建打包流程。

GEO 缓存由编译器的 `CompileResult.ruleSetResources` 显式声明，覆盖本地命中与
未命中情况。桌面启动任务按周检查刷新，不再只扫描 remote 配置；原生 JSON 的
rule_set 来源、format、download_detour 和 tag 不进入应用预取。编译只读缓存，
坏文件清理由任务层处理，启动修复仅清理当前清单的自有 GEO，Android 不清理或
在线刷新固定打包的 GEO。提交下载前检查 SRS/版本/zlib 文件头，失败保留旧文件；
这是轻量筛查，不能宣称完成解压、校验和或规则语义校验。原生版本与 SRS 上限对齐
固定 [1.14.2 常量](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/constant/rule.go)
和 [二进制读取实现](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/common/srs/binary.go)。

编译失败时已有 fidelity 条目继续向只读明细与失败状态返回；空 JSON 不代表没有
保真度问题。未生成结构化条目的纯语法错误仍经 error 报告。


第一个有效 MATCH 终止订阅规则列表。显式 MATCH,DIRECT 保留 DIRECT；只有没有
MATCH 时才用首个 selector 作应用默认。不可用 MATCH 目标（含 PASS）编译失败并
记 unsupported，不再改成 DIRECT；REJECT-DROP 的普通规则及 MATCH 使用原生
reject + method: drop。它只控制内核处理的连接，不是内核停止时的系统 kill switch。
rules 根类型错误与非字符串行也明确记账。

sing-box 侧可用但未映射：Android 应用选择入口、`user` / 更多逻辑子条件 /
本地 `rule_set` 与外部 rule-providers 的完整转换（已支持 inline 子集和国内别名）。

### 4.5 已知边界（**不要当成 bug 去修**）

- **`urltest` 不支持手动锁定**：`PUT /proxies/{name}` 只接受 `Selector`
  （`experimental/clashapi/proxies.go`），响应 `400 Must be a Selector`；`/proxies` 也不
  返回 `fixed` 字段。因此「点节点锁定自动选择」在内核层不存在。
- **`interval` 必须 ≤ `idle_timeout`**（缺省 30m）。编译器保留订阅间隔；超过 1800s 时
  同步把 `idle_timeout` 延长至该间隔，并记 `Group/Approx` 说明空闲巡检时间的变化。
  无效间隔记账并使用内核默认 3m，不改测速 URL；`tolerance` 的边界见 §4.3。
- 组的 `lazy` / `timeout` / `max-failed-times` / `expected-status` 无对应字段。
- 订阅 `proxies:` 里的 `type: openvpn` 节点尚未识别（当前只支持整条订阅为 OpenVPN 的
  「原生连接」路径）。
- sing-box 的 clashapi 不返回 `selectable`（仅 Selector 语义）、不返回组的 `testUrl`、
  `/providers/proxies` 为空壳：任何依赖这些字段的 Clash 面板能力都只能「打开但残缺」。

### 4.6 页面级决定（**已定，请勿「顺手改回」**）

这些是产品层在既有内核约束下选定的行为，代码注释在同一位置说明理由：

- **全局模式的根组 = `route.final` 指向的那个真实策略组，不是 `GLOBAL`。**
  `GLOBAL` 是 clash_api 合成的只读组（`type: Fallback`，`PUT /proxies/GLOBAL`
  返回 404），它的 `now` 等于内核默认出站（`Manager.Default()` = `route.final` =
  订阅 `MATCH` 目标；我们的编译器缺省回落到首个 selector 组，见 `src/singbox.cpp:66-86`）。
  所以 GLOBAL 只能用来**反查**目标组，不能当可交互根组——拿它当根组会让整屏卡片
  都点不动。
  取法（`src/ui/proxies_page.cpp` 全局模式根组解析）：live 用 `GLOBAL.now` 反查；
  预览态没有 GLOBAL 时回落首个可选组；`route.final` 指向节点或 `DIRECT` 时**保持
  只读 GLOBAL**——此时全局出口被钉死，任何组的切换都影响不了全局流量，显示别的组
  才是撒谎。
- 代理页**不渲染内核做不到的可交互控件**：`urltest` 组的节点卡不提供锁定 / 取消锁定，
  也不做「点进去看子组」的层级（每个策略组是独立标签）。

## 5. 结构性做不到清单（mihomo 专有）

用于回答「为什么 Verge 有、我们没有」——**不做、不模拟**：

- `url-test` 组的手动锁定 / 取消锁定（`fixed`）；
- `fallback` / `load-balance` 组；
- 组级 `lazy` / `timeout` / `max-failed-times` / `expected-status`；
- 代理 provider 页（`/providers/proxies` 真实数据与单个/全部更新）；
- 依赖 mihomo 扩展字段的 Web UI 面板。

这些能力在 UI 中要么不出现，要么明确标注为不支持；已经落地为页面行为的案例见 §4.6。

## 6. sing-box 独有能力（差异化路线）

- 端点家族：`openvpn-client` / `openvpn-server` / `openconnect` / `wireguard` / `tailscale`
  ——作为一等「连接」对象参与路由策略，而不是仅仅当作节点；
- `rule_set`（`.srs`）官方分发与按周预取（缓存校验与预取已实现，缺管理界面）；
- 进程 / 包名路由（`process_name` / `process_path` / `package_name`）；
- 移动端进程内 libbox（Android 已实现，Verge 无移动端）；
- 与内核同源的完整 CLI、Linux 统一 root 服务、PPTP 补充路径。

## 7. 优先级

| 阶段 | 主题 | 内容 |
|---|---|---|
| M1 | 翻译层补面 | 协议类型已覆盖 10/15、`interval`/`tolerance` 与源地址/端口/网络规则已补齐；拨号字段、DNS 解析依赖、inline provider 与逻辑规则子集已补齐；继续补外部 provider、更多 DNS/协议字段，详见能力审查 |
| M2 | 产品层 Clash 化 | 代理页排序/筛选/定位/组导航/滚动记忆、订阅 merge/script 覆写链、连接页 / 日志页 / 设置页补齐 |
| M3 | 差异化 | 端点作为一等连接对象 + 路由策略、`rule_set` 管理界面、进程规则、移动端能力 |

顺序理由：**L2 决定能吃下多少订阅，L3 决定好不好看，差异化决定为什么不用 Verge。**

## 8. 维护要求

- 升级 sing-box / libbox 时，逐条复核 §4 基线表，并把差异写回本文与账本。
- 修改编译器映射时，同步更新账本条目与对应测试。
- 本文与 `AGENTS.md` 摘要不一致时，以本文为准并修订摘要。

## 桌面最终形态与交互参考

[桌面订阅编排](desktop-subscription-orchestration.md)定义唯一主订阅与多次来源、
稳定引用、编译隔离、应用恢复和平台边界；桌面 Clash YAML 多来源按显式规则已接入。
完整最终形态、原生 JSON 次来源与崩溃恢复仍有明确缺口，见该文档 §7。
手机端继续单活动代理订阅，不扩展多订阅编排。
[官方 GUI 研究](singbox-official-gui-review.md)记录状态驱动交互及能力门控，作为
产品接入参考，不把官方当前 main 的能力自动套到固定 1.14 内核上。
