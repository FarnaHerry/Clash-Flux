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

当前固定 sing-box 1.14.2，源码 revision
`af6e64c3b69e6132ebaee0e1a3d24e93903f6709`，与桌面发布内核一致。
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
