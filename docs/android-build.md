# Android 构建与运行时版本一致性

Android UI 由 HuxerUI Java 模块和 C++ native 模块共同组成，两者必须使用同一份 HuxerUI
源码或 SDK。Clash-Flux 的 CMake 优先选择显式传入的 HuxerUI 源码，其次选择仓库内
`third_party/huxerui`，最后才使用已安装 SDK。`platform/android/settings.gradle` 按相同顺序
选择 Java 模块，并把该路径传给 CMake。

源码模式下，Gradle 编译所选源码中的 `platform/android/huxerui`；SDK 模式下，Gradle 使用
`HUXERUI_HOME/share/huxerui/platform/android/HuxerUI.aar`。不能把源码构建的 native 库与旧 SDK
AAR 混装，否则 Java 层可能调用 native 层已经移除或改变的入口，导致应用在首帧创建时崩溃，
例如 `HuxerUI Android application has not been initialized`。

本地构建仍需提供 CLI 和 SDK 元数据所需的 `HUXERUI_HOME`：

```bash
export HUXERUI_HOME=/path/to/huxerui-sdk
huxerui build android --profile debug
```

如果 `third_party/huxerui` 存在，Android Java 和 C++ 模块会一起从该源码构建；CI 显式传入
`--source` 时，则两者一起使用该指定源码。构建配置会打印 Gradle 实际选中的 Android Java
模块或 AAR 路径，可用于确认没有混用版本。
