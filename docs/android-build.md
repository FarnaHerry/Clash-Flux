# Android 构建与运行时版本一致性

Android UI 由 HuxerUI Java 模块和 C++ native 模块共同组成，两者必须使用同一份 HuxerUI
源码或 SDK。Clash-Flux 的 CMake 优先选择显式传入的 HuxerUI 源码，其次选择仓库内
`third_party/huxerui`，最后才使用已安装 SDK。`platform/android/settings.gradle` 按相同顺序
选择 Java 模块，并把该路径传给 CMake。

源码模式下，Gradle 编译所选源码中的 `platform/android/huxerui`；SDK 模式下，Gradle 使用
`HUXERUI_HOME/share/huxerui/platform/android/HuxerUI.aar`。不能把源码构建的 native 库与旧 SDK
AAR 混装，否则 Java 层可能调用 native 层已经移除或改变的入口，导致应用在首帧创建时崩溃，
例如 `HuxerUI Android application has not been initialized`。

框架源码来自 `FarnaHerry/HuxerUI` 的 `farna/main`，与 CI 固定到同一个 SHA；
源码包含 Android 整个宿主视图的默认焦点高亮修复。框架真机 instrumentation 仍待完成，
不能把 Linux 回归或 APK 编译成功记作 Android 真机测试通过。维护流程见 [HuxerUI fork](huxerui-fork.md)。

本地构建仍需提供 CLI 和 SDK 元数据所需的 `HUXERUI_HOME`：

```bash
export HUXERUI_HOME=/path/to/huxerui-sdk
huxerui build android --profile debug
```

如果 `third_party/huxerui` 存在，Android Java 和 C++ 模块会一起从该源码构建；CI 显式传入
`--source` 时，则两者一起使用该指定源码。构建配置会打印 Gradle 实际选中的 Android Java
模块或 AAR 路径，可用于确认没有混用版本。

## sing-box libbox 来源与缓存

当前固定 sing-box 1.14.4，源码 revision
`c005f35300ce76427dc285481ec5695f5b179cd2`，与桌面发布内核一致。
1.14.4 移除 PlatformInterface.useProcFS；API 24–28 在 findConnectionOwner 回调中显式调用
Libbox.findConnectionOwner，API 29+ 继续使用 ConnectivityManager，均保留包名查询和失败反馈。
`platform/android/build-singbox-libbox.sh /absolute/path/libbox-arm64.aar` 会同时生成
`libbox-arm64.aar.metadata.json`，包含正式版本、实际 checkout revision、arm64-v8a ABI 与 AAR
SHA256。设置 `CLASHFLUX_SINGBOX_AAR` 时，两份文件必须在一起。

Gradle `stageSingboxAar` 验证来源和摘要后再复制；无环境变量时只复用身份校验通过的
已暂存 AAR。旧 AAR 缺少元数据或版本不符会明确失败，须重新运行构建脚本；不能直接
给旧产物手写新 revision 元数据。CI 使用同一脚本生成两份文件，不下载第三方 APK
中的 native 库。此流程不改变 `stageBundledRuleSets` 的强制规则集打包。

Android libbox 构建只浅取正式版本 tag，核对 tag 对应的 HEAD 与固定 revision；
保留真实 tag，避免上游 `ReadTag()` 在浅取裸 commit 后把版本写成 unknown。
AAR 元数据包含 version，Gradle 同时核验版本、revision、ABI 与内容摘要。
自定义 pin 时必须同时提供 `SINGBOX_VERSION` 和 `SINGBOX_COMMIT`，不伪造版本标签。

上游构建要求 OpenJDK 17，脚本在获取源码前检查 JAVA_HOME；Gradle 使用的 JDK
可以单独配置。完整升级记录见[稳定内核升级](singbox-stable-upgrade.md)。

共享 JSON codec 使用固定 Glaze 9.0.0；Android Legacy 头只引用拥有型 DTO，
`clashflux_wire` 单独编译 Glaze，不合入领域模块的兼容大 TU。接入与验证边界见
[迁移记录](glaze-migration.md)。inline provider 与逻辑规则子集也用于 Android，
但不增加手机多订阅；routing-mark 不能因 Android 的 `__linux__` 宏而开放。
PROCESS-NAME/正则转原生包名匹配，仍通过后台 owner 查询；应用选择 UI 尚未实现。

MainActivity 的 BROWSABLE/VIEW intent filter 接收 sing-box 与 FlClash 订阅链接；
冷启动和运行中 intent 都经同版本 HuxerUI application activation 打开导入二级页，
不新增普通订阅多选能力。格式、解析约束与验证方式见[订阅唤醒链接](profile-links.md)。

## 发布 APK 签名

Android Release APK 必须同时启用 v1（JAR 签名，包含 `META-INF` 签名文件）和 v2 签名。
虽然现代 Android 支持 v2，但部分 OEM 安装器仍会因为缺少传统签名文件而将 v2-only APK
报告为未签名。GitHub Actions 使用 `apksigner verify --verbose --min-sdk-version 23` 并分别
断言 v1、v2 均为 `true`；由于应用的 minSdk 是 24，默认核验会跳过 v1，因此只检查默认命令
退出成功不能防止该兼容性回归。

Release APK 必须使用长期发布 keystore，不能使用 Gradle 生成的 debug keystore。正式发布证书
SHA-256 指纹为 `a4a0bc09a3dc76d7ac37f92043d2be6e153bda1347ce344013828e15472419f4`；CI 用
`apksigner verify --print-certs` 同时核对 v1、v2 签名和该指纹。Release 打包任务缺少发布密钥
时会失败，不能以 debug 签名包替代。

应用内 APK 更新沿用该指纹，在 HUI 平台模块工作线程校验下载摘要、包名、版本及已安装/候选签名，
通过私有只读 Provider 打开系统安装界面；不自动卸载 debug 包或清除用户数据。安装授权与生命周期见
[应用升级](app-updates.md)。Gradle `preBuild` 必跑 `:app:testAppUpdatePolicy`（宿主 JVM 的真实文件/JCA 回归）。
`preBuild` 也运行 `:app:testRuntimeStartPolicy` 与 `:app:testVpnUptime`，覆盖启动权限及后台
会话时钟契约；独立执行命令与实机验收边界见[Android 生命周期](android-background-lifecycle.md)。

## 发布版本号

发布时 `platform/android/app/build.gradle` 的 `versionCode` 在上一发布基础上 +1，
`versionName` 与 `CMakeLists.txt` 的 project VERSION 保持完全一致，并与 CMake 版本号
放进同一次发布提交。CI 不校验两侧版本号的一致性：漏改或冲突解决把该文件退回旧值都不会
让流水线失败，只会体现在用户安装包上（v0.3.34 曾因此把 0.3.28 的版本号打进发布包）。
发布推送前用 `git show <tag>:platform/android/app/build.gradle | grep version` 核对已
提交内容。

## CMake / CPS 依赖基线

Android native 与桌面统一要求 CMake 4.4，验证版本为 4.4.2。Gradle 的 CMake
版本声明和 CI 的 pip 安装必须一致；`local.properties` 或用户 Gradle 属性中的
`cmake.dir` 应指向该安装前缀。通用 C/C++ 依赖由目标 NDK 工具链构建为 CPS 包，
缓存区分 ABI、API、编译器、flags 和配置；不能拿宿主机包给 Android 链接。
HuxerUI Java/native 同源、固定 libbox revision 和 `stageBundledRuleSets` 保持原约束。
流程见 [CPS 依赖管理](cps-dependencies.md)。
