# 内核分层与 Clash → sing-box 保真度契约

本文规定 Clash-Flux 的架构分层，以及「Clash 生态里的东西怎么进 sing-box、UI 允许承诺
什么」。`AGENTS.md` 的「架构分层与保真度契约」是本文的强制摘要；两者冲突时以本文为准，
并回头修订摘要。

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
- 版本固定：桌面二进制与 Android libbox 必须是同一版本、同一 SHA256（当前 1.14.0）；升级
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
  DNS 引用未知出站（`Dns` + `Approx`）、`MATCH` 目标缺失（`Rule` + `Approx`）。
- **判定标准**：用户订阅里的条目**消失或语义改变** → 进账本（`note`）；**运行期事件、
  语义不变** → 留在 `warn`。目前只剩 4 处 `warn`，都是后者：托管 TUN 关掉
  `auto_redirect`、规则集缓存不可用改走在线拉取（×2）、原生连接未连接导致规则暂停。
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

1. **协议与端点补面**：出站 8/16、端点 1/5（见 §4.1 / §4.2），以及 `interval` 的
   30m 硬约束与 `tolerance` 透传（见 §4.5）；
2. `RULE-SET` / rule-providers 的通用转换（目前只认 `cn` / `cn-ip` 别名）；
3. Android 侧的进程/包名匹配（`package_name` / `package_name_regex`）。

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

## 4. 当前基线（sing-box 1.14.0）

### 4.1 出站协议（8 / 16）

已映射（`src/singbox_proxy.inc:136-247`）：

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

sing-box 1.14 官方还提供、但尚未映射：`hysteria`(v1)、`snell`、`anytls`、`shadowtls`、
`ssh`、`tor`、`naive`、`wireguard`。

### 4.2 端点（1 / 5）

已映射：`openvpn-client`，由 `.ovpn` 原文编译（`src/singbox_yaml_openvpn.inc:226`），支持
多 `remote` / `remote-random` / static-key / TLS 内联 `ca`/`cert`/`key` / `peer-fingerprint` /
`tls-auth`/`tls-crypt`/`tls-crypt-v2` / 内联 `auth-user-pass`；**外部文件路径一律拒绝并提示
改为内联**。

sing-box 1.14 另有：`openvpn-server`、`openconnect`、`wireguard`、`tailscale`。

### 4.3 组类型（2 / 2，另两类降级）

| 订阅类型 | 结果 |
|---|---|
| `select` | `selector`（exact） |
| `url-test` | `urltest`（exact） |
| `fallback` | `urltest` + 降级账本条目（approx） |
| `load-balance` | `urltest` + 降级账本条目（approx） |

见 `src/singbox_proxy.inc:267-296`。另：Android「仅测速」实例会把 `urltest` 再降为
`selector`，避免内核与 App 重复扫描（`src/singbox_proxy.inc:277-285`）。

### 4.4 规则（约 13 类）

已映射：`DOMAIN` / `DOMAIN-SUFFIX` / `DOMAIN-KEYWORD` / `DOMAIN-REGEX` / `IP-CIDR` /
`IP-CIDR6` / `GEOIP`（`PRIVATE` → `ip_is_private`；国家码 → SagerNet 官方 `.srs`
rule_set）/ `MATCH` / `PROCESS-NAME` → `process_name` / `PROCESS-PATH` →
`process_path` / `PROCESS-PATH-REGEX` → `process_path_regex`，以及 `action:reject`、
sniff、DNS 劫持等前置规则。

`process_name` / `process_path` / `process_path_regex` **只在 Linux/Windows/macOS 生效**
（上游文档明确标注）：Android 目标编译时这三个记 `Unsupported` 并提示按包名匹配；
`PROCESS-NAME-REGEX` 在**任何平台都没有**对应字段（sing-box 只有路径正则），同样记账
并给出替代写法。

sing-box 侧可用但未映射：`source_ip_cidr` / `source_port` / `port` / `port_range` /
`package_name`(Android) / `user` / `network` / `ip_version` / 逻辑规则（`and`/`or`）/
本地 `rule_set`、Clash `RULE-SET` 与 rule-providers 的通用转换（目前只认 `cn` /
`cn-ip` 这组国内别名）。

### 4.5 已知边界（**不要当成 bug 去修**）

- **`urltest` 不支持手动锁定**：`PUT /proxies/{name}` 只接受 `Selector`
  （`experimental/clashapi/proxies.go`），响应 `400 Must be a Selector`；`/proxies` 也不
  返回 `fixed` 字段。因此「点节点锁定自动选择」在内核层不存在。
- **`interval` 必须 ≤ `idle_timeout`**（缺省 30m），否则 sing-box 启动即失败。编译器当前
  原样透传订阅的 `interval`，`interval > 1800` 的订阅会让内核起不来；`tolerance` 也尚未透传。
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
| M1 | 翻译层补面 | 协议 8 → 16、端点 1 → 5、YAML `openvpn` 节点、`interval`/`tolerance` 修正、保真度账本：**结构化 + 设置页消费已落地**，剩余 warn-only 站点、页面级角标与 CLI 输出见 §2.2 |
| M2 | 产品层 Clash 化 | 代理页排序/筛选/定位/组导航/滚动记忆、订阅 merge/script 覆写链、连接页 / 日志页 / 设置页补齐 |
| M3 | 差异化 | 端点作为一等连接对象 + 路由策略、`rule_set` 管理界面、进程规则、移动端能力 |

顺序理由：**L2 决定能吃下多少订阅，L3 决定好不好看，差异化决定为什么不用 Verge。**

## 8. 维护要求

- 升级 sing-box / libbox 时，逐条复核 §4 基线表，并把差异写回本文与账本。
- 修改编译器映射时，同步更新账本条目与对应测试。
- 本文与 `AGENTS.md` 摘要不一致时，以本文为准并修订摘要。
