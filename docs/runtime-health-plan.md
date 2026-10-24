# 运行健康检查优化计划

日期：2026-10-24。审查基线：`41c9291`；工作区已有首页猫咪按钮焦点修复。
状态：**计划，尚未实施健康检查改动**。本文不把代码缺口认定为用户偶发问题的已证实根因。
2026-10-24 补充：检索官方客户端及固定内核源码，加入订阅语义、错误分类和生命周期对照。

## 目标与本轮范围

用户在多个平台遇到两类主订阅问题：网络正常但首页流量停止更新，以及网络访问也失败、
界面仍显示内核运行。先提高识别、诊断和观测通道恢复能力，让下次出现问题时有明确证据。

本轮优先做有界、可复现的健康检查与统计通道恢复。PPTP 拨号、主订阅切换、TUN 路由补偿、
订阅编译和持久化事务保持现有流程；iOS 继续暂缓。所有自动恢复仅作用于观测通道，
不自动重启内核、不切换订阅/节点、不关闭业务连接、不重建 TUN、不重设系统代理。

## 已核实的现状

| 链路 | 当前实现 | 可改进点 |
|---|---|---|
| 桌面运行状态 | `CoreStore::checkAlive()` 调用 `coreAlive()`；服务托管先查 `/version`，失败再问服务；接管实例看 pidfile，无 pidfile 时信任；直接启动看进程句柄 | 存活证据强度不同；服务查询的 bool 把通信失败与明确停止合并；运行状态不能证明出口可达 |
| 桌面统计 | `CoreStreams` 的 `/traffic`、`/connections` 经 IXWebSocket 推送，已有 1–30 秒自动重连退避 | `open` 不代表仍有有效帧；没有完整的新鲜度、停更原因和通道恢复记录 |
| 首页展示 | `SubscribeHomeStreams()` 消费统计，推送通过 `SubscribeStreamUpdates()` 投递到 UI | 缺少“已收帧—已解码—已消费”的关联证据，难分后台停更与 UI 通知异常 |
| Android | 后台 `ClashVpnService` 的 CommandClient 每秒订阅 status，Binder 将缓存快照送至 UI/JNI | 快照未携带 status 接收时间/序号；`disconnected()` 当前仅记日志，缓存的非空 client 不能证明通道仍正常 |
| 网络恢复 | Android 已处理物理接口、DNS、亮屏及 Doze 变化 | 健康检测要复用事件并设置恢复宽限，不能再叠加一套连接重置机制 |

主要代码入口：

- `src/store/core_store_helpers.inc`、`core_store_lifecycle.inc`：存活及生命周期。
- `src/stream.cppm`、`src/stream.cpp`：桌面观测通道及通知注册表。
- `src/api.cppm`、`src/api.cpp`：本地控制 API、超时与鉴权。
- `src/ui/common.cpp`、`src/ui/core_model.h`：唯一应用级状态泵、版本校验和模型。
- `src/ui/home_page.cpp`、`src/ui/stream_updates.h`：统计消费与 UI 线程桥接。
- Android 的 `ClashVpnService.java`、`RuntimeControlService.java`、`MainActivity.java`、
  `IClashRuntime.aidl`、`platform/android/android_bridge.cpp`：后台状态流与跨进程快照。

## 官方客户端检索与采用边界

### 研究方法和版本

只使用 SagerNet 官方仓库及官方文档。读取源码，没有运行官方客户端或验证官方客户端的实机恢复。
本节研究运行健康和连接可靠性，不把服务可达性当作证书、隐私或网络安全的证明。
客户端当日分支与本项目固定内核分别核对，不因官方客户端版本变化而升级本项目依赖。

| 研究对象 | 固定源码快照 |
|---|---|
| 官方 Android 客户端 | `f87c2ebdb7e34754be751bb5e58926db2fed8f8c` |
| 官方 Apple 客户端 | `2dd154e7586188d5fff8de615e8c821ed25ec089` |
| 官方桌面宿主 | `8aab8eab022366e1032e952d499c9252b833e6c3` |
| 官方 dashboard（独立审查的快照） | `3e4bf08e5bf5b9417cf9c36e5a2d5abcb21bdd34` |
| Clash-Flux 固定 sing-box 1.14.3 | `7054cac5124465ffdb145870d7c01885566711b1` |

桌面宿主通过 `@dashboard/App` 使用共享界面，daemon adapter 使用 gRPC；
研究共享 dashboard 的订阅策略，不声称上表的独立 dashboard HEAD 就是桌面宿主当日锁定的子模块 SHA。
宿主依据：[renderer/main.tsx](https://github.com/SagerNet/sing-box-for-desktop/blob/8aab8eab022366e1032e952d499c9252b833e6c3/src/renderer/src/main.tsx)、
[main/daemon.ts](https://github.com/SagerNet/sing-box-for-desktop/blob/8aab8eab022366e1032e952d499c9252b833e6c3/src/main/daemon.ts)。

### 已核实的官方做法

下表“本项目采用”是设计决策，不是官方已有能力的声明。证据链接固定到以上 revision。

| 主题 | 官方源码事实与证据 | 本项目采用 |
|---|---|---|
| 服务状态与流量分离 | dashboard 分别订阅 `serviceStatus`、`status`，并读取 `trafficAvailable`。[daemon.ts](https://github.com/SagerNet/sing-box-dashboard/blob/3e4bf08e5bf5b9417cf9c36e5a2d5abcb21bdd34/src/api/daemon.ts)、[OverviewView.tsx](https://github.com/SagerNet/sing-box-dashboard/blob/3e4bf08e5bf5b9417cf9c36e5a2d5abcb21bdd34/src/views/OverviewView.tsx) | 保留内核、控制面、采样和统计能力四种独立证据；统计不可用不冒充零速率 |
| 可恢复与终止性错误 | dashboard StreamStore 按 1 秒递增、最多 5 秒退避；收到数据才重置尝试数。Unimplemented、NotFound、Unauthenticated、PermissionDenied 停止该重试循环。[stream.ts](https://github.com/SagerNet/sing-box-dashboard/blob/3e4bf08e5bf5b9417cf9c36e5a2d5abcb21bdd34/src/api/stream.ts) | 借鉴错误分类及取消；本项目现有 IX 退避不改成另一套重复重试，修复永久错误需显式重新触发 |
| 短暂故障提示宽限 | dashboard 对可恢复流错误采用 6500ms 展示宽限，终止性错误立即展示。[hooks.ts](https://github.com/SagerNet/sing-box-dashboard/blob/3e4bf08e5bf5b9417cf9c36e5a2d5abcb21bdd34/src/app/hooks.ts) | 借鉴防闪烁和错误保留；故障展示宽限与有效帧超时分别计时，不照搬横幅布局 |
| 控制面可达性 | dashboard 的服务器探测收到首条 `SubscribeServiceStatus` 即返回；这不是访问公网的测试。[reachability.ts](https://github.com/SagerNet/sing-box-dashboard/blob/3e4bf08e5bf5b9417cf9c36e5a2d5abcb21bdd34/src/lib/reachability.ts) | 控制健康必须完成鉴权和预期协议响应；TCP 连接成功不够，控制成功也不宣称业务可达 |
| libbox 建连与订阅恢复不同 | 固定内核的 CommandClient 有建连探测重试；`Connected()` 在分发订阅之前调用，各流 Recv 失败后调用 `Disconnected()` 并退出该流读取函数。[command_client.go](https://github.com/SagerNet/sing-box/blob/7054cac5124465ffdb145870d7c01885566711b1/experimental/libbox/command_client.go) | 握手只说明传输建立，必须等待所需订阅的首个有效样本；不能依赖 gRPC 底层重连自动复活已退出的应用订阅 |
| 旧连接回调隔离 | Android CommandClient 使用 `connectionEpoch`，Apple 使用 `activeConnectionToken`，连接工作放到异步执行路径。[CommandClient.kt](https://github.com/SagerNet/sing-box-for-android/blob/f87c2ebdb7e34754be751bb5e58926db2fed8f8c/app/src/main/java/io/nekohasekai/sfa/utils/CommandClient.kt)、[CommandClient.swift](https://github.com/SagerNet/sing-box-for-apple/blob/2dd154e7586188d5fff8de615e8c821ed25ec089/Library/Network/CommandClient.swift) | 为每次控制连接增加独立 generation，与内核实例 epoch 分开；全部回调、首帧及重连完成均验身份 |
| 主动暂停订阅 | Android 通知栏速率订阅在息屏时断开、亮屏时连接；Apple 的 UIKit 远程连接有后台 suspend/前台 resume。[ServiceNotification.kt](https://github.com/SagerNet/sing-box-for-android/blob/f87c2ebdb7e34754be751bb5e58926db2fed8f8c/app/src/main/java/io/nekohasekai/sfa/bg/ServiceNotification.kt)、[CommandClient.swift](https://github.com/SagerNet/sing-box-for-apple/blob/2dd154e7586188d5fff8de615e8c821ed25ec089/Library/Network/CommandClient.swift) | 显式记录预期暂停和订阅所有者，暂停不触发 watchdog；本项目后台共享通道不能照搬“息屏全断开” |
| 周期采样与事件流不同 | 固定内核的 `SubscribeStatus` 周期发送；`SubscribeConnections` 在没有更新事件时可以不发；Clash API 的 `/connections` WebSocket 默认每秒发快照。[started_service.go](https://github.com/SagerNet/sing-box/blob/7054cac5124465ffdb145870d7c01885566711b1/daemon/started_service.go)、[clashapi/connections.go](https://github.com/SagerNet/sing-box/blob/7054cac5124465ffdb145870d7c01885566711b1/experimental/clashapi/connections.go) | 按实际 adapter 声明周期和能力；Android 连接事件、组和日志的静默不能套桌面快照的 5 秒规则 |
| 物理网络与休眠 | Android 更新默认接口并将 Doze 状态传给 command server 的 pause/wake。[DefaultNetworkMonitor.kt](https://github.com/SagerNet/sing-box-for-android/blob/f87c2ebdb7e34754be751bb5e58926db2fed8f8c/app/src/main/java/io/nekohasekai/sfa/bg/DefaultNetworkMonitor.kt)、[BoxService.kt](https://github.com/SagerNet/sing-box-for-android/blob/f87c2ebdb7e34754be751bb5e58926db2fed8f8c/app/src/main/java/io/nekohasekai/sfa/bg/BoxService.kt) | 网络不可用、设备休眠及显式暂停单列，恢复后重开采样宽限；复用本项目现有网络事件 |
| 主动测试独立于运行状态 | dashboard Tools 提供用户启动、可取消的网络质量测试，指定 outbound 和运行预算；URLTest 另有 interval/idle_timeout 语义。[ToolsView.tsx](https://github.com/SagerNet/sing-box-dashboard/blob/3e4bf08e5bf5b9417cf9c36e5a2d5abcb21bdd34/src/views/ToolsView.tsx)、[URLTest 官方文档](https://sing-box.sagernet.org/configuration/outbound/urltest/) | 诊断是独立按需动作；不把组测速的空闲停测或某目标失败当成主内核故障 |

**研究结论的限制：**上述文件未显示一个可以由“界面显示 Started”直接推导整个代理路径健康的统一保证，
也不足以证明官方客户端能自动修复所有“进程还在但业务断网”的情况。
本计划的帧新鲜度 watchdog、恢复预算和分层诊断是我们结合现场问题增加的设计，不能标成官方现成实现。
官方用 gRPC daemon 的事实不构成本项目迁移桌面控制架构的理由：继续使用 clash_api / Android libbox，
新增接口只采用本项目固定 revision 实际可用且已验证的能力。

## 健康状态的含义

保留 `CoreState` 的生命周期语义，新增独立的健康快照，不把所有问题塞进 `Failed` 或 `lastError`。

| 维度 | 建议状态 | 能说明什么 |
|---|---|---|
| 内核实例 | Unknown / Alive / Exited | 服务、进程或后台运行时提供的存活证据 |
| 控制面 | Unknown / Responsive / Unreachable / AuthenticationError | 本地控制端点或 Binder 是否可用；鉴权失败单独归因 |
| 统计通道 | Unsupported / Paused / Waiting / Fresh / Stale / Reconnecting / Blocked | 依据订阅契约判断有效样本；暂停、能力不支持和永久错误不反复重连 |
| 接管状态 | Unknown / Active / Inactive / Mismatch | 用户配置意图与实际系统代理/TUN 证据是否一致 |
| 业务可达性 | NotChecked / Checking / Reachable / Failed / Inconclusive | 指定目标、指定路径在本次诊断中的结果，不能泛化为“整个网络正常” |

规则：

- 新鲜的 `0 B/s` 是合法空闲采样。业务零流量、总量未增长、空连接表都不是故障依据。
- “进程存在 + 控制面不通”归为控制面异常；查询超时归为 Unknown/Unreachable，
  只有权威退出证据才把内核标为 Exited。既有 `checkAlive()` 调用者也必须遵循该区分。
- 同时支持多个维度异常：不能用单个优先级标签掩盖统计停更和网络失败的并存。
- 缺少证据明确保留 Unknown/NotChecked，不展示伪造的健康状态。
- 通道连接、首个有效样本和统计能力分别记录。`trafficAvailable=false` 时 status 本身可以是新鲜的，
  速率数据则不可用；不能因为不支持统计而无限重连，也不能把传输握手当成 Fresh。
- 权限/鉴权/接口不支持按实际协议分类；停止旧连接导致的取消回调属于正常收尾，不计入异常和修复次数。

## 实施顺序与交付

### M1：先补证据和可测试的状态判定

1. 提取普通 C++ 的健康状态与判定器（建议 `src/runtime_health.h/.cpp`）。
   注入时钟和探测结果；判定器不做网络、系统修改、数据库访问或 UI 操作。
2. 为每次实际内核实例生成独立 `runtimeEpoch`，不只使用可能复用的 `planRevision`。
   探测请求关联实例 epoch、操作版本、网络 epoch 和请求序号；旧实例/旧网络/旧请求结果拒绝发布。
3. 记录通道连接状态、最后收到帧时间、最后有效解码时间、有效帧序号，以及 UI 最后消费序号。
   收到畸形帧不能刷新“有效帧新鲜度”。连接统计零值仍更新序号。
   每种 adapter 声明周期流/事件流、请求的采样周期、所需订阅、首样本到达、能力及主动暂停状态。
   周期流才使用帧年龄 watchdog；事件流通过控制面、错误回调和显式读取诊断，静默不等于过期。
4. 用单调时间判断超时。快照保存采样时间，界面不逐帧写入“已过几秒”触发重组。
   Android 在 Java 内用 `elapsedRealtime` 计算年龄后传递，禁止直接比较 Java 与 C++ 时钟纪元。
5. 增加有界诊断记录：异常出现、恢复尝试、恢复结果、状态恢复；正常心跳不逐秒记日志。
   每条关联实例、通道、错误类别、耗时和计数，不记录 token、订阅 URL、凭据、完整配置或连接目标清单。

交付：能从日志分清“未收到帧”“解码失败”“已收到有效帧但 UI 未消费”。
先形成可复现证据，不宣称已经修复用户现场的全部断流。

### M2：桌面统计通道检测和有限自愈

1. 在应用级健康调度中读取 M1 的不可变快照，复用现有唯一状态泵的发布入口。
   页面不增加定时器；耗时探测在任务线程执行，不能堵住原有状态读取、停止或退出操作。
2. 对预期持续推送的 `/traffic`、`/connections`，检测长时间没有有效帧的情况。
   `/logs` 可以长期没有业务日志，不能用相同的静默阈值重连日志通道。
   这里的 `/connections` 指桌面 clash_api 周期快照，不包括 libbox 的连接事件订阅。
3. 统计停更后，先进行有界的本地控制面检查，再决定是否重连对应通道。
   API 不可达或鉴权失败分别记原因；不借此重启内核。
4. 已在 IXWebSocket 自动重连中的通道由原退避负责，watchdog 不反复 stop/start 绕过退避。
   仅对“已连接但有效数据停更”等现有重连覆盖不到的情况，按预算重建该统计通道。
5. 重建只替换目标通道，保留最后有效累计值并标记过期；恢复后以权威快照校正，
   不把缺失样本填成零，也不重复累计重连前后的总量。
6. 通道生命周期在单一所有者上串行管理；UI 只发请求。
   不在 WebSocket 回调中停止并 join 自己的线程，不在 UI 线程或持有共享快照锁时 join。
7. 核对通知合并/确认的生命周期、异常和卸载竞态，用真实挂载/卸载与可控回调回归；
   只有复现后才修改通知算法，不以“再加一条页面轮询”掩盖通知故障。
8. 借鉴官方 StreamStore，区分临时传输错误、鉴权失败、权限拒绝、明确不支持的端点及正常取消。
   终止性错误进入 Blocked/Unsupported，等待 endpoint、凭据、能力版本变化或用户显式重试，
   不用冷却到期反复重试原请求。保留最后错误和有效数据，恢复时才清除。

交付：统计通道可独立恢复；关闭/重启内核不会被旧 watchdog 请求重新连接。

### M3：Android 后台状态流与 Binder 的新鲜度

1. 后台快照增加 runtime epoch、status 序号、status 年龄、CommandClient 连接状态。
   UI 最近成功读取 Binder 的时间与后台最近收到 libbox status 的时间分别记录。
   Binder 返回成功不能把旧 status 伪装成新数据。
2. 在 `:background` 的服务工作线程串行处理 CommandClient 的断开、失效、重新创建与订阅。
   固定 libbox 已核实有建连重试，但流读取失败后的订阅循环会退出；应用需重建失效订阅，
   不能只检查 `client != null`。不要把一次多流回调分别排成多个全量重建任务。
   旧 client 的晚到回调必须携带/校验 generation，不能污染新实例。
   `Connected()` 之后先进入 Waiting，收到必需 status 样本才确认采样恢复。
   当前 libbox `Disconnected` 回调只提供字符串、且可来自任意订阅，不能凭中文/英文错误文本
   硬判全部通道死亡或终止性错误；结合订阅能力、status 新鲜度与有界验证，无法归因时保留 Unknown。
3. 只修复控制/统计订阅，不重新启动 libbox、不关闭 TUN fd，不把 core-only 报成 VPN 已连接。
4. UI 解绑、Activity 停止、进程回收和系统休眠保留各自语义。没有 UI 时不增加高频 Binder 轮询；
   后台 watchdog 不新增 wake lock。亮屏、网络变化、退出 Doze 后进入宽限并重新采样。
   借鉴官方按消费者暂停观测的思路，但不直接照搬通知栏息屏断开到本项目共用的 CommandClient；
   必须保留后台运行控制和其它所需订阅。主动暂停状态进入 Paused，不累计故障时长或重试预算。
5. 沿用 AIDL/Binder 和原子快照文件边界，后台不打开 SQLite，不使用 SharedPreferences 跨进程同步新健康状态。

交付：明确区分后台 status 停更、Binder 不可达和 UI 尚未绑定；后台观测恢复不影响正在工作的代理。

### M4：用户可读诊断与按需连通性检查

1. 提供独立“运行诊断”入口，展示各维度证据、采样年龄和最近恢复记录，支持脱敏导出。
   首页不新增“内核未运行/请去设置启动”的常驻横幅；异常不每秒 toast，用户主动诊断才给完整反馈。
   若新增文案，同步 default/en/zh 三份资源。
2. 用户主动检查时，用有界请求区分本地控制面、统计通道、接管状态和指定出口的目标可达性。
   使用当前实际监听端口、实际来源/出站映射，不能拿设置端口或其它节点的测速结果替代。
   借鉴官方“收到类型化服务状态才认控制面可达”：本项目须验证实际端点的鉴权和预期响应结构。
   不照搬浏览器 CORS/mixed-content 诊断到原生客户端，也不新增远程监听端口或扩大权限来做健康检查。
3. 优先复用现有明确配置的测试目标；没有合适目标则由用户指定，不暗中增加固定外站定时访问。
   单目标失败只说明该路径/目标失败，区分 DNS、连接、TLS、HTTP 与取消；证据不足返回 Inconclusive。
4. 节点测速只能说明该节点到该目标的测试路径，不能冒充整个规则链、系统代理或 TUN 的端到端验收。
   系统 DNS 与内核 DNS 的测试路径也要明确；无法独立验证的环节如实标为未检查。
5. 第一轮不自动重启内核、切节点或关闭全部连接。后续若需要数据面恢复策略，
   单独设计触发证据、冲突处理、用户可控入口及回滚，不混入本计划的统计自愈。

交付：两类现场问题可以导出不同证据；不承诺仅凭一次主动测试锁定根因。

## 初始参数与调度约束

以下为待故障注入及真机验证的初值，不是已验证性能指标：

| 项目 | 建议初值与约束 |
|---|---|
| 健康判定节拍 | 前台/桌面复用约 1 秒的应用泵；Android 后台结合平台生命周期，不为观测唤醒设备 |
| 首次采样/网络恢复宽限 | 10 秒；从实例就绪或网络恢复开始，正常启动耗时不计算为断流 |
| 统计停更阈值 | 仅已启用的周期流：初值 max(5 秒, 3 × 请求采样周期)；事件流/主动暂停不适用，零速率帧保持新鲜 |
| 本地控制面检查 | 专用约 1 秒总预算，单飞；不沿用普通请求默认 10 秒超时拖慢状态泵 |
| 通道修复预算 | 同一通道最多 3 次/5 分钟，最短 30 秒间隔；实际连续失败采用退避并合并重复请求 |
| 恢复确认 | 连续 2 个有效样本后恢复 Fresh；仅握手成功不清除异常计数 |
| 终止性错误 | 权限/鉴权/明确能力不支持不自动重试；配置或能力变化、用户显式重试再解除阻断 |
| 故障展示宽限 | 暂定 6.5 秒，借鉴官方 dashboard；与采样停更阈值独立，终止性错误主动诊断时直接显示 |
| 主动网络诊断 | 显式发起、可取消、总预算不超过 10 秒，限制目标数和响应体大小 |

停止/重启、切订阅、TUN 操作及服务维护期间，取消或挂起冲突探测与通道修复。
新实例就绪重新进入宽限。网络/API 调用不持有模型锁或生命周期锁；需要串行访问的
客户端先捕获不可变 endpoint/身份，再在独立有界任务中执行，完成后校验版本才发布。
超时不允许堆积新任务，退出不等待无限期探测；恢复预算及错误计数归应用/后台运行时所有，
不能因切页、重挂载或重新打开窗口而清零。

## 回归与验收

| 场景 | 必须证明的行为 |
|---|---|
| 持续收到零速率帧，运行至少 10 分钟 | 始终 Fresh，不重连、不重启、不产生故障提示 |
| WebSocket 正常关闭/连接被拒绝 | 由原重连机制恢复，watchdog 不形成重连风暴 |
| 握手成功但服务器停止发送统计 | 在宽限外按阈值识别停更；只修复受影响的通道 |
| 连续收到畸形统计帧 | 不覆盖最后有效值，不伪装 Fresh，诊断指出解码问题 |
| 连接事件/组/日志长期无变化 | 事件流保持合法静默，不使用周期帧 watchdog，不产生重连 |
| status 正常但 trafficAvailable=false | status 为 Fresh，统计能力明确不可用，不显示假零值、不反复恢复 |
| 主动暂停、息屏、休眠或消费者解绑 | Paused 与异常断开分离，不累计失败；恢复启用后重新等待首样本 |
| 握手成功但 status 订阅立即失败 | 不能短暂误报 Fresh；多流重复断开回调只安排一次有界恢复 |
| 401/403 或明确不支持的订阅 | 进入终止性错误，预算冷却后也不循环重试；凭据/能力变化后可恢复 |
| 控制 API 超时、401、服务 IPC 暂不可达 | 分别归因；无退出证据不报内核已退出；停止操作仍可执行 |
| 内核真实退出或后台服务死亡 | 发布确认退出/未知的正确状态；不保持过期 Running 假象 |
| 统计回调丢弃、异常、页面卸载重挂载 | 后台序号与 UI 消费序号可追踪；后续有效通知能继续处理 |
| 重连期间停止/重启/切源、同值 ABA | 老 epoch/操作/请求结果全部拒绝；停止后无新连接复活 |
| 睡眠唤醒、时钟调整、网络切换 | 使用单调年龄和宽限；不会集中触发历史超时或重复修复 |
| Android Binder 正常但 status 冻结 | 能识别后台停更；UI 读取缓存不刷新 status 年龄 |
| Android client 断开、晚到回调、Activity 回收 | 后台单飞恢复订阅；旧回调不污染新实例；TUN 与数据库边界不变 |
| 网络正常但统计故障 | 独立修复统计，数据面持续转发；累计值不清零、不重复计算 |
| 统计正常但指定业务请求失败 | 业务维度给出有限结论；不把统计正常当成网络健康 |
| 用户主动检查、取消或连续点击 | 单飞、预算和取消生效；无配置修改，无日志凭据泄漏 |

测试采用可控时钟、独占假控制器/推送端点和隔离后台适配器，覆盖生产调度及通道生命周期，
不只测判定器枚举。通过独立本地目标的真实转发验证“恢复统计时业务不断”，不能用进程存活替代。
平台验收覆盖 Linux/Windows/macOS、Android Wi-Fi/移动网络/休眠恢复；未跑的平台单列未验证。

实施时每批执行主目标构建与 version、相关回归、`git diff --check`。
最终桌面门禁使用 `-DCLASHFLUX_REQUIRE_PROJECT_TESTS=ON` 和
`ctest --test-dir build --output-on-failure --no-tests=error`，现有 17 项不能减少或禁用。
如新增独立必跑测试，同步 `ProjectTestGate.cmake`、测试注册及文档中的数量；
Android 兼容源码图、AIDL/JNI 和打包也要验证，桌面通过不代表 Android 实机通过。

## 建议执行批次

1. **第一批：M1 + M2。** 先完成桌面证据采集、独立统计通道恢复和故障注入回归。
2. **第二批：M3。** 复用同一健康语义，补 Android 后台状态流和 Binder 新鲜度及真机恢复验证。
3. **第三批：M4。** 接入已验证的诊断结果及按需测试，完善用户反馈与跨平台长时间验收。

本计划的完成标准是：能区分统计故障与业务失败，观测通道故障可有界自愈，
状态不因超时或过期缓存误报，并能导出下一步定位所需证据。
它不等同于已经解决所有节点、DNS、路由或外部网络故障。
