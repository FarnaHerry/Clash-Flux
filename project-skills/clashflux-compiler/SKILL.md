---
name: clashflux-compiler
description: 修改 Clash-Flux 订阅转换、协议/DNS/规则/provider 映射、保真度、来源编排、候选应用或 sing-box 能力边界时使用。
---

# 编译器、保真度与来源编排

先读 [保真度契约](../../docs/singbox-layers-and-fidelity.md) 的三层、账本、决策流程，再按下表只读相关基线章节。
该文档维护完整支持矩阵和字段语义，本技能不再复制矩阵。版本/revision/资产由 [稳定升级文档](../../docs/singbox-stable-upgrade.md) 与依赖锁维护。

## 不可突破的边界

- L1 紧贴真实 sing-box 能力、官方桌面内核与 libbox 同源码版本；不 fork 内核或造字段。Windows 数据面由服务启动，不能把“桌面内核”误解成 GUI 自行 spawn。
- L2 统一编译 Clash YAML、原生 JSON、PPTP/OpenVPN；每个协议/组/规则/字段标 exact/approx/unsupported，不能静默丢弃或为跑通改测速 URL、证书校验等用户语义。L3 使用 Clash 词汇，只暴露真实能力。
- 有原生等价用 exact；可表达但语义不同用 approx 并记账；无对应能力先降级记账。应用补齐须证明内核运行/未运行均有定义、失败能回滚且无第二真相源；做不到不做假 UI。
- 用户条目消失或语义改变用 ctx.note(scope,level,subject,detail,action)；语义不变的运行事件才 ctx.warn。编译失败仍保留已产生 fidelity，JSON 为空不能抹掉 MATCH/依赖错误。
- CompileResult.fidelity → CoreSnapshot.fidelity，warnings 只是文本投影；指定 profile 的只读保真度页不切活动来源，摘要 toast、分组 ! 角标、CLI profile check 各保留。toast 仅导入/刷新/启用等用户动作后发，不因启停/TUN/模式重编译重复弹。
- 启动与实例身份用 StartedService 鉴权 gRPC-Web 确认，原生控制面与兼容接口健康分开记录；clash_api 保留 REST/统计流，未迁移操作不宣称已迁移。

## 按映射领域读取基线

| 改动 | 保真度文档的相关章节与不可省略的核对 |
| --- | --- |
| 协议 / TLS / 凭据 | 出站协议、UDP/填充/HY2、SSH、HY1、SS 插件、AnyTLS/Snell。核对字段/传输白名单、异常成员整条拒绝、原文凭据、TLS 默认与平台支持；不能截断列表/trim 凭据或生成不存在字段。HY1 认证顺序/窗口/跳端口与大小写带宽单位、QUIC/uTLS、TUIC disable-sni、全局指纹继承及随机分布差异按基线记账。 |
| 组 / 代理链 / 拨号 | 组类型、代理链、原生拨号字段及“已知边界”。核对 include-all-proxies 只展开本来源成功顶层节点、顺序/重复/空组及过滤拒绝；urltest 无手动锁定，fallback/load-balance 降级；长 interval/idle_timeout、tolerance=0 和未映射组字段不得假装等价。detour/组成员/DNS 共同查缺失、重复 tag、环；平台 bind_interface/routing_mark 不因 Android 的 __linux__ 开放。 |
| DNS / hosts / TLS URL | DNS 基线和 hosts/policy/TLS 示例。核对从右到左 trie 次序、*/./+. 分支、后声明覆盖、节点 resolver 与普通 DNS 分离、端点 TLS 例外不扩散、name-cert-verify 不改 SNI、hosts 精确/完整数组/拒绝规则、TTL/缓存和再次 resolve 排除；来源命名空间同步 hostsDnsTag，不硬编码旧 tag。 |
| 规则 / inline provider | 规则基线；AND/OR/NOT 原子转换，不删除坏子条件扩大匹配，不将失败 provider 换同名国内别名。route/HeadlessRule 分 schema，Android 包名与 Linux UID 分平台；首个有效 MATCH 终止，DIRECT 不替换，坏兜底失败、REJECT-DROP 原生 drop，no-resolve 与未知修饰符按基线记账/拒绝。 |
| GEO / file / HTTP / ASN / MRS | 本地 file provider、HTTP 缓存快照章节。编译器只读缓存/输出资源需求，不联网或任意清理；读取根/符号链接/大小边界、完整原子转换、完整缓存身份、请求头秘密、TLS/重定向及失效行为按文档执行。ASN 展开与 MRSv1 domain/ipcidr 子集记账，不宣称 classical/未来版本已支持。 |

## 下载与候选提交

- GEO 仅处理 CompileResult.ruleSetResources 自有清单，任务预取/周刷新/坏缓存清理，不扫描原生 tag 拼路径或清理无关 .srs；Android 打包 GEO 不参与运行时更新/清理，SRS 头检查不等于完整解析。
- file provider 仅从显式 ruleProviderDir 读取根内普通相对文件，canonical 后不越界，8 MiB；不写源文件或纳入 GEO 清理，失败整份候选拒绝。
- HTTP provider 编译器输出 httpRuleProviders，任务按 HTTP(S)、直连/DIRECT 及文档支持的 header 准备快照。单值非空 ASCII header 总量 16 KiB，值参与完整缓存身份、不进配置或诊断；带头禁重定向并记 approx，iOS 暂拒带头，Android 用 Java TLS 桥接。
- 下载默认验证证书，8 MiB 或更小 size-limit 在传输时拒绝、不截断提交；全部来源独占暂存、整份转换/候选与目标检查成功才原子换自有缓存，失败保留旧缓存/订阅。path 只作数据目录内只读种子，不改用户文件。
- 缓存检查完整身份/格式，摘要文件名不是安全验证；未来版本/冲突拒覆盖。运行计划持 raw 快照，回滚不重新下载；interval 仅应用配置时检查到期，记 approx。没有多文件崩溃事务，不能宣称完整崩溃恢复。
- ASN 固定 ipverse JSON 编号/双地址族 CIDR 完整检查；MRS 固定 Zstandard，输入 8 MiB、解压 32 MiB、集合有界；未知版本/扩展/畸形或不支持域名整份拒绝。完整候选通过前不提交任何暂存缓存。
- 通用原子替换、订阅保护、Windows 不先删目标、Android 无桌面 CLI 预检查等遵守 [数据技能](../clashflux-data/SKILL.md)。

## 桌面编排

改编排前读 [桌面订阅编排](../../docs/desktop-subscription-orchestration.md)。唯一主订阅 + 按启用规则参与的次来源；手机仍单活动普通来源，UI/CLI/导入/写入口按平台限制，不随视口变化。
规则层次固定：前置动作 → UserOverride → SourcePolicy → 模式规则 → MainPolicy → MainFallback，priority 仅同层比较后按 order。
不可用目标保留匹配并 Reject（默认）/UseMain（默认出口、不重跑主规则）/Direct，无启用规则不载入次来源。
身份 source ID + kind + 原唯一名称；改名引用失效，不宣称对象 UUID；policy format_version=2，v1 保序迁移，拒未知/未来版本，不顺手改 SQLite schema。
候选先 check 再停旧实例，启动失败仅旧原生资源有效才恢复；planRevision 是成功应用运行号，预览不能覆盖运行映射。
规则编辑器对象目录只读、独立于运行/预览，包含未参与 Clash 来源；异步核对请求号/来源/内容修订，切源清旧选择，失效引用保留原名，不自动选首项/跨来源同名对象。
原生 JSON 次来源、跨来源原始 detour 与完整崩溃恢复不能宣称已完成；原生连接/JSON 和手机仍只开放默认出口，当前完成度见 [状态记录](../../docs/l1-l2-l3-status.md)。

改映射/升级内核同步保真度基线与真实完成度，按 [测试技能](../clashflux-test/SKILL.md) 做编译/固定内核运行回归；交互研究按需查 [官方 GUI 研究](../../docs/singbox-official-gui-review.md)，不作为已实现功能承诺。
