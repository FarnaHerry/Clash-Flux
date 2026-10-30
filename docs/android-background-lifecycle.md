# Android 后台服务生命周期

订阅切换使用应用级单任务队列：重复点击同一待切卡片不再提交准备，改选只保留最后一个目标，
取消旧候选并拒绝其回写。取消在编译条目/阶段边界生效，不强行中断正在执行的单次解析、序列化、
下载或已开始的内核交接。候选校验、启动、代理组预览与成功提示复用同一份准备产物和解析结果；
未改变 selector 默认值时复用原 JSON 字节，不重新序列化。真实设备耗时仍需单独测量。
后台到期更新不进入切换队列，也不接收切换取消令牌；仍逐个抓取订阅并调用
`completeRemote` 检查、原子提交和必要的重启。切换与更新在运行锁下串行提交，
可能相互等待，但改选不能取消其它订阅的后台更新。

2026-10-25 订阅准备优化：在规则集发现/首次编译前统一 Android 运行参数，TUN 继续
使用 `strict_route=false`，保留 IPv6、端口及用户来源语义。Android 打包 GEO 预取不改输入，
检查回调与启动计划复用已有编译产物；HTTP/ASN 新快照仍重新编译并完成候选检查后提交，
桌面 GEO 预取改变输入或规则集目录变化时仍重编译。后台 `Libbox.checkConfig` 保留。
日志 `profile-providers` 记录依赖发现编译次数、微秒与准备总耗时；`profile-prepare`
区分网络检查、规则集/编译、GEO 复用/重编译、代理组/JSON、检查及暂存写入，
`runtime-prepare` 区分 libbox 初始化、配置等待/读取、校验、CommandServer 和数据面启动。
仅输出阶段、耗时、次数和字节数，不输出订阅地址、内容或凭据；Android 的原生检查
标记为延后到 libbox，不能将计时标签当作桌面 CLI 校验通过。尚无手机耗时对比。

2026-10-25：首页/快捷磁贴在请求 VPN 授权前、授权返回后，以及后台 `openTun`
前检查同一用户可见的其他活动 VPN。使用系统 owner UID 或当前系统 VPN 授权区分自身，
不依赖静态字段或持久化标记猜测所有权；证据未知时保留原授权流程。
发现冲突只反馈并停止本次启动，不主动撤销其他 VPN；core-only 自启不触发 VPN 接管。
范围与验证限制见[跨平台冲突检查](windows-tun-coexistence-plan.md)。

## 参考取舍

- 官方 [sing-box Android 功能说明](https://sing-box.sagernet.org/clients/android/features/)
  将无 TUN 运行放在普通前台服务中，需要 TUN 时使用 `VpnService`。Clash-Flux 复用同一套
  libbox 数据面和配置编译器，当前由一个 `ClashVpnService` 承担两种模式，避免两个服务争夺
  libbox 生命周期。
- Android 官方要求前台服务尽快提升前台优先级；[服务启动说明](https://developer.android.com/develop/background-work/services/fgs/launch)
  中的启动期限不能被耗时初始化占用。ClashVpnService 在 `onCreate()` 立即调用
  `startForeground()`，JNI 初始化、libbox setup、配置校验和数据面启动都在服务工作线程执行。
- [NekoBox](https://github.com/MatsuriDayo/NekoBoxForAndroid) 把代理服务、VPN 服务、启动广播
  和磁贴放在后台进程中。其网络设置也提供“网络变化后重置连接”和“设备唤醒后重置连接”的思路；
  Clash-Flux 在检测到物理网络、接口或 DNS 服务器变化、退出 Doze 或亮屏后，先更新 libbox
  默认接口，再延迟关闭旧连接，让 sing-box 通过新网络重新拨出。
- 不照搬任何客户端的单个版本的启动顺序。官方 sing-box Android 在 1.14.0 的
  [Android 16 前台启动问题](https://github.com/SagerNet/sing-box/issues/4494)记录了磁贴/Always-on
  启动时异步启动服务、未及时调用 `startForeground()` 的失败路径；Clash-Flux 保持先提升前台、后做异步初始化。

## 进程和数据所有权

| 所有者 | 负责内容 | 进程边界 |
|---|---|---|
| `MainActivity` + C++ `CoreStore` | HuxerUI、SQLite、订阅和配置编辑、生成运行配置 | 默认 UI 进程 |
| `ClashVpnService` | libbox、TUN fd、通知、网络/唤醒恢复、代理命令通道 | `:background` |
| `RuntimeControlService` | UI 到后台运行时的 AIDL/Binder 命令和状态读取 | `:background` |
| `BootReceiver` / 快捷设置磁贴 | 启动恢复、切换 VPN | 默认进程，只提交 Android 服务命令 |

配置数据库只由 UI 进程的 CoreStore 持有。UI 编译完整 sing-box JSON 到应用私有目录，后台服务
读取已提交的 `clash-flux/core/config.json`；后台进程不打开或写入 SQLite。小型状态通过 Binder
每秒同步一次；出站组和连接 JSON 通过同一私有目录中的临时文件加原子改名共享，避免 Binder
事务大小限制和 SharedPreferences 多进程缓存问题。后台日志通过有界 Binder 日志缓冲转交给 UI，
数据面不依赖 UI 是否已绑定。

## 启停和恢复

- 应用启动先由 CoreStore 读取已保存的 TUN 设置，再编译对应配置；core-only 请求不改写
  已提交配置中的 TUN 恢复意图，运行时另在内存中移除 TUN inbound。
- `ClashVpnService` 在 Android `:background` 进程中执行。它在 `onCreate()` 第一时间调用
  `startForeground()`，随后由工作线程初始化 JNI/libbox 和运行配置。
- 服务以 `START_STICKY` 返回。系统传入空 Intent 时，从服务专属持久状态精确恢复 `core_only`
  或 `vpn`；状态不明确时停止空服务，绝不猜测为 TUN。
- 打开 UI 的 core-only 请求只保留后台当前确实在运行或启动、且未取消的 VPN；旧恢复标记
  不能把该请求升级为 VPN。无 TUN 模式在内存中移除 `tun` inbound，保留其它 inbound 和
  已提交配置文件供真正的 sticky VPN 恢复；`openTun` 在创建接口前再次检查本次请求权限。
- 用户关闭、VPN 授权撤回、启动失败会清除恢复模式。普通进程回收不依赖 `onDestroy()` 清理恢复状态。
- UI 的显式停止在任务线程通过 Binder 等待 libbox 关闭结果；启动中的停止先取消启动并等待工作线程收尾，关闭失败不得回报“已停止”。Activity 销毁本身仍不停止常驻内核。
- 从最近任务划走 Activity 继续保留后台 VPN，重新打开只重新绑定；这与显式停止或撤销 VPN
  授权不同。后两者及 libbox 的停止回调在后台工作线程收尾，即使 libbox 关闭失败仍尝试
  关闭 TUN；失败保留未关闭句柄和错误通知供重试，确认释放后才移除前台通知并停止服务。
  服务对象缺失的快照仍保留实际错误状态，不把缺省 Bundle 值解释为已停止。
- `BootReceiver` 仅在先前 TUN 状态仍启用时启动 VpnService；它不加载 HuxerUI，也不初始化 UI 数据库。
- 旧的同进程 `CoreService` 常驻通知已移除；无 TUN 内核与 VPN 共用 libbox/VpnService 生命周期，
  避免两个常驻前台服务和重复通知。

## 网络切换和唤醒

服务监听带 Internet 能力的网络与默认物理接口。物理网络、接口或当前物理网络的 DNS
服务器列表变化时更新 libbox 的 `InterfaceUpdateListener`；DNS 变化使用回调携带的
`LinkProperties`，保证 libbox 刷新接口时读到新列表，然后稍作合并关闭现有出站连接。
屏幕点亮或设备退出 Doze 时执行同一连接重置，恢复休眠前卡住的上游连接。若服务未在运行，
不创建额外内核，也不持有额外 wake lock。

## 限制与诊断

独立进程可让 HuxerUI Activity/native view 崩溃时 libbox 数据面继续运行；它不能阻止用户强行停止、
Android Task Manager 停止或厂商禁用后台/自启动。`START_STICKY` 是系统回收后的重建请求，不是
“永不被杀”。特定设备仍需允许后台运行或忽略电池优化。服务内的运行日志会在 UI 连接期间经 Binder
缓冲转交；进程崩溃前尚未同步的内存日志仍应结合 Logcat 与 libbox 崩溃报告排查。

## 关键实现文件

- `platform/android/app/src/main/AndroidManifest.xml`：后台进程和服务声明。
- `platform/android/app/src/main/java/dev/farna/clashflux/ClashVpnService.java`：唯一 libbox/VPN
  所有者、sticky 恢复、网络及唤醒恢复。
- `platform/android/app/src/main/java/dev/farna/clashflux/RuntimeControlService.java` 和
  `platform/android/app/src/main/aidl/dev/farna/clashflux/IClashRuntime.aidl`：跨进程控制面。
- `platform/android/app/src/main/java/dev/farna/clashflux/RuntimeSnapshotStore.java`：共享原子快照。
- `platform/android/android_bridge.cpp`：JNI 运行状态写回和 UI 进程快照接收。

## 首页 VPN 运行时长

`VpnUptime` 由 `:background` 的 `ClashVpnService` 持有，只有确认 VPN 已连接
（state=2）才用 `SystemClock.elapsedRealtime()` 开始本次计时；core-only、授权等待、
启动失败和取消不计时。重复发布 connected 保留起点，断开后再连接建立新会话。
状态与 elapsedMs 在同一同步快照读取，经现有 Binder Bundle → MainActivity → JNI
成对发布到 UI 进程，`ReadCoreView` 再发布到应用模型；界面不自行推测起点。
因此锁屏休眠、UI 进程重建和观察者重新绑定都延续后台会话，系统墙钟调整不影响计时。
Binder 暂时丢失只保留最后观测，不能清零会话或伪造断开；后台进程真正重启后重新连接
自然建立新会话。首份 Binder 快照有独立就绪标志，不能把 JNI 默认值当作已确认状态。

无 Android 设备也可从 `platform/android` 运行宿主 JVM 回归；两项均接入 `preBuild`：

```powershell
./gradlew.bat :app:testRuntimeStartPolicy :app:testVpnUptime
```

`RuntimeStartPolicyTest` 覆盖重新绑定保留活动 VPN、停止/取消拒绝旧启动、无 TUN 请求权限
与 inbound 过滤策略；`VpnUptimeTest` 覆盖后台会话时钟。它们不代替 Java/NDK/APK 构建，
也不代替真机的划走后重开、显式停止、授权撤回、通知/系统 VPN 图标与 sticky 恢复验收。
