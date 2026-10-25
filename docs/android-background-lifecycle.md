# Android 后台服务生命周期

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

- 应用启动先由 CoreStore 读取已保存的 TUN 设置，再编译对应配置；此前台服务启动请求不再把
  已启用的 TUN 配置改写成无 TUN 配置。
- `ClashVpnService` 在 Android `:background` 进程中执行。它在 `onCreate()` 第一时间调用
  `startForeground()`，随后由工作线程初始化 JNI/libbox 和运行配置。
- 服务以 `START_STICKY` 返回。系统传入空 Intent 时，从服务专属持久状态精确恢复 `core_only`
  或 `vpn`；状态不明确时停止空服务，绝不猜测为 TUN。
- 用户关闭、VPN 授权撤回、启动失败会清除恢复模式。普通进程回收不依赖 `onDestroy()` 清理恢复状态。
- UI 的显式停止在任务线程通过 Binder 等待 libbox 关闭结果；启动中的停止先取消启动并等待工作线程收尾，关闭失败不得回报“已停止”。Activity 销毁本身仍不停止常驻内核。
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
