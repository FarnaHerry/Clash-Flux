# Android 后台服务生命周期

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
  Clash-Flux 在检测到物理接口变化、退出 Doze 或亮屏后，先更新 libbox 默认接口，再延迟关闭旧连接，
  让 sing-box 通过新网络重新拨出。
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
- `BootReceiver` 仅在先前 TUN 状态仍启用时启动 VpnService；它不加载 HuxerUI，也不初始化 UI 数据库。
- 旧的同进程 `CoreService` 常驻通知已移除；无 TUN 内核与 VPN 共用 libbox/VpnService 生命周期，
  避免两个常驻前台服务和重复通知。

## 网络切换和唤醒

服务监听带 Internet 能力的网络与默认物理接口。物理网络或接口变化时更新 libbox 的
`InterfaceUpdateListener`，稍作合并后关闭现有出站连接。屏幕点亮或设备退出 Doze 时执行同一连接
重置，恢复休眠前卡住的上游连接。若服务未在运行，不创建额外内核，也不持有额外 wake lock。

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
