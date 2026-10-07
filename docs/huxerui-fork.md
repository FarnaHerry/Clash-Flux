# HuxerUI fork 维护与应用固定版本

框架维护仓库为 [FarnaHerry/HuxerUI](https://github.com/FarnaHerry/HuxerUI)，独立本地目录为
`/home/farna/dev/cpp/mcpp/HuxerUI-fork`。`farna/main` 是长期集成分支；`main` 保留官方
`HuxerUI/HuxerUI` 基线。更新采用 merge，不 rebase 或 force push 已发布的集成历史。

Clash-Flux 当前固定 `3e16bf4ccb02aa3dfd7699f3342fe3a911e3e550`，各平台 CI 共用
`.github/workflows/build.yml` 的 `HUXERUI_REPO` / `HUXERUI_COMMIT`。本机应用使用独立的
`third_party/huxerui` checkout，不直接构建可变的框架维护工作树。Android Java 与 native
必须来自这同一份源码；CLI 使用源码目录时显式传入 `--source`。

## 补丁归属

此固定提交包含 acgu 的五个补丁提交、28 个未提交文件快照、Android 宿主默认焦点高亮修复，
以及 Clash-Flux 的五项框架修复：拖动预览跟随抓取点、Pager 反向切换、隐藏虚拟页布局、
Linux 窗口帧生命周期和 Objective-C++ 的 WindowTitleBar 聚合初始化兼容。
这些修复已从应用 `cmake/patches/` 和 CMake/CI 应用步骤中移除，配置应用不会修改框架源码。

Lib-Charts、Lib-SQLite、Lib-Camera 的项目修复已收录到各自 fork 的 `farna/main`，
独立维护目录为 `/home/farna/dev/cpp/mcpp/HuxerUI-libs`。应用已移除四份扩展库补丁和
全部 CMake 自动应用步骤；通过框架的 `huxerui_use_library(URL, COMMIT)` 在构建缓存获取
固定源码，不自动采用本地 `third_party/lib-*` 或可变的维护工作树。

| 库 | 固定提交 | 项目补丁审核 |
| --- | --- | --- |
| [Lib-Charts](https://github.com/FarnaHerry/Lib-Charts/tree/farna/main) | `9c183e87393ef589f3bf20dd8d80281a1fe9c9cd` | 饼图与 96pt 紧凑绘图面均已包含，原两份补丁 reverse check 通过。 |
| [Lib-SQLite](https://github.com/FarnaHerry/Lib-SQLite/tree/farna/main) | `88f610fe66c57260b1ac38e676fc36b23c900fc2` | 启动临时只读查询已包含，原补丁 reverse check 通过；另保留逐值 UTF-8 校验与主键 schema 兼容修复。 |
| [Lib-Camera](https://github.com/FarnaHerry/Lib-Camera/tree/farna/main) | `b8b3b9304646cdc1afb9722ead16d85cf2160610` | 已采用 ApplicationContext/UiWindow；Android/iOS/Windows/Linux 行为覆盖原补丁。macOS 用直接 UiWindow factory，当前 backend 不消费 NSWindow，因此与原 typed wrapper 等价；库图模式早退也已包含。 |

CMake 原生构建和 CLI 的 Android 库图共用这些 HTTPS URL 与完整 SHA。Camera 的库图
兼容已由库本身处理，应用不再预声明占位目标。旧的本地 Charts/SQLite 工作树及旧
FetchContent 源码缓存仍原样保留，不能因迁移清理它们的未提交改动。

本次迁移保留原 Clash-Flux 框架工作树在 `third_party/huxerui-before-farna`；其中原有六个
修改文件未丢弃。acgu 的原工作树也未改动。其它姊妹项目按各自验证结果逐个迁移。

## 检出与升级

新环境检出应用依赖：

```bash
git clone https://github.com/FarnaHerry/HuxerUI.git third_party/huxerui
git -C third_party/huxerui checkout 3e16bf4ccb02aa3dfd7699f3342fe3a911e3e550
```

在独立维护仓库中先 `git fetch upstream main`，审查官方更新和集成差异，再在 `farna/main`
执行 `git merge upstream/main`。官方已有等价修复时消除重复差异；不要把维护仓库切换分支的
过程传播给正在构建的应用 checkout。更新官方基线时只允许快进 `main`，不要将应用补丁合入它。

框架与应用验证通过后，发布集成提交，再把本机依赖、本文固定 SHA 和 workflow pin 一起更新。
Clash-Flux 必须重新运行 `cmake --build build --target clash-flux`、`./run.sh --version`、
`git diff --check`，并运行生产分页交互及桌面必跑回归。扩展库同样先审核并验证集成分支，
再更新 CMake 固定提交与本文；不要在应用配置时修改库源码。Android 构建必须保留 GEOIP/GEOSITE 打包及 Java/native
同源约束，桌面门禁和 iOS 暂缓政策保持不变。

## 验证边界

此次框架合入通过 Linux Debug 运行时与独立头文件构建、Release 框架构建和四个 common
CTest 套件（含 953 个 runtime 用例）。新增回归覆盖跨标签反向时的几何连续性、视口边缘
拖动抓取点和隐藏虚拟页重新显示后的测量。

Clash-Flux 已在此 SHA 重新编译，通过 `./run.sh --version`（v0.3.27）、全部 15 项
`clashflux-required` 桌面回归和 Android arm64 Debug APK 构建（显式 `--source third_party/huxerui`，
Java/native 同源且保留 `stageBundledRuleSets`）。扩展库补丁已按上表逐项审核为集成或等价实现，
因此不再保留应用层重复补丁。

2026-10-07 扩展库迁移后重新执行 `cmake --build build --target clash-flux` 与
`./run.sh --version`，15 项 CTest 回归全部通过（含 SQLite ORM、持久化迁移与重开、启动只读查询），
Android arm64 Debug APK 构建也通过。独立的全新库图配置成功；桌面、库图、Android native
缓存的三个 checkout 均与固定 URL/SHA 一致且无源码修改，Android Java 库图使用相同提交。

此前完整框架 CTest 为 30/32；两项失败已在官方基线复现，详情见 fork 的
[集成文档](https://github.com/FarnaHerry/HuxerUI/blob/farna/main/docs/development/farna-integration.md)。
Android 真机框架 instrumentation 尚未完成，Windows/macOS/iOS 原生运行也未在此 Linux
主机验证；不能把编译通过或 common 回归通过表述为这些平台运行验证通过。
