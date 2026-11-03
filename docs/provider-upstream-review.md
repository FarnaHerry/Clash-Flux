# Provider 下载与更新：上游源码对照

复核日期：2026-11-02。应用快照：`9266acf` 加当前未提交工作区。
复核本身仅研究、记录，没有修改下载、刷新或运行控制逻辑，也没有性能对照实验；
后续按结论实施的子集见文末“实施状态”。

## 研究范围与来源

- Clash Verge Rev：正式 tag `v2.5.8`；区分应用自己的主订阅下载与 Mihomo provider 下载。
- Mihomo：`Meta` 在本次复核时的 `3f27ef39f8c96bbfbc8b0b9bb10f5c9ef60149d8`。
  所列 fetcher、vehicle、HTTP 和初始化源码已与该 SHA 逐字节核对；不推定所有 Verge 安装包使用此内核快照。
- sing-box：应用固定版本 `v1.14.4`，研究官方内核的 remote rule-set、HTTP Transport 和更新调度；
  未研究官方 Android/Apple 客户端主订阅下载，不把 rule-set 等同于 Clash proxy-provider。

## Clash Verge Rev 与 Mihomo

Verge 的 provider 按钮调用内核 API；“更新全部”在循环中逐个 await，因此这个手动入口串行。
主订阅下载器每次创建 reqwest Client，显式设置 `pool_max_idle_per_host(0)`；
`tcp_keepalive` 不是 HTTP 连接池，不能据此声称它依靠长驻连接池加速主订阅下载。
证据：[provider-button.tsx](https://github.com/clash-verge-rev/clash-verge-rev/blob/v2.5.8/src/components/proxy/provider-button.tsx)、
[network.rs](https://github.com/clash-verge-rev/clash-verge-rev/blob/v2.5.8/src-tauri/src/utils/network.rs)。

Mihomo 的 HTTPVehicle 走公共 HttpRequest；本次源码每次调用新建 Transport/Client，
不是多个 provider 共用的长驻 Transport 池。Android 还显式关闭该 Transport 的 KeepAlive。
有此实现不等于没有网络成本，也不能解释为“建了 Transport 就一定跨请求复用连接”。
证据：[vehicle.go](https://github.com/MetaCubeX/mihomo/blob/3f27ef39f8c96bbfbc8b0b9bb10f5c9ef60149d8/component/resource/vehicle.go)、
[http.go](https://github.com/MetaCubeX/mihomo/blob/3f27ef39f8c96bbfbc8b0b9bb10f5c9ef60149d8/component/http/http.go)。

Fetcher 优先解析有效本地缓存，恢复其修改时间后启动自己的刷新循环；过期缓存可以先供初始化使用，
再在循环中更新。各 provider 的 interval 与主订阅更新分离；网络或解析失败不发布新内容，
按退避时间安排重试。正文哈希相同不再运行 parser/onUpdate，只更新时间；ETag 与对应正文哈希绑定，
304 沿用已有内容。初始化并发受平台构建选择影响：该快照 amd64/arm64 无额外数量上限，
部分架构为 5，mips 为 1；不能笼统写成统一固定并发数。
证据：[fetcher.go](https://github.com/MetaCubeX/mihomo/blob/3f27ef39f8c96bbfbc8b0b9bb10f5c9ef60149d8/component/resource/fetcher.go)、
[executor.go](https://github.com/MetaCubeX/mihomo/blob/3f27ef39f8c96bbfbc8b0b9bb10f5c9ef60149d8/hub/executor/executor.go)、
[并发构建选择](https://github.com/MetaCubeX/mihomo/blob/3f27ef39f8c96bbfbc8b0b9bb10f5c9ef60149d8/hub/executor/concurrent_load_unlimit.go)。

## sing-box 1.14.4

HTTPClientManager 按显式名称缓存共享 Transport，默认 Transport 也可共用；内联配置或特殊
ResolveOnDetour 路径可能创建独立 Transport，不是所有请求无条件共享。网络重置会重置托管 Transport。
共享引用跟踪活跃使用方，空闲连接在使用方全部结束时回收；启动批次结束与运行期 fetch 都有释放动作，
不能把它概括为跨所有更新周期永久保留连接的池。
证据：[manager.go](https://github.com/SagerNet/sing-box/blob/v1.14.4/common/httpclient/manager.go)、
[managed_transport.go](https://github.com/SagerNet/sing-box/blob/v1.14.4/common/httpclient/managed_transport.go)、
[adapter/http.go](https://github.com/SagerNet/sing-box/blob/v1.14.4/adapter/http.go)。

启动规则集用 task.Group，最多并发 5 个，失败可快速结束该批次。remote rule-set 先恢复
URL 身份匹配、可解析的持久化缓存；没有缓存时可读取 initial_path，否则必须首次下载成功。
默认更新周期 24 小时；运行期统一 RuleSetUpdater 维护各资源的下次时间，在单个循环中逐个
更新到期规则集。这个版本运行期没有给每个规则集各起一个常驻更新 goroutine。
证据：[router.go](https://github.com/SagerNet/sing-box/blob/v1.14.4/route/router.go)、
[rule_set_updater.go](https://github.com/SagerNet/sing-box/blob/v1.14.4/route/rule/rule_set_updater.go)。

远程规则集发送 If-None-Match，304 更新时间并保留已解析规则；200 完成解析、规则构造和
元数据检查后才交换运行规则并通知引用方。下载或解析失败不交换这份规则；该资源更新不要求
重启整个代理内核。启动缺失资源时仍可能失败，不能把运行期沿用旧规则理解为首次缺失也放行。
证据：[rule_set_remote.go](https://github.com/SagerNet/sing-box/blob/v1.14.4/route/rule/rule_set_remote.go)。

## 对 Clash-Flux 的结论与执行顺序

这是依据源码形成的优化建议，不是测得的性能收益；实现与验收仍需单独交付。

1. **先分开更新意图。** 当前桌面 download 和 Android completeRemote 都把远程订阅刷新
   作为 provider 强刷，自动更新也共用这些入口。自动更新应尊重 provider 自己的 interval；
   保留用户此前要求的手动更新同步强刷语义，并限制到本次相关资源，避免扩大到无关来源。
2. **复用同一次准备结果。** provider 准备内部已检查候选，部分调用方仍重复检查；运行中
   更新又进入应用准备。只在来源/策略版本、确切配置字节和检查前提都一致时复用结果；
   selector 投影等改变了最终字节时仍检查最终配置，不删除必要校验来制造提速。
3. **资源管理独立于页面。** 应用作用域管理资源身份、缓存、到期调度和同资源在途请求合并；
   初次缺失资源阻塞候选，后台失败保留旧缓存，更新完成按版本发布。主订阅文件不成为另一个
   provider 缓存所有者，编译器继续只读缓存，不联网。
4. **再测量连接复用与队列。** 当前是每轮 3 路并发、proxy/rule/新依赖分轮等待，桌面每次
   下载创建 curl easy handle。可评估有界下载队列、批次 Transport/easy 复用；不能仅因上游
   使用 Go/reqwest 就推定存在更强连接池。复用必须隔离代理、TLS 和鉴权策略，网络切换可失效。
5. **保持 L1/L2 边界。** 原生 remote rule_set 仍交 sing-box 下载与更新；Clash YAML/MRS
   规则和节点 provider 的转换归 L2。后续本地 SRS 与内核热重载另行设计，不能直接把 Clash
   provider URL 当 sing-box 原生规则集，也不能宣称已有任意节点增删的无损热更新接口。

分段计时应分别覆盖主订阅下载、provider 排队/网络/解析转换、候选检查和运行应用；
记录缓存命中、304、实际建连/复用次数与字节数，不输出 URL、请求头或凭据。
现有 profile-providers 总耗时不能直接当作网络耗时，也不能代替上述阶段的对照测量。

## 实施状态（2026-11-02）

- 第 1 项已落地：`CompileOptions.forceProviderRefreshSources` 按来源 ID 强刷；桌面
  `download`/`refreshDue` 和 Android `completeRemote`/自动更新泵区分手动与自动意图。
  手动更新只强刷本次来源自己的代理/规则集合，自动更新不再强刷。
- 第 2 项的调用方复用已落地：provider 准备记录 `candidateChecked`，同一最终字节不再
  重复执行候选检查；selector 投影或 GEO 重编译后仍检查最终配置。`saveYaml` 验证后仍
  重新进入运行准备，未做跨阶段 PreparedStart 复用。
- 第 3 项首批已接入应用级下载入口和在途请求表；第 4 项已补排队/网络/验证/检查、
  实际请求、字节与桌面建连计数，契约见 [资源管理与计时](provider-resource-management.md)。
  没有新增独立 provider 调度器、Transport/easy 池或本地 SRS/热重载路径，也未做公网性能对照。
- 验证与剩余边界见 [状态记录](l1-l2-l3-status.md) 同日“Provider 更新意图与候选检查复用”
  及“Provider 资源协调与分段计时（第一批）”。

2026-11-03 后续：第 3 项独立到期调度已接入应用 Runtime，显式 HTTP provider 与自定义
Geo DAT/MMDB 按各自间隔刷新，复用单项暂存/校验/来源 guard 和候选交接；失败保留旧快照
并退避。相同原生字节跳过重启，变更可能重启内核，不等同于 sing-box 原生 remote rule_set
无损交换。平台存活范围与验收见[资源调度契约](provider-resource-management.md#独立到期调度2026-11-03)。
