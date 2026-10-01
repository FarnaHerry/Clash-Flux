# iOS 构建与签名

## 支持状态：TODO（暂缓，不承诺支持）

iOS 当前不是 Clash-Flux 的受支持或发布平台。仓库中的 App、Packet Tunnel 和 CI 工作仅为实验性集成与 Simulator 编译检查，不能据此宣称 iOS VPN 已可用；不生成或发布 IPA，也不纳入正式 Release。仓库目前没有 Apple 开发团队签名配置。是否恢复 iOS 支持，待项目维护者未来重新评估；在此之前按 TODO 处理，不以 TestFlight 或 App Store 发布为当前目标。

## 当前构建范围

`.github/workflows/build.yml` 中的 iOS 诊断 job 会：

- 以 iOS Simulator SDK 编译 Clash-Flux app 与 Packet Tunnel extension；
- 运行 `platform/ios/build-singbox-libbox.sh`，从与 Android 相同的固定 sing-box revision
  构建 iOS device 和 Simulator 的 `Libbox.xcframework`。

该 job 生成未签名的 Simulator `.app` 和 `.appex`，但不生成 IPA、不上传 Release 资产，也不属于
Release 门禁；签名设备包和真机 VPN 生命周期仍需验证。Clash-Flux iOS `CoreProcess` 显式拒绝
桌面 sing-box 子进程启动；Packet Tunnel extension 持有 Libbox，并通过 App Group 原子文件读取
编译配置，extension 不得打开应用数据库。

iOS 订阅和规则集下载由 `src/api.cpp` 转到 `platform/ios/App/ClashFluxBridge.mm` 的
`NSURLSession`，默认使用 Apple 系统证书校验。订阅的“允许无效证书（危险）”选项只影响这一个
下载请求的 server-trust challenge；未勾选时 delegate 执行系统默认校验。不要通过修改全局
URLSession 信任设置或 HuxerUI 通用 API 实现证书跳过。

在 macOS + Xcode 环境中可单独生成 Libbox framework：

```bash
platform/ios/build-singbox-libbox.sh /tmp/Libbox.xcframework
```

脚本会检出 `SINGBOX_COMMIT` 指定的上游 revision；默认值与项目当前 sing-box pin 一致。
构建需要 Go、Make 和可用的 iPhoneOS 与 iOS Simulator SDK。

## Apple 签名

不要自行填入随机字符串或使用自签名证书作为 iPhone 发布签名。iOS 安装需要有效的代码
签名和匹配的 provisioning profile；iOS 会拒绝无效签名的应用。[Apple 代码签名说明](https://developer.apple.com/documentation/xcode/using-the-latest-code-signature-format)

Packet Tunnel 使用 `NEPacketTunnelProvider`，必须由 provisioning profile 授权
`com.apple.developer.networking.networkextension` 中的 `packet-tunnel-provider` 能力。
App 与 Packet Tunnel extension 各自需要正确的 App ID、entitlements、签名身份和 profile。
[Apple Network Extension entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.networking.networkextension)
和 [Packet Tunnel Provider 文档](https://developer.apple.com/documentation/networkextension/nepackettunnelprovider)
说明了这项要求。Apple DTS 说明，付费 Apple Developer 团队可为 App ID 启用 Network Extensions 能力，
并生成授权该能力的 provisioning profile。[Apple Developer Forums](https://developer.apple.com/forums/thread/814047)

私钥、`.p12`、profile 和 App Store Connect API 私钥只应作为受限 CI secret 或本机 Keychain
材料管理，不写进仓库。没有真实 Apple Developer 团队及 Packet Tunnel entitlement 时，CI
只能验证未签名构建；不能发布可安装 IPA。

## 纳入 Release 前的门槛

Release job 只有在以下项目完成后才纳入 iOS：

- 可启动的 Clash-Flux iOS app 与 Packet Tunnel extension；
- extension 通过 App Group 原子文件读取 app 编译后的配置，并由固定 sing-box Libbox 生命周期管理隧道；
- 在 Simulator 或真机上核对 iOS URLSession 的默认证书校验、显式无效证书选项和 HTTPS 订阅下载；
- Xcode device archive 与签名导出通过 CI，应用和 extension 的签名、entitlements 与 provisioning
  profiles 均通过核验；
- 真机启动、VPN 授权、连接、停止和网络切换完成验证。

当前的未签名 Simulator app、Packet Tunnel extension 和 Libbox framework 只通过编译检查，仍不满足这些发布门槛。

## 内核版本对齐

构建脚本默认使用 sing-box 1.14.2 revision
`af6e64c3b69e6132ebaee0e1a3d24e93903f6709`，与 Android/libbox 和桌面一致。
本次本地验证只覆盖 Linux；未构建新的 iOS xcframework 或验证真机生命周期。
iOS 仍按 TODO 暂缓，不属于 Release 目标。升级记录见
[稳定内核升级](singbox-stable-upgrade.md)。
