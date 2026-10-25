---
name: clashflux-runtime
description: 修改 Clash-Flux 内核控制、托盘与无窗启动、首帧快照、PPTP 交接、OpenVPN 导入或订阅唤醒链接时使用。
---

# 启动、运行控制与原生连接

涉及 UI 控制先读 [UI 生命周期](../clashflux-ui/references/lifecycle.md)，服务/系统网络同时按 [平台技能](../clashflux-platform/SKILL.md) 选择当前平台；阻塞内核、网络、路由、系统设置工作进任务线程。

## 首帧与托盘

先读 [启动设置快照](../../docs/ui-development.md#启动设置快照) 与 Windows 后台/托盘章节。

- 桌面 AppOptions::create_initial_window=false；application hook 在 Runtime Running 后才建窗。启动阶段唯一同步持久化读取是 readStartupSettings，桌面在托盘/首窗前读一次，AppRoot 复用；Android 等平台先建窗入口由 AppRoot 读。
- 快照只含首帧主题模式/颜色/语言与 tray.enabled/start_minimized；其它设置等待 SettingsModel ready，慢启动不扩大同步读取。临时只读连接不建文件、不迁移、不改 journal、不 hydrate/改订阅缓存；失败明确终止、保留原库。
- tray.enabled 与 start_minimized 都真时，先无窗启动持久化、数据泵、服务版本同步和内核自启，等待异步托盘可用性。图标发布成功保持无窗，激活用 OpenWindow；超时/图标失败建窗，后续刷新失败不退出或隐藏唯一窗口。
- 托盘关闭时快照/SettingsModel 将 startMinimized 视为 false，但不改历史值；设置仅在托盘启用时显示该选项，重新启用恢复历史值。伪 CLI 隐藏窗口执行运行时命令；外部订阅链接打开窗口只预填表单。
- 关闭按钮仅由 tray.enabled 控制：启用且可用则隐藏，否则确认退出；不读 tray.close_behavior，确认框不提供托盘动作。最小化使用系统原生行为，Windows 保留任务栏入口，不因托盘/轻量模式隐藏窗口。
- Linux 无窗简版菜单切完整菜单时先隐藏，下一轮托盘轮询才发布，不同结构不在同轮替换，避免 Plasma 旧动作；共享退出收尾与实际状态映射。
- 启动后异步检查已安装桌面服务，在内核自启前完成版本同步；同版本跳过，未安装不主动装，失败一次反馈、不自动请求 UAC。Windows 升级信任边界见平台技能；macOS 无独立服务，手机服务随应用更新。

## PPTP 与 OpenVPN

先读 [PPTP 交接](../../docs/native-pptp-handoff.md) 或 [OpenVPN 导入](../../docs/openvpn-import.md)，按实际任务读取。

- VpnStore 的任务线程持续刷新服务能力并发布 VpnSnapshot，用 setEngineAvailable，不用会断连接的 setAdapters；卡片只读应用 VpnModel，渲染时不探测服务。显式连接先查服务/能力，具体失败原因优先。
- Windows 活动 TUN 要求 PPTP_LIVE_DIAL，主 TUN 保留到候选准备后交接；旧服务拒绝并提示更新。PptpSingleSession 平台仅一条 PPTP，存储在扰动主内核前拒绝第二条并指明占用，UI 同样反馈；不静默断旧连接或假装并发。
- Windows/Linux 释放接口前必须撤出仍引用它的运行计划，或确认主内核已停；候选失败且旧计划未引用则保留主内核。PPTP_STATUS 三态，UNKNOWN 不清会话/伪造失败。Windows 拨号 TUN 补偿租约随会话，PPP 路由成功才发布，失败留旧租约，确认断开才释放临时租约。
- .ovpn 按内容识别走普通远程/本地来源，输出同一 sing-box endpoint。主来源可默认出口，桌面次来源仅启用规则引用才载入，手机单活动普通来源；原生独立拨号只保留 PPTP。
- 不恢复 type=openvpn 独立会话、专用编辑器、批量开关或文本粘贴入口；粘贴先存文件走本地导入，扫码文本仍按内容识别。用户确认无需迁移旧独立记录，不加启动迁移/清库；外部文件、脚本及未转换路由/DNS 指令拒绝并记保真度。

## 订阅唤醒链接

先读 [订阅链接](../../docs/profile-links.md)。统一 src/profile_link.* 解析 sing-box import-remote-profile 与 Clash/FlClash install-config，仅解码一次、只接受 HTTP(S)。
桌面单实例 IPC，Android/macOS 用 application activation；粘贴/扫码/网页同一解析，收到链接只预填独立待确认表单，不直接切换活动订阅，不绕过平台能力或默认证书检查。

按 [测试技能](../clashflux-test/SKILL.md) 验证；首帧外观、托盘失败回退、权限、VPN 生命周期等需要对应 GUI/平台证据，不能用无窗口测试替代。
