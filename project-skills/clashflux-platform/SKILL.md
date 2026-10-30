---
name: clashflux-platform
description: 修改 Clash-Flux Android、Windows、Linux 或 iOS 平台服务、后台生命周期、权限、数据面和原生构建路径时使用。
---

# 平台边界

只读取当前任务涉及的平台条目及其链接；跨平台改动逐个平台核对，不因视口或编译宏的相似性扩大能力。
发版用 [发布技能](../clashflux-release/SKILL.md)，依赖升级用 [依赖技能](../clashflux-dependencies/SKILL.md)，运行控制/PPTP 用 [运行技能](../clashflux-runtime/SKILL.md)。

## Android

先读 [Android 生命周期](../../docs/android-background-lifecycle.md)；构建/Gradle/libbox 改动另读 [Android 构建](../../docs/android-build.md)。

- sing-box 固定归 :background：ClashVpnService 持 libbox/VpnService，RuntimeControlService 通过 AIDL/Binder 提供控制/快照，JNI 接回 UI；不能依赖跨进程静态字段。
- SQLite/CoreStore 只由默认 UI 进程打开/写，后台只消费原子提交 core/config.json；大快照用原子文件，不通过 SharedPreferences 同步运行态，也不在后台调用会写库的 CoreStore。
- onCreate 先同步 startForeground，JNI/libbox/数据面放服务线程；不等 POST_NOTIFICATIONS 才启动前台服务或继续 VPN 授权。START_STICKY 空 Intent 从持久化恢复模式，不能猜 TUN。
- 打开应用只 startCore(startVpn=false)，不自动启动代理；配置仍按持久化 TUN 意图编译以保住恢复 inbound，core-only 标记 tun_preference_reset_pending 供 UI 回读关闭。TUN 只由显式按钮/磁贴或服务按持久模式回收恢复启动。
- 网络变化、亮屏/退出 Doze 先更新 sing-box 物理接口再关闭旧连接；VPN 时长使用后台 VpnUptime，不能把 core-only/UI 存活当会话。
- Gradle Java HuxerUI 必须与 CMake native 同源：有源码用该源码的 platform/android/huxerui，只有 SDK 回落才用 SDK AAR。国内 GEOIP/GEOSITE 固定 revision/SHA256 构建打包，不跳 stageBundledRuleSets 或改运行时下载。
- libbox 与桌面固定同版本/revision；AAR 的 version/revision/ABI/SHA256 元数据缺失/不符须重建，不能只看文件存在。
- APK 更新读 [应用升级](../../docs/app-updates.md)：HUI 下载与应用作用域平台模块、工作线程校验正式签名/版本，保存配置并停核后由系统确认安装；私有只读 Provider 不暴露订阅或初始化 Runtime，拒绝/取消可重试，不卸载清数据绕过签名。

## Windows

先读 [Windows 服务](../../docs/windows-service.md)，修改交接/路由另读 [PPTP 交接](../../docs/native-pptp-handoff.md)。

- 数据面归 SCM ClashFluxService（LocalSystem），GUI/CLI 不直接 spawn 内核或提权 GUI；服务持 RAS/PPTP 和 TUN 补偿，GUI 持用户数据库/订阅/系统代理。安装/更新/卸载只提权服务子命令，UAC 前读取原 SID，卸载不触碰订阅。
- 管道仅安装用户/SYSTEM/管理员，校验双方身份。载荷为受保护 Program Files 独立目录；升级只接受真实管道客户端的受保护载荷，逐级核对 owner/DACL，拒绝可写文件与 reparse point。
- 自更新先暂存，由受保护旧服务的 LocalSystem 助手替换、重启 SCM，保留安装 SID；版本不符优先自更新，失败拒运行并保留手动 UAC 入口。旧版不支持 UPGRADE 或可写便携目录不能为免 UAC 放宽 SYSTEM 代码替换权限。
- 后台优先 Win32 API；子进程 CreateProcessW + CREATE_NO_WINDOW 并重定向标准句柄，不用 system/_popen/闪窗 shell，启动/轮询/清理同样遵守；只有用户交互授权流程可显示系统提示。
- 本进程 HANDLE/HKEY 用 win32_raii.h 的 UniqueHandle/UniqueHkey；服务专属回归与 WFP 需管理员测试环境。

## Linux

- 与 Windows 共用应用层生命周期和服务状态判定；sing-box/TUN/PPTP 归 systemd root 服务，普通启停不回落 GUI/CLI spawn，root GUI 也不例外。首次显式启动可沿已有安装流程授权，服务不可达/版本不符不能切换成无服务运行。
- 停核不能仅按 pref 9000 删除策略规则，它也可能属于其它 sing-box。只清理 52000–52999 表内协议 186、metric 4278190000 的完整保留哨兵；普通用户无权限不尝试修改，由 root 服务 STOP 清理已确认归属的规则/路由。
- pppd/接口释放遵守运行计划撤出边界，UNKNOWN 不能当断开；服务 IPC fd 用 service.cppm 的 UniqueFd。无窗托盘与单实例读运行技能/数据技能。

## iOS（暂缓）

只有任务实际涉及 iOS 才读 [iOS 构建](../../docs/ios-build.md)。iOS 不承诺支持/发布，不加入 Release 门禁：固定框架 revision + Simulator SDK 编译 app/Packet Tunnel，Libbox device/Simulator xcframework 与 Android 同 sing-box revision，仅未签名诊断，不产 IPA。
订阅/规则集用 ClashFluxBridge.mm 的 NSURLSession（iOS curl 无 TLS），默认系统证书校验；仅显式 allowInvalidCert 的那次请求 delegate 允许无效证书，不改公共 HTTP API 或全局信任。
Libbox 必须由 Packet Tunnel extension 托管，不 posix_spawn/fork/CLI；App Group 原子文件共享配置，extension 不读写应用数据库。
设备签名必须 Apple 开发团队身份及匹配 profile，Packet Tunnel 授权 packet-tunnel-provider entitlement；不造随机/自签身份，私钥/profile 不入库。签名设备包与真机 VPN 仍需独立验证。
