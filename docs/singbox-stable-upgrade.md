# sing-box 稳定内核升级记录

日期：2026-09-30。由 1.14.0 升级到官方最新稳定版 **1.14.2**，发布于
2026-09-24；源码 revision：`af6e64c3b69e6132ebaee0e1a3d24e93903f6709`。
不跟随 1.15 alpha。依据为[官方 Release](https://github.com/SagerNet/sing-box/releases/tag/v1.14.2)
和[发布资产 API](https://api.github.com/repos/SagerNet/sing-box/releases/tags/v1.14.2)。

## L1：版本、资产与缓存

桌面 CMake、CI `SINGBOX_COMMIT`、Android Gradle / libbox 脚本与 iOS libbox 脚本
统一版本和源码 revision。各平台产物独立编译，其二进制 SHA256 不相同。
桌面官方归档摘要固定在 `cmake/singbox_bundle.cmake`：

| 1.14.2 资产后缀 | 官方 SHA256 |
|---|---|
| `linux-amd64.tar.gz` | `a684484d7477d1437282ee411f4d131d0340aaad60a7868841ebd5d87dd8a0c6` |
| `linux-arm64.tar.gz` | `b43a1fb1bda131c6653576741ce527eb2bdeab7c9308ca90ee8b972abb7e4a7f` |
| `windows-amd64.zip` | `c2d8bfff918755808781dfdeeb8581b6c91eb3a243d9a7b55483cfc0c0684d32` |
| `windows-arm64.zip` | `2bb467039310452380958b821983d5bb4f78fb70a2583914e4bea8b011582b64` |
| `darwin-amd64.tar.gz` | `b0bfb0dc70a5fc708710b9f5ea98b9ee76d40fa4169928d25d73edc4331df2fe` |
| `darwin-arm64.tar.gz` | `925c5382eca8492b0150f868a6db20b18290a38700e621724b3703fd453e032d` |

桌面缓存为 `build/vendor/singbox/<version>/<asset-sha256>/`，不覆盖旧版本缓存。
复用解包二进制前比对其 `.sha256`；重新解包时也校验已下载归档的官方摘要。
这样升级 pin 后不会因旧的规范文件名 `sing-box` 已存在而继续打包旧内核。
`CLASHFLUX_BUNDLE_SINGBOX=OFF` 仍允许使用自行管理的内核，须自行确认版本。

Android 构建脚本生成 AAR 及同名 `.metadata.json`，记录实际 checkout revision、
ABI 和 AAR SHA256。`stageSingboxAar` 将三者与固定版本核对，拒绝无来源记录、
旧 revision 或损坏产物。`CLASHFLUX_SINGBOX_AAR` 的源文件和 sidecar 须一起提供；
本地已暂存产物也受同一校验约束，不能为旧 AAR 手写新 revision。
详见[Android 构建](android-build.md)。iOS 只对齐默认源码 pin，仍按 TODO 暂缓。

## 上游兼容性审查

对比 v1.14.0 与 v1.14.2 的相关官方源文件：

- `option/group.go`、`outbound.go`、`rule.go`、`rule_dns.go`、`dns.go` 未变。
- `experimental/clashapi/proxies.go` 未变，selector/urltest 控制语义保持。
- `option/experimental.go` 删除内部 `ModeList` 成员（原为 `json:"-"`），不新增
  可写入配置的 `mode_list`。应用仍使用内核推导的模式列表。
- `experimental/libbox/command_client.go` 调整连接上下文、取消与锁管理；其导出的
  函数签名未变。接口审查不能替代新 Android AAR 构建和后台运行验证。

固定版源码：[组配置](https://github.com/SagerNet/sing-box/blob/v1.14.2/option/group.go)、
[拨号字段](https://github.com/SagerNet/sing-box/blob/v1.14.2/docs/configuration/shared/dial.md)、
[Clash API](https://github.com/SagerNet/sing-box/blob/v1.14.2/experimental/clashapi/proxies.go)、
[CommandClient](https://github.com/SagerNet/sing-box/blob/v1.14.2/experimental/libbox/command_client.go)。

## L2：本次继续接入

| 输入或问题 | 当前处理 |
|---|---|
| `dialer-proxy` | 映射原生 detour；支持当前配置内节点/组及向后引用 |
| 缺失代理链目标、编译后重复 tag、循环 | 完成全部出站后检查依赖图；组的全部候选成员参与；报编译错误，不改成直连 |
| `select.default-selected` | 有效成员映射 selector.default；不强制覆盖已缓存的选择 |
| 未映射组字段 | 每个字段记 Group/Approx，不臆造 lazy/timeout 等原生设置 |
| `use` 数组、include-all 系列 | 不展开 provider，记 Group/Unsupported 并保留显式成员 |
| 根级 proxy-providers / rule-providers | 不展开，记 Field/Unsupported；仍待开发 |
| `no-resolve` | 保留匹配条件并记 Rule/Approx，明确托管前置 resolve 仍可能解析域名 |
| 未知规则修饰符或缺少必需字段 | 整条跳过并记 Rule/Unsupported，不截断后静默接受 |

`dialer-proxy` 依据 [Mihomo 代理链文档](https://wiki.metacubex.one/config/proxies/dialer-proxy/)
与固定版 sing-box detour；保真度基线见[分层契约](singbox-layers-and-fidelity.md)。
示例见 [kernel-capabilities.yaml](examples/kernel-capabilities.yaml)，服务端均为占位值。
本次不改变手机单活动普通订阅，也未实现桌面跨订阅合并；后者仍是
[最终编排设计](desktop-subscription-orchestration.md)。

## 本地验证范围

实际执行并通过：

- `cmake --build build --target clash-flux`：重新编译 `src/singbox.cpp` 并链接，
  POST_BUILD 打包 sing-box 1.14.2。
- `./run.sh --version`：退出码 0，Clash-Flux v0.3.10。
- `build/engines/sing-box version`：1.14.2，完整 revision 与固定源码一致，
  官方 Linux amd64 包使用 Go 1.26.8、CGO disabled。
- 官方 Linux amd64 归档完整 SHA256 与上表一致，随后由 CMake 再次校验后解包。
- Android `./gradlew help --offline --no-daemon`：在设置 HUXERUI_HOME、JAVA_HOME、
  ANDROID_HOME 后成功；这是 Gradle 配置加载，不执行 AAR 暂存或 APK 构建。
- `bash -n platform/android/build-singbox-libbox.sh platform/ios/build-singbox-libbox.sh`
  与 `git diff --check`：通过。

没有新增或运行测试套件。Android AAR/APK 后续重新构建结果见下节；
iOS xcframework 未在本机重新构建。源接口对比和版本对齐不能视为平台运行验证。
服务器握手、真实网络代理链、手机后台和 GUI 操作仍需实际环境验证。

## 后续升级流程

1. 查询官方 latest stable，核对非 prerelease、release tag 对应源码 revision 和六个
   桌面资产摘要；同时更新 CMake、CI、Android Gradle / 脚本及 iOS 脚本默认 pin。
2. 对比配置结构、Clash API 与 libbox 导出接口，必要时更新调用方，禁止写入未知字段。
3. 由固定源码重新生成 Android AAR 和 metadata，不能沿用旧二进制补新身份。
4. 重新构建桌面，核对实际 `engines/sing-box version`；执行仓库要求的应用版本
   命令和 diff 检查，并同步能力基线与实际平台验证范围。

Android libbox 构建只浅取正式版本 tag，核对 tag 对应的 HEAD 与固定 revision；
保留真实 tag，避免上游 `ReadTag()` 在浅取裸 commit 后把版本写成 unknown。
AAR 元数据包含 version，Gradle 同时核验版本、revision、ABI 与内容摘要。
自定义 pin 时必须同时提供 `SINGBOX_VERSION` 和 `SINGBOX_COMMIT`，不伪造版本标签。

## 2026-10-01 后续本地构建

Linux 主目标增量编译、`./run.sh --version`、`git diff --check` 通过。
Glaze 与本轮拨号/DNS/逻辑规则/inline provider/平台规则已编入生产主目标。
Android arm64 C++ 主目标也已通过，包括 UI codegen、Legacy 领域代码、codec 与 JNI。
`./gradlew help --offline --no-daemon` 配置检查通过；完整 native Gradle 任务曾在
stageSingboxAar 因缺少新身份元数据失败，未绕过缓存校验。

临时 Go 1.26.8 归档校验 SHA256：
`d0f743b33e8d8945e6b1f432edd15785c70507121d6e2a723b21285eddf8b57b`。
Temurin OpenJDK 17.0.20.1+1 归档校验 SHA256：
`3808d1d15e3ec6bd5b84057fb5d84c33d8a1536a258146bcea2e603fc726e08e`。
工具仅用于本地构建，不入库、不改变项目内核 pin。Go 依赖下载曾遇 unexpected EOF
和停滞；本地构建临时使用 `GODEBUG=http2client=0`，并从 Go 官方代理分段补齐
一个 Cronet 模块 ZIP，通过临时 file GOPROXY 交给 Go 校验。未禁用 go.sum/校验服务，
未修改应用运行时网络配置。

最终实际完成：

- `bash platform/android/build-singbox-libbox.sh /tmp/clash-flux-libbox-1.14.2/libbox-arm64.aar`：
  退出码 0。正式 tag/revision 双重检查通过，使用 Go 1.26.8、OpenJDK 17、NDK 29；
  上游 modern/legacy arm64 构建均完成，项目暂存 modern API 24 AAR。
- AAR 元数据：version `1.14.2`、revision 与项目 pin 一致、ABI `arm64-v8a`；
  内容 SHA256 `b68f4f95851160c89d32ae27651eed557524638df617e521b2da965054e3b48c`。
- 在 `platform/android`，设置 `JAVA_HOME=/opt/android-studio/jbr`、
  `HUXERUI_HOME=/home/farna/.local/share/HuxerUI`、`ANDROID_HOME=/home/farna/Android/Sdk`、
  `CLASHFLUX_SINGBOX_AAR=/tmp/clash-flux-libbox-1.14.2/libbox-arm64.aar` 后执行
  `./gradlew :app:assembleDebug --offline --no-daemon`：成功（本地 Gradle 配置实际使用 JDK 21）。
  `stageSingboxAar` 与 `stageBundledRuleSets` 执行，Java 与固定版本 libbox 一起编译。
- 产物 `platform/android/app/build/outputs/apk/debug/app-debug.apk`，SHA256
  `92e212ea0a2b3ca692008ceadd2de463ee3a9e3f1761e399eceb81efbc838a6b`。
- `apksigner verify --verbose --min-sdk-version 23`：v1、v2 均为 true。
  这是 Debug 签名包，没有生成或发布 Release。
- 合并阶段 libbox 与新 AAR 字节一致；APK 内 libbox 与该 AAR 经 NDK
  `llvm-strip --strip-unneeded` 后字节一致。Go build info 为 go1.26.8/Android/arm64；
  APK 包含 `assets/rules/geoip-cn.srs` 与 `assets/rules/geosite-cn.srs`。

没有安装 APK、运行真机 VPN、远端握手或性能基准。构建产物与临时工具不入库。
