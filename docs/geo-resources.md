# Geo 资源：DAT、MMDB 与独立更新（2026-11-03）

Clash YAML 的自定义 Geo 数据由 L2 下载并转换，L1 只消费 sing-box 原生 inline rule_set。
沿用订阅外部资源页，不增加全局数据库或第二套代理内核。

## 声明与格式

```yaml
geodata-mode: true
geo-auto-update: true
geo-update-interval: 24  # 小时
geox-url:
  geosite: https://example.com/geosite.dat
  geoip: https://example.com/geoip.dat
rules:
  - GEOSITE,example,DIRECT
  - GEOIP,CN,DIRECT,no-resolve
  - MATCH,DIRECT
```

- `geosite` 读取 V2Ray GeoSiteList protobuf；Plain、Regex、RootDomain、Full 分别生成
  domain_keyword、domain_regex、domain_suffix、domain。支持 `category@attribute` 和 `category@a@b`，按属性名存在性筛选，多个属性取 AND；
  名称不区分大小写，bool=false 和 int 属性仍按存在匹配，不猜测按值比较。空属性段拒绝。
  筛选为空时以原生互斥精确域名的 AND 表达永不匹配（固定内核不接受空 inline 列表），
  不输出会匹配全部目标的空对象。完整/根域名仍只接受普通 ASCII/IDNA 文本，拒绝未知语义。
- `geoip` 在 `geodata-mode: true` 时读取 GeoIPList；保留 IPv4/IPv6 CIDR 与 inverse_match。
  类别取数据库真实代码，不局限于内置 GEOIP 的两位国家码。私有地址仍走已有 ip_is_private。
- 文件扩展名不代替内容校验；opaque HTTP(S) URL 同样可用。未知字段/未来类型、截断、
  重复分类、非法 CIDR 和超限整份拒绝。缺少用户引用的分类时失败，绝不替换成内置数据。
- `geodata-mode: false`（默认）时，`geox-url.mmdb` 为 GEOIP 数据来源；true 时只选
  `geox-url.geoip` DAT。未选中的声明只展示、不下载，不混用两份国家库。
- MMDB v2 支持 24/28/32 位记录、IPv4/IPv6 搜索树、数据指针；国家记录接受标准
  MaxMind `country.iso_code`、`sing-geoip` 字符串和 `Meta-geoip0` 字符串/多分类数组。
  IPv6 树中的 IPv4 根及别名按真实前缀展开，IPv4 同时生成显式 IPv4 CIDR；不丢弃别名。
- `geox-url.asn` 用于 IP-ASN/SRC-IP-ASN，接受 GeoLite2-ASN、DBIP 兼容 GeoLite2-ASN、
  ipinfo generic_asn_free.mmdb 的 AS 编号结构；转为原生目标/来源 CIDR，不再改用 ipverse。
  缺少引用的国家/ASN、未知 ASN 数据库类型、错误类型/指针/版本/UTF-8/搜索树均拒绝。
  `.metadb` 只有正文确实为上述受支持 MMDB 结构时可用，不按文件名猜测格式。
- 未配置自定义来源的内置 GEO SRS 路径保持原有行为；本批不改变 Android 固定来源资产。

格式依据：[V2Ray protobuf schema](https://github.com/v2fly/v2ray-core/blob/master/app/router/routercommon/common.proto)、
[Mihomo Geo 配置默认值](https://github.com/MetaCubeX/mihomo/blob/Meta/config/config.go)、
[属性存在性与 AND](https://github.com/MetaCubeX/mihomo/blob/Meta/component/geodata/attr.go)、
[MMDB v2 格式](https://maxmind.github.io/MaxMind-DB/)、
[Mihomo MMDB 记录结构](https://github.com/MetaCubeX/mihomo/blob/Meta/component/mmdb/reader.go)、
[sing-box 原生 headless rule](https://sing-box.sagernet.org/configuration/rule-set/headless-rule/)。
固定内核仍为工程锁定版本，网页最新字段不自动成为本项目支持承诺。

## 下载、缓存与应用

完整身份包含格式版本、来源 ID、Geo 种类、URL、更新间隔与大小上限。原始二进制缓存复用
应用 `core/rule-providers` 中带完整身份的缓存封装；不覆盖用户文件、内置 `.srs` 或共享 cache.db。
单文件 64 MiB，最多 16,384 分类、2,000,000 条域名/网段、单文本 4,096 字节；支持的数据
只有通过完整候选校验才提交。下载协调器的通用上限提高到 64 MiB，普通 rule/proxy provider
声明仍独立限制 8 MiB。已验证的 Geo 偏移索引最多缓存两份原始文件，每轮编译复用索引和 raw
快照，不按每条规则复制或下载整个数据库。

MMDB 搜索树使用紧凑网段索引，只为实际引用的分类生成 CIDR 文本。原始文件仍限 64 MiB，
元数据 128 KiB；遍历最多 16,000,000 次，最多 4,000,000 条分类网段、200,000 个分类。
单记录解码深度 64、65,536 个值、1 MiB 正文，全库解码 32,000,000 个值/256 MiB 正文；
指针展开也计数，超限整份拒绝。最多缓存两份已验证 MMDB 索引，以完整正文相等命中。

`geo-auto-update` 默认 false；true 时按 `geo-update-interval`（默认 24 小时，接受 0–8760，
0 为关闭）独立刷新，与订阅的 auto-update 无关。应用 Runtime 的单份任务每 30 秒检查，
最多处理三个到期声明，按完整身份轮换；失败以单调时钟退避 60/120/240/480/960 秒，
重开后从持久缓存时间恢复到期判断。修改/删除声明移除旧退避身份，失败保留原缓存时间。
调度线程只尝试获取运行锁，已有生命周期操作时跳过本轮；准备、提交和运行交接仍串行。
应用退出取消后续调度，并在准备/提交边界检查取消；已开始的同步下载受原有超时约束。

显式更新订阅强制检查该来源的依赖；外部资源行与 CLI 可强制只刷新一个 Geo DAT/MMDB，
其他过期资源复用已验证快照。其余必需快照缺失时提示先应用订阅，不顺带下载；未引用 Geo
也可显式/到期刷新。首次缺失多项依赖仍须先完整应用/更新订阅，不能用空缓存绕过候选检查。

```bash
clash-flux profile resources <profile-id>  # Geo 缓存状态，只读且不触发下载
clash-flux profile resource-refresh <profile-id> geo geosite
clash-flux profile resource-refresh <profile-id> geo geoip
clash-flux profile resource-refresh <profile-id> geo mmdb
clash-flux profile resource-refresh <profile-id> geo asn
clash-flux profile resources-update-due  # 执行一轮正常到期检查，不强刷未到期资源
```

下载使用任务线程、DIRECT 和默认证书验证，共用在途合并、条件请求、独占暂存、原文 guard
与完整候选检查。坏内容、缺少分类或来源在 IO 期间改变均保留旧缓存/订阅/活动配置；运行
交接失败恢复所选缓存。停止/未参与只更新缓存，不启动代理；桌面待命保留无业务监听。
运行中若原生配置字节改变，沿用完整候选交接，可能重启内核；相同内容跳过运行重启。
调度是应用层能力，继续记 approx，不宣称无损热替换。仅应用 Runtime 存活期间执行：桌面
隐藏窗口仍运行；完全退出不依赖系统服务替 UI 写库，Android UI 进程被系统回收后暂停，
再次打开按持久时间补查，不宣称 WorkManager/后台 libbox 中存在第二个调度器。
Android 的自定义 DAT/MMDB 在 UI 进程准备后生成后台消费的 JSON，不修改打包 GEO 资产或让后台写库；
异步交接不提前报告已应用，完整异步失败回滚仍未完成。多缓存文件提交不承诺崩溃事务。

## 资源状态

外部资源页对受支持的 DAT/MMDB 显示「尚未缓存 / 缓存可用 / 缓存无效」及有效原始正文大小，
手动刷新结束回读状态，定时更新成功发布应用级修订，已打开页面也回读。状态查询在任务线程有界读取并核对完整身份/格式，不下载、不提交，
不把缓存存在当作已应用或原生自动更新遥测。`profile resources <id>` 返回种类、名称、
缓存状态（missing/valid/invalid/unavailable）和原始字节数；目前 Geo 之外的缓存状态为
unavailable，仅表示没有该状态投影，不代表该 provider 不受支持。

## 验证入口

`singbox` 检查 protobuf/MMDB 类型、边界、双地址族、三种国家结构、ASN、坏指针、缺分类和来源隔离；
`resource_management_runtime` 经生产 CLI/固定内核检查真实 HTTP、逐项刷新、坏内容留旧、
过期响应拒绝与多 ID 保留；`personal_rule_sets_runtime` 启动固定内核，验证各域名匹配类型、
GeoIP DAT/MMDB、ASN MMDB、属性筛选实际规则拒绝、空筛选不误拦截及无关 loopback 流量直连。公共 HTTP(S)/TLS 拒绝仍由下载门禁覆盖。
本轮实际执行与未覆盖平台见[主线状态](l1-l2-l3-status.md)。

## 公开样本复核（2026-11-03）

从 MetaCubeX/meta-rules-dat 的 latest 发布下载以下快照，仅进入独占测试目录；未改依赖锁、
用户订阅或应用缓存。生产 `rule_provider_cache::Prepare` 提取 CN 分类，生成原生 inline
配置并通过固定 sing-box 1.14.4 `check`。该检查覆盖整份 DAT 的格式索引与所选分类转换，
不表示全部分类已做实际流量匹配或公网 VPN 验收。

| 样本 | 字节数 | SHA256 |
|---|---:|---|
| [geosite.dat](https://github.com/MetaCubeX/meta-rules-dat/releases/download/latest/geosite.dat) | 4,356,006 | `7297bf574a563aed8a69528e1f3a2bb275911fb83ebe3082494213d97ab09deb` |
| [geoip.dat](https://github.com/MetaCubeX/meta-rules-dat/releases/download/latest/geoip.dat) | 16,549,675 | `fbf747a085be25eb61ecac8c27140b4ab0bf8181af6fdb104caa325bd93cf117` |

latest 是可变发布地址；上表哈希只标识本次检查快照，不承诺后续同地址内容不变。

2026-11-03 后续使用上表同一快照复核 `GEOSITE,google@cn`：生产准备器从已验证快照
转换成功，固定内核 check 通过；未据此声称全部类别/属性已做实际流量验收。

## MMDB 公开样本复核（2026-11-03）

以下样本仅下载到独占测试目录；生产准备器转换所选分类，固定 sing-box 1.14.4 check 通过。
不将 latest 地址视为依赖固定版本，也不据此声称全部分类已做流量验收。

| 样本 / 所选分类 | 字节数 | SHA256 |
|---|---:|---|
| [MetaCubeX country.mmdb](https://github.com/MetaCubeX/meta-rules-dat/releases/download/latest/country.mmdb) / CN | 7,651,117 | `9a485c40b6fda868f3d3b981f928b8ab3d1e400fac1bf9cc8d27ebc08ae9bb0b` |
| [MaxMind 国家测试库](https://github.com/maxmind/MaxMind-DB/blob/main/test-data/GeoIP2-Country-Test.mmdb) / GB | 17,300 | `fc12d6470b35b70bd3e4534dd433d3e262a6c73f7682f85c344e832d4190f02b` |
| [MaxMind ASN 测试库](https://github.com/maxmind/MaxMind-DB/blob/main/test-data/GeoLite2-ASN-Test.mmdb) / AS15169 | 11,958 | `a0f9e8614ad582ba1af3eeb18ead666f73ad6e4cbd5280f80d5d4d95bbe6c3f1` |
