# 裸节点订阅代理目标改为服务器远端解析（P1）

2026-11-17。工作区基线 `71ce44a` 加 HUI `e2afd8a3` 升级与本轮 P1 修改。WSL2 Fedora 45 /
Clang 21 / libc++ / Debug，固定 sing-box 1.14.4（revision `c005f353`）。

## 起因与诊断

用户报告 Windows 安装 v0.3.112 后“有的网站进不去”。活动配置为一份裸 VMess 节点订阅
（46–51 节点，全部 `aid>0 + TLS`，`aes-128-gcm` 已按 v0.3.112 生效）。

实测证据：

- `github.com` 经代理 200/2.6s；`baidu.com` 强制 IPv4 200/5s，默认双栈超时。
- `www.google.com` 经代理超时；内核 DNS 日志给出 **A=`69.171.235.22`、AAAA=`2001::1`**
  （典型 DNS 污染伪造地址）。
- SOCKS5 指定真实 IP `142.251.157.119` 后 Google **200/2.2s**，证明 VMess 链路正常。
- 活动配置 `dns` 仅 `local` 服务器；`route.rules` 含无条件 `{"action":"resolve"}`，
  强制每条连接先用本地 DNS 解析目标，污染 IP 被当成 VMess 目标。

对照实现：Xray `domainStrategy: AsIs` 默认不本地解析；v2rayN Wiki「代理目标解析策略」
明确“未设置或 AsIs 时代理节点自行处理域名解析”。sing-box 的 `resolve` 规则动作
（`resolve request destination from domain to IP addresses`）是我方额外注入，非内核默认。

## 隔离实验

同一 sing-box 1.14.4、同一订阅配置，仅改路由动作：

| 配置 | Google | GitHub | Baidu | 本地解析 Google |
| --- | --- | --- | --- | --- |
| 含 `resolve`（现状） | 超时 | 200 | 双栈超时 | 有，污染 IP |
| 仅删 `resolve` | 200 / 2.36s | 200 | 200 | 0 次 |
| 再删 `default_domain_resolver` | 200 / 1.58s | 200 | 200 | 0 次 |

结论：只需移除无条件 `resolve`，`default_domain_resolver` 可保留。

## 修改

- `src/singbox_rules.inc`：仅 `NodeSubscription` 来源在最终规则不含 IP 匹配条件
  （`ip_cidr` / `geoip` / `ip_is_private` / `rule_set`，含嵌套逻辑规则）时不注入前置
  `resolve`；含 IP 匹配条件时在原位置补回并同步调整 `rulePlan.origins` 索引。
  其余来源（Clash YAML、原生 JSON、PPTP/OpenVPN）行为不变；hosts 专用 resolve 保留。
- `tests/test_singbox.cpp`：裸订阅断言无 `resolve`；加入 `ip_cidr` 个人规则后断言
  `resolve` 补回且 origins 索引与规则位置一致。
- `tests/fixtures/target-rule-baseline.json` `node-uri`：仅删除预期中的 `resolve`，
  `rulePlan.count` 6→5（观察模式复核：除该两处外配置/账本/origins/labels 完全一致）。
- 文档：`docs/node-subscriptions.md`、`docs/singbox-layers-and-fidelity.md`。

## 验证

- `test_singbox` 全量通过；`--target-baseline` 42 冻结案例通过；
  `--protocol-baseline`、`--protocol-entries` 通过；
  `ctest -R '^(dns_hosts_runtime|dns_policy_runtime|dns_tls_runtime)$'` 3/3 通过。
- 真实老订阅（46 节点）重新编译：`route.rules` 不再含 `resolve`。
- 修复后配置实跑（独立端口、独立工作目录）：
  Google **200 / 1.85s**、GitHub **200 / 2.56s**、百度 **200 / 1.77s**，
  内核日志本地解析 Google 次数为 **0**。

## 边界

- 带 geoip/ip_cidr/rule_set 的来源仍前置本地解析，被墙域名在这类来源下的解析正确性
  依赖来源自带 DNS；远程 DoH 与 DNS 设置项（P2）未实施，另行评估。
- 实测为 WSL mixed 入站；Windows 本机 TUN 与 Android 真机需另行验收。
- 本地构建临时绕过 libc++ 21 `std::filesystem::path` 迭代器比较问题，验证后已还原，
  最终 diff 不含该绕过。

## 追加：应用内远程 DNS 设置（模式 3，内核绑定）

2026-11-17 实施。形态与 Clash Verge Merge / v2rayN 直连/远程 DNS 一致：设置与来源分离，
生成内核配置时合并，不修改订阅原文，不建第二 DNS 库。

- `network::Advanced::remote_dns`（`core.network_settings.advanced` 扩展位，Glaze 字段追加），
  `ParseDohUrl` 仅接受 `https://IP[:port][/path]`（IPv4/方括号 IPv6），`ValidateAdvanced` 同步拒绝。
- 网络设置“基础”页新增“远程 DNS（DoH，可空）”文本框与说明、tab 错误徽标；
  三份 strings properties 同步（`msg_1048d593…`、`msg_dca14db6…`）。
- 编译：仅当来源无自带 DNS（`defaultDnsInjected`）且设置非空时，追加
  `type=https` 的 `remote` DNS server（detour = `route.final`），`dns.final = remote`；
  DIRECT 出站固定 `domain_resolver = local`，节点域名引导仍走 local。
- 测试：`test_singbox` 新增远程 DoH 编译断言（服务器/detour/final/DIRECT resolver）
  与 8 组 DoH URL 正反差例；全量 `test_singbox`、`--target-baseline`、
  `--protocol-baseline/--protocol-entries`、`page_transition`、`ui_resources`、
  `dns_hosts/dns_policy/dns_tls` 均通过。
- 编译与回归均通过（含命名空间 tag 修正后的全量回归）；真实订阅整份配置的 DoH 实跑未全部完成，
  原因见下节（首节点当时不可达、活动配置被切换到自带 DNS 的 profile-3）。

## 远程 DoH 验证补充

- 初版把 DIRECT resolver 写成字面量 `local`，命名空间来源下引用不存在；
  改为读取 `servers[0].tag`（命名空间后 local tag），`test_singbox` 同步用
  `mainConnectionId = runtime-main` 覆盖，避免生产配置启动失败。
- 中间几次实跑失败均为测试环境问题：首节点（a台湾）当时不可达、活动配置被用户切换到
  profile-3（自带 DNS + DoH，非本功能覆盖对象），不是 DoH 本身失败。
- 最小 DoH 配置（`final=remote`、DoH detour 直连）实测：`cloudflare 200`、
  `google 200`，`lookup succeed for www.google.com: 142.251.155.119 …`（真实 IP）。
  另：活动配置 profile-3 自身已使用“DoH detour 到 selector”的同一模式，证明代理内 DoH 可行。

## 追加：独立“DNS 设置”二级页与覆写开关

- 设置首页“代理与网络”新增 `NetworkDnsEntry` → `NetworkDnsEditorPage`（`network_settings_editor.cpp`），
  首行 `AdvancedSwitch(override_dns)`，开关下显示远程 DoH 字段与说明；保存复用
  `core.saveNetworkSettings`（revision/候选/失败留旧），保存/保存并应用与网络设置一致。
- `Advanced::override_dns`（Glaze 追加）控制应用 DNS 是否参与编译：false 时 remote_dns 仅作草稿，
  行为同旧默认；true 且 remote_dns 非空才注入远程 DNS server。远程 DoH 字段从虚拟网卡“基础”页
  移至该独立页面，基础页徽标同步移除。
- 测试：`override_dns=false + remote_dns 非空` 时 servers 仅 local、final=local；
  `override_dns=true` 时注入 remote（detour=route.final）。全量 `test_singbox`、
  `--target-baseline`、`--protocol-baseline/--protocol-entries`、`page_transition`、
  `ui_resources`、`dns_hosts/dns_policy/dns_tls` 通过。
