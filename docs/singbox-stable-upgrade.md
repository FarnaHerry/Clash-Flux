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
   `cmake/singbox_bundle.cmake` 的 `CLASHFLUX_SINGBOX_VERSION` 是 UI 内核标识
   （`cfg::singboxFallbackLabel()`）的唯一来源，更新 pin 后设置页/首页展示自动联动。
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
   `HUXERUI_HOME=$HOME/.local/share/HuxerUI`、`ANDROID_HOME=$HOME/Android/Sdk`、
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

## 2026-10-19：升级到 1.14.3

由 1.14.2 升级到官方最新稳定版 **1.14.3**，发布于 2026-10-14；源码 revision：
`7054cac5124465ffdb145870d7c01885566711b1`（v1.14.3 tag 直指该 commit）。
不跟随 1.15 alpha。依据为[官方 Release](https://github.com/SagerNet/sing-box/releases/tag/v1.14.3)
和[发布资产 API](https://api.github.com/repos/SagerNet/sing-box/releases/tags/v1.14.3)。

桌面官方归档摘要固定在 `cmake/singbox_bundle.cmake`，六个资产在本机
`sha256sum` 与发布资产 API digest 逐一核对一致：

| 1.14.3 资产后缀 | 官方 SHA256 |
|---|---|
| `linux-amd64.tar.gz` | `e2bdf179c15a3652955dc44867e33ca0965c9c9b91b34267dcc5a5d639a5feee` |
| `linux-arm64.tar.gz` | `29dae24d76bea3bc62d07dd3e60cfc12003de7debbd6b768df9be9c68360174e` |
| `windows-amd64.zip` | `d8b86ef58936dba9230ec6a57ffa4243933f3c14ef4809828b6c553440ed1518` |
| `windows-arm64.zip` | `195b35424c6e3f5913954c7f08c5d8ba2d2199f6d18eb27a3972a5a29cdaa78c` |
| `darwin-amd64.tar.gz` | `6d002b74d03b478547349cc10af89e007868596e5aa931816d45da9f544715a9` |
| `darwin-arm64.tar.gz` | `0360ce634a04c26a6b4fd957517987bb4f6c8950f63435ce3c5af2fcae5860c5` |

上游兼容性审查（对比 v1.14.2…v1.14.3 共 30 个提交）：

- `option/` 目录零变更，配置 schema 完全一致；`docs/changelog.md` 对 1.14.3
  仅记 "Fixes and improvements"。编译器映射与保真度账本内容不变，仅更新
  版本/固定 revision 引用。
- `experimental/clashapi/proxies.go` 未变，selector/urltest 控制语义保持；
  `clashapi/server.go` 只是把 HTTP server 关闭改挂到启动 scope 的内部重构。
- `experimental/libbox/command_client.go` 未变；libbox 增量新增
  `SetupOptions.PlatformMetadata` 与 `CommandServer.WakeNow`/`RecordScreenState`，
  Android 的 `Pause()` 额外调用 `CloseIdleConnections()`（与本应用
  「先更新物理接口再关闭旧连接」的后台生命周期一致）。AAR 由 CI 按固定
  revision 重新生成，未在本地沿用旧二进制。
- 组出站新增内部 `adapter.Referrer` 实现，`SelectOutbound` 选择语义未变；
  urltest 对多路复用出站在计时前做 keep-session 预热，属测量精度修复。

实际执行并通过：

- `cmake --build build --target clash-flux`：configure 期下载 1.14.3
  linux-amd64 归档并通过 EXPECTED_HASH 校验，POST_BUILD 打包 v1.14.3。
- `./run.sh --version` 与 `build/engines/sing-box version`：1.14.3，
  revision 与 pin 一致，Go 1.26.8、CGO disabled。
- `ctest --test-dir build -L clashflux-required --output-on-failure
  --no-tests=error`：17/17 通过（含 project_test_gate、cps_dependencies、
  dns_hosts/policy/tls_runtime、persistence）。
- `git diff --check`：通过。

本机未重新构建 Android AAR 与 iOS xcframework；两者由 CI 与各平台脚本按
同一固定 revision 生成，AAR 版本/revision/ABI/SHA256 元数据校验不通过会
直接失败。真机 VPN、远端握手与服务端兼容性仍需实机验证。

## 2026-10-28：1.14.3 → 1.14.4

基于 `98da670` 的未提交工作区，固定官方稳定版
[v1.14.4](https://github.com/SagerNet/sing-box/releases/tag/v1.14.4)，revision
`c005f35300ce76427dc285481ec5695f5b179cd2`。官方发布说明虽只写修复与改进，仍按
[完整源码差异](https://github.com/SagerNet/sing-box/compare/v1.14.3...v1.14.4) 检查兼容性。

| 1.14.4 资产后缀 | 官方 SHA256 |
|---|---|
| `linux-amd64.tar.gz` | `699a1b510aa4396b2176d32d1181920399ebf1e267835da6f3b598d0f7c24968` |
| `linux-arm64.tar.gz` | `436441f76ee2f7fe21b5b2d426d3cb261a480455a704865789ad533e4273ec26` |
| `windows-amd64.zip` | `99185d75d314a07dbba4a130c8ad986cbeb629a07927975636b341ca61f991df` |
| `windows-arm64.zip` | `8cd0a4d0cd8cd480be5731ac31f17038969a42d12add9feb6e031fd43d85bbae` |
| `darwin-amd64.tar.gz` | `eae86b9b033bba50b7c44ab719e4bfac0aca05efd28483390ed3aa510f32db25` |
| `darwin-arm64.tar.gz` | `0d8c7f5ed5d587b0f1a5e9772a708ec3bfa4ac4c9329604b01d8d9c2ba406838` |

摘要来自官方 expanded_assets 发布页的逐项 digest；CMake 实际下载并校验 Windows x64
归档。桌面 CMake、CI 与 Android/iOS 脚本、Gradle 全部对齐同一 revision。

兼容性处理：

- selector/urltest 新增重复 tag 拒绝。旧展开示例会产生重复成员，真实内核 check 已复现失败。
  Clash 组编译保留首次位置、不同成员顺序及有效 default，每个重复项记 Group/Approx；原订阅不改写。
  显式组及自动展开共用处理，原生 JSON 不自动改写。单位回归和真实 SOCKS 转发/组切换验证通过。
- `option/rule_action.go` 移除 `action:direct`。托管 DIRECT 仍通过 direct 出站路由，未使用该动作；
  固定新内核实测旧动作拒绝、route + outbound:direct 接受。用户原生配置仍由候选校验拒绝，不做静默修复。
- libbox PlatformInterface 移除 useProcFS。API 24–28 的 owner 回调显式调用新增
  Libbox.findConnectionOwner，API 29+ 继续系统 UID 查询；保留包名解析、非空结果与错误反馈。
- Clash API 和 StartedService 组状态内部改用 TCP Selected，现有组/选择/版本读取通过真实接口回归；
  真实版本断言改为与同一固定内核的 version 输出核对，不再写死前一版本。

实际验证：Windows MSVC Release 主目标、test_singbox/test_runtime_health/test_page_transition 构建
及 HuxerUI codegen 通过；page_transition、ui_resources、singbox、native_control_runtime、
group_expansion_runtime、dns_hosts_runtime、dns_policy_runtime、dns_tls_runtime、runtime_health、
personal_rule_sets_runtime 共 10/10 通过（94.81s）。打包内核输出 1.14.4 和上述 revision；
应用版本仍为 v0.3.74。运行脚本版本与 git diff --check 通过。

Android 实际执行 build-singbox-libbox.sh，使用正式 tag 构建 arm64-v8a AAR，
SHA256 `c3125f8d3bc55dd26a7310534528fbe96a2278ea6eba52f6b67ecc5a1ce21ccc`；
stageSingboxAar 与 stageBundledRuleSets 通过，未伪造旧 AAR 的来源元数据。
新 AAR 的 PlatformInterface/Libbox 导出已用 javap 核对，生产 findConnectionOwner 方法与
StringArray 原样提取后，用 JDK 17、Android API 36 和新 classes.jar 独立 javac 编译通过。
完整 compileDebugJavaWithJavac 在前置 configureCMakeDebug 失败：Gradle 未找到 CMake 4.4.2；
该独立编译不等于完整 Java/APK 构建或真机 VPN 通过。未做 Android 真机、其它桌面平台、
iOS xcframework、服务安装升级或完整 required 门禁；未提交、推送或发布。
