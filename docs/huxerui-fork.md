# HuxerUI fork 维护与应用固定版本

框架维护仓库为 [FarnaHerry/HuxerUI](https://github.com/FarnaHerry/HuxerUI)，独立本地目录为
`/home/farna/dev/cpp/mcpp/HuxerUI-fork`。`farna/main` 是长期集成分支；`main` 保留官方
`HuxerUI/HuxerUI` 基线。更新采用 merge，不 rebase 或 force push 已发布的集成历史。

Clash-Flux 当前固定 `3517ea2944662ca9c5ec59365d3c9f494e811d02`，各平台 CI 共用
`.github/workflows/build.yml` 的 `HUXERUI_REPO` / `HUXERUI_COMMIT`。本机应用使用独立的
`third_party/huxerui` checkout，不直接构建可变的框架维护工作树。Android Java 与 native
必须来自这同一份源码；CLI 使用源码目录时显式传入 `--source`。

## 补丁归属

此固定提交包含 acgu 的五个补丁提交、28 个未提交文件快照、Android 宿主默认焦点高亮修复，
以及 Clash-Flux 的八项框架修复：拖动预览跟随抓取点、Pager 反向切换、隐藏虚拟页布局、
Linux 窗口帧生命周期、Objective-C++ 的 WindowTitleBar 聚合初始化兼容、
composition 锚点在结构替换间转移（响应式断点切换不再抛 presentation anchor
logic_error）、Windows 自定义标题栏窗口的 DWM 圆角和 Windows 安装器引擎错误
提示移除"忽略继续"（防止卸载失败被忽略后新旧两套程序文件并存）。
这些修复已从应用 `cmake/patches/` 和 CMake/CI 应用步骤中移除，配置应用不会修改框架源码。

Lib-Charts、Lib-SQLite、Lib-Camera 的项目修复已收录到各自 fork 的 `farna/main`，
独立维护目录为 `/home/farna/dev/cpp/mcpp/HuxerUI-libs`。应用已移除四份扩展库补丁和
全部 CMake 自动应用步骤；通过框架的 `huxerui_use_library(URL, COMMIT)` 在构建缓存获取
固定源码，不自动采用本地 `third_party/lib-*` 或可变的维护工作树。

| 库 | 固定提交 | 项目补丁审核 |
| --- | --- | --- |
| [Lib-Charts](https://github.com/FarnaHerry/Lib-Charts/tree/farna/main) |  `8ed3e03ff265ae396b064617e2010e7229d581f2` | 饼图、96pt 紧凑绘图面与 `inner_radius` 圆环样式（环形段外弧+反向内弧、NonZero 填充，取全图最大解析值）均已包含，原两份补丁 reverse check 通过。 |
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
git -C third_party/huxerui checkout 3517ea2944662ca9c5ec59365d3c9f494e811d02
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

2026-10-17 固定版本更新到 `3517ea2`：Windows 安装器（windows_installer.cpp）的
引擎错误提示不再提供"忽略继续"选项——忽略 MajorUpgrade 卸载旧版本失败会同时
留下新旧两套程序文件，是升级残留的直接来源；错误现在只能重试或中止。本轮为
安装器专用改动，不影响应用运行时；安装/升级/卸载行为需 Windows 真机验证。

2026-10-17 固定版本更新到 `45bd211`，相对 `ba8a900` 新增两项：composition 锚点
在结构替换间转移（`UsePopup`/`UseMenu` 锚点视图在响应式断点切换时不再抛
`presentation anchor must be mounted on only one View`，回归见应用侧
`tests/test_page_transition.cpp` 的 `testResponsiveAnchorMount`）与 Windows 自定义
标题栏窗口的 DWM 圆角（`DWMWA_WINDOW_CORNER_PREFERENCE`，Windows 10 忽略）。
推送时发现 `bd8fb05` 曾把维护工作树里进行中的 backdrop_effect 代码一起提交
（依赖未提交的 `effects.h` / `effects_internal.h` 与样式字段，树不完整无法编译），
`45bd211` 移除这些代码块后固定树恢复自洽；backdrop 特性仍完整保留在维护仓库
工作区，等待作为完整提交集成。Clash-Flux 已用新 checkout 重新编译并通过
`./run.sh --version`；锚点转移语义与 `Unmount` 幂等性在 Linux Debug 下由
分页交互回归覆盖。本轮 Windows 圆角与响应式窗口实际表现、Android 真机、
macOS/iOS 原生仍未运行，需随对应平台 CI 与真机验证。

2026-10-11 固定版本更新到 `ba8a900`，相对 `446a5a9` 新增三项：`ImageFit::Cover`
源矩形按固有尺寸收口（运行时渲染修复）、Android 宿主 focus instrumentation 检查
加载 native HuxerUI、fork 集成文档记录 10月7日真机验证。Clash-Flux 已用独立
checkout 重新编译并通过 `./run.sh --version`（v0.3.29），启用
`CLASHFLUX_REQUIRE_PROJECT_TESTS=ON` 后全部 17 项 `clashflux-required` 桌面回归
通过（含 page_transition 分页交互、persistence、cps_dependencies）。本轮在 Linux
Debug 执行；Windows/macOS/iOS 原生与 Android 真机构建未运行，`ImageFit::Cover`
修复需随对应平台 CI 验证。

2026-10-08 固定版本包含 `c40a2da` 的深层拉伸布局修复和 `446a5a9` 的 Windows 图标句柄成员位置修正。
主轴有界且拉伸交叉轴已确定时，Row/Column 直接用最终分配尺寸测量 Grow 子树，避免逐层重复测量；松交叉轴与无界主轴仍保留自然尺寸计算。
独立回归中，12 层 Row/Column 的叶子测量从 4096 次降为 1 次，同时验证加权分配、固定项、缩放和十万项虚拟列表滚动。
框架 Linux Debug 构建、5 个新增回归及全部 4 组 common CTest 通过；Clash-Flux 用独立 checkout 重新构建，启用 `CLASHFLUX_REQUIRE_PROJECT_TESTS=ON` 后 15 项必跑回归全部通过，`./run.sh --version` 为 v0.3.28。
隔离复制当前三个配置的 Linux Debug/X11 GUI 滚动采样中，测量阶段最大耗时从约 1663ms 降至约 42ms，新版本 228 个采样帧均未超过 100ms；这是当前桌面 Debug 测量，不代表 Release 或其它平台帧率。
本轮未执行 Windows/macOS/iOS/Android 原生构建与运行；Windows 图标改动只修正已有提交的成员声明位置，仍需对应平台 CI 验证。


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

## 应用 CPS 接入边界

扩展库 URL/COMMIT 的唯一声明移至 `cmake/dependencies/Lock.cmake`，原生构建
和 Android 库图继续通过 `huxerui_use_library` 获取同一身份，不升级当前固定提交。
HuxerUI 的 codegen、资源、宿主工具和 Java/native 图仍走框架接入；通用依赖的
CPS 迁移不替代这些流程，见 [依赖管理](cps-dependencies.md)。
