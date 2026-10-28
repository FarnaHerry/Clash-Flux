# HuxerUI fork 维护与应用固定版本

框架维护仓库为 [FarnaHerry/HuxerUI](https://github.com/FarnaHerry/HuxerUI)，独立本地目录为
`../HuxerUI-fork`。`farna/main` 是长期集成分支；`main` 保留官方
`HuxerUI/HuxerUI` 基线。更新采用 merge，不 rebase 或 force push 已发布的集成历史。

Clash-Flux 当前固定 `be67da4a898856382bb8d4876a6c8ce71d889f5e`，各平台 CI 共用
`.github/workflows/build.yml` 的 `HUXERUI_REPO` / `HUXERUI_COMMIT`。本机应用使用独立的
`third_party/huxerui` checkout，不直接构建可变的框架维护工作树。Android Java 与 native
必须来自这同一份源码；CLI 使用源码目录时显式传入 `--source`。

2026-10-26 更新到 `3a6c18e`：上游 `main` 的 `0c51262` HTTP streaming 修复已包含在集成分支；
最新 fork 提交修复框架 warning，并更新 Windows renderer 头文件和多窗口测试。
Windows MSVC 主目标、UI codegen 及 10 项定向回归通过。具体构建 warning 和平台未覆盖范围见
[L1 / L2 / L3 验收记录](l1-l2-l3-status.md)。

2026-10-28 更新到 `5873eeb`：`farna/main` 合入上游应用级多窗口重构并增加 Linux runtime 部署安全检查。
这是破坏性 API 更新：窗口创建改为 `ApplicationHandle::OpenWindow(content, options)`，每个窗口必须提供自己的
`ViewFactory`；旧 `Runtime::OpenWindow(WindowOptions)`、`WindowOptions::key` 和 `WindowIdentity` 已移除。
Clash-Flux 的托盘唤起与无窗启动路径都显式复用 `AppRoot`，并继续由应用级服务共享数据模型。
窗口关闭现在有明确的 last-window 退出策略；应用关闭确认/托盘隐藏继续由应用自己的 close handler 控制。
Linux 包安装时框架要求 patchelf ≥0.19.0，工作流在配置和 CPack 前下载官方 0.19.2 并按架构核对 SHA-256。
`HUXERUI_COMMIT`、本机独立 checkout 与 Android Java/native 构建统一固定到上述提交。

2026-10-28 更新到 `be67da4`：修复 SDK 的 Win32 `CreateWindow` 宏与 Runtime 工厂方法冲突，
将工厂重命名为 `CreateApplicationWindow` 并同步平台实现、测试和文档；同时修复 macOS 合并回归，
让窗口关闭命令走 `UiWindow::CloseWindow` / Runtime 生命周期，移除残留的 `QueueClose` 调用。
Windows 全量框架构建与 24 项 CTest 通过；macOS 编译和原生生命周期由 fork CI 验证。
macOS 原生 bundle 编译成功，但生命周期 CTest 为 10/12：
`HuxerUIMacOSNativeWindowFrameFailuresPropagateFromTheSharedLoop` 在关闭后的窗口释放断言失败，
`HuxerUIMacOSNativeRetirementKeepsWindowlessRuntimeAliveAndCanReopen` 超时。键盘派发关闭测试通过；
CI 诊断器仍尝试运行该键盘测试并最终超时，两个失败尚未定位或修复。

2026-10-28 应用接入上游 `AppOptions::quit_on_last_window_closed=false`：当前固定 SDK 已包含该选项，
无需升级框架 pin。轻量模式使用普通窗口关闭及 OpenWindow 重建，应用级协程由
UseApplicationTaskScope 持有，不再消费 fork 的 Retire 原语。该选项控制最后一个窗口关闭后的
Runtime 退出策略，不等于隐藏窗口或清空全部框架缓存。Windows 原生隔离夹具验证三次关窗/重建、
无窗协程和模型存活、默认 last-window 退出及显式 Quit；不替代尚未通过的 macOS 原生生命周期回归。

## 补丁归属

此固定提交包含 acgu 的五个补丁提交、28 个未提交文件快照、Android 宿主默认焦点高亮修复，
以及 Clash-Flux 的十一项框架修复：拖动预览跟随抓取点、Pager 反向切换、隐藏虚拟页布局、
Linux 窗口帧生命周期、Objective-C++ 的 WindowTitleBar 聚合初始化兼容、
composition 锚点在结构替换间转移（响应式断点切换不再抛 presentation anchor
logic_error）、Windows 自定义标题栏窗口的 DWM 圆角、Windows 安装器引擎错误
提示移除"忽略继续"（防止卸载失败被忽略后新旧两套程序文件并存）、Windows 安装器
命令驱动运行修复（命令式安装/卸载绕过新版本闩锁、不阻塞收尾并按终端状态映射
退出码、保留引擎停止与 bundle 注销，修复升级时每个陈旧 bundle 注册弹一页且
注册跨版本累积的根因）、Windows 安装器 embedded/quiet 隐藏原生 HWND 与退出等待修复，
以及 iOS 链接闭包修复（link.rsp 解析 app 目标自有传递
依赖 + IOS 平台补 CoreMedia 框架声明）。
这些修复已从应用 `cmake/patches/` 和 CMake/CI 应用步骤中移除，配置应用不会修改框架源码。

Lib-Charts、Lib-SQLite、Lib-Camera、Lib-CodeEditor 的项目修复已收录到各自 fork 的 `farna/main`，
独立维护目录为 `../HuxerUI-libs`。应用已移除四份扩展库补丁和
全部 CMake 自动应用步骤；通过框架的 `huxerui_use_library(URL, COMMIT)` 在构建缓存获取
固定源码，不自动采用本地 `third_party/lib-*` 或可变的维护工作树。

| 库 | 固定提交 | 项目补丁审核 |
| --- | --- | --- |
| [Lib-Charts](https://github.com/FarnaHerry/Lib-Charts/tree/farna/main) | `e90cfa84707bdf72eb6029410bf05c420efa62fc` | 饼图、96pt 紧凑绘图面与 `inner_radius` 圆环样式（环形段外弧+反向内弧、NonZero 填充，取全图最大解析值）均已包含，原两份补丁 reverse check 通过；包含 `rounded_caps`、中心占比/180ms 悬停动画，全零数据按条目数等分且中心保持 0%。 |
| [Lib-SQLite](https://github.com/FarnaHerry/Lib-SQLite/tree/farna/main) | `88f610fe66c57260b1ac38e676fc36b23c900fc2` | 启动临时只读查询已包含，原补丁 reverse check 通过；另保留逐值 UTF-8 校验与主键 schema 兼容修复。 |
| [Lib-Camera](https://github.com/FarnaHerry/Lib-Camera/tree/farna/main) | `b8b3b9304646cdc1afb9722ead16d85cf2160610` | 已采用 ApplicationContext/UiWindow；Android/iOS/Windows/Linux 行为覆盖原补丁。macOS 用直接 UiWindow factory，当前 backend 不消费 NSWindow，因此与原 typed wrapper 等价；库图模式早退也已包含。 |
| [Lib-CodeEditor](https://github.com/FarnaHerry/Lib-CodeEditor/tree/farna/main) | `a332e7c3158818a891bb8181756d439a62df6268` | 包含只读复制、滚动/绘制兼容；补齐固定子模块声明，提供工作线程预构建并跨卸载保留的 `initial_document`，向消费者传播 Windows 宏约束，默认关闭输入追踪，并修复工具栏剪贴板、文本变更通知和 Android 库图接口目标。Linux 从固定 simdutf v7.7.0 源码按 libc++ 工具链重建，复用 SweetEditor 同版本兼容头（仅进入构建期 include），并移除已合入静态库的冗余链接边，兼容 Linux 与 iOS 链接闭包。 |

CMake 原生构建和 CLI 的 Android 库图共用这些 HTTPS URL 与完整 SHA。Camera 的库图
兼容已由库本身处理，应用不再预声明占位目标。旧的本地 Charts/SQLite 工作树及旧
FetchContent 源码缓存仍原样保留，不能因迁移清理它们的未提交改动。

本次迁移保留原 Clash-Flux 框架工作树在 `third_party/huxerui-before-farna`；其中原有六个
修改文件未丢弃。acgu 的原工作树也未改动。其它姊妹项目按各自验证结果逐个迁移。

## 检出与升级

新环境检出应用依赖：

```bash
git clone https://github.com/FarnaHerry/HuxerUI.git third_party/huxerui
git -C third_party/huxerui checkout be67da4a898856382bb8d4876a6c8ce71d889f5e
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

### Windows legacy bundle migration

Windows 安装器从旧 `dev.farna.clashflux.bundle` 转到 `dev.farna.clashflux.bundle.v2` 的确定性 Burn UpgradeCode，避免新安装器调用旧缓存中无法静默执行的 bootstrapper。MSI UpgradeCode 保持不变，旧产品仍由 `MajorUpgrade` 在 MSI 事务内移除，失败时保留回滚能力。新框架修复在独立维护仓库中验证后才更新应用固定提交；未提交的框架修改不能成为 CI pin。

MSI 内嵌静态 CRT 的原生清理 DLL，在安装/修复成功提交时以系统权限执行；卸载和回滚路径不清理。它只接受 HKLM 64 位卸载项中名称、发布者、旧 UpgradeCode、GUID ProviderKey 和 0.3.0–0.3.68 版本均匹配的 Burn Bundle。新升级标识的 Bundle、MSI 产品、其它软件、用户数据库和订阅均不匹配。

清理仅删除 `%ProgramData%\Package Cache\{BundleId}` 内与注册版本一致的 Setup.exe、`state.rsm` 和空目录，拒绝路径不符、重解析点和额外文件；不递归删除共享 MSI 载荷。随后移除对应依赖项的旧 Bundle 引用、无其它依赖者的旧 Provider 和卸载注册。存在其它依赖者时保留该 Bundle；权限或缓存占用错误写入 MSI 日志，并保留尚未完成的注册以供下一次安装/修复重试，不使已成功安装的产品失败。

`legacy_bundle_cleanup` 回归使用独立 HKCU 注册表树和临时目录，无需管理员权限，不改变真实安装记录。发布前另需 Windows 实机验证旧版到高于 0.3.68 的升级、失败回滚、新版静默卸载和维护页；不能用相同版本重新打包替代跨版本验收。

2026-10-26 本机只读盘点发现旧升级标识下的 Bundle 0.3.60、0.3.61、0.3.62、0.3.66、0.3.68 各一条及各自缓存，另有当前 0.3.68 MSI 注册。当前 Bundle 与 MSI 两条记录不等于两份应用。盘点没有执行真实注册或缓存删除。

Windows/MSVC 构建、隔离迁移、真实 Burn 测试产品及未完成边界见 [L1 / L2 / L3 验证记录](l1-l2-l3-status.md)。

2026-10-22 固定版本更新到 `9333c98`：Windows Burn 安装器在 embedded 与 quiet 模式下
不创建 HuxerUI 窗口，直接执行检测、计划、应用和退出；普通完整界面保持原样。
同时修复 UI 线程调用 Burn Quit 时的退出等待，并保留引擎错误与文件占用的非交互应答。
应用 MSI 将 `RemoveExistingProducts` 排在 `InstallInitialize` 之后，使旧版移除加入 MSI 回滚事务。
HuxerUI Windows 测试与应用独立固定源码构建的安装器目标通过；应用版本命令返回 0，
桌面必跑回归 15/17 通过。`windows_service` 因当前机器服务管道不可用而失败，`vpn`
仍以 `0xc0000409` 异常退出，未将两项从门禁移除。
旧版本已缓存的 Bundle 不会因新框架发布而改变，仍需 Windows 实机完成跨版本升级复核。

2026-10-22 固定版本更新到 `a954965`：新增桌面应用选项
`AppOptions::create_initial_window`，默认仍自动创建首窗；设为 `false` 时，Windows、Linux、
macOS 跳过首窗创建及界面组合，应用级托盘、任务、定时器和事件循环继续运行，后续复用
`ApplicationHandle::OpenWindow()` 创建窗口。`extra_windows` 独立处理，手机、Web 与嵌入式
宿主维持平台驱动的窗口生命周期。Clash-Flux 桌面统一跳过框架首窗，在 Runtime 进入 Running 后
创建窗口；启用托盘且打开“启动时隐藏到托盘”时，等待最多两秒获取异步托盘可用性，
可用且图标发布成功则保持无窗，由托盘激活创建主窗口；不可用或发布失败则创建主窗口。
后续刷新失败也保留可见窗口。托盘停用时选项按 false 生效并保留历史值，重新启用后恢复读取。应用级持久化、
状态泵、服务版本同步与内核自启仍在无窗口启动期间运行。

框架在 Windows/MSVC Release 下完成共享库、独立头文件与多窗口示例构建；公共 CTest
4/4、Windows 平台 CTest 和新增原生启动测试四种场景通过（24 assertions），模块导出检查
在 LF 规范化副本中通过。Clash-Flux 使用 CMake 4.4.2 重新配置并完整编译，
`./run.sh --version` 返回 0。配置启用 `CLASHFLUX_REQUIRE_PROJECT_TESTS=ON`，显式指定
Git for Windows 自带的 OpenSSL CLI，编译及测试环境使用 `CL=/utf-8`。
`ctest --test-dir build --output-on-failure --no-tests=error -L clashflux-required` 实际为
15/17 通过，包含生产分页、订阅持久化、CPS 重定位/版本拒绝/缓存完整性及 DNS/TLS 回归。
`windows_service` 因当前进程没有管理员权限而无法安装独占 SCM fixture；`vpn` 以
`0xc0000409` 异常退出，本轮未解决。两者的构建图和 PE 导入均不依赖 HuxerUI，不能据此
声称全部桌面回归通过。直接 ORM 测试按已记录的 MSVC C4737 例外未注册，persistence
保持必跑；测试和 CI 门禁均未禁用。Linux/macOS 原生及 Android/iOS 本次未在本机验证。

2026-10-22 固定版本更新到 `99fd7ba`，修复 Linux 托盘轻量模式退役窗口后仍由 Runtime 持有，
GTK 控件保留回调、输入法上下文未销毁，以及外部直接销毁 GTK 窗口时输入法委托释放顺序错误；
同时修复任意线程调用 `WindowHandle::Retire()` 与 Runtime 退出的竞争。新增 1,000 次窗口创建/退役、
旧句柄和所有者链回归，定向 ThreadSanitizer 检查不再报告该竞争；X11 与 Wayland 各 4 项 Linux 原生
退役回归均通过（27 assertions），HuxerUI 公共 CTest 4/4 通过。Clash-Flux Linux 构建、版本命令与
17 项必跑 CTest 全部通过。隔离 XDG 配置下的 Debug 程序实测，普通隐藏 PSS 约 136.9 MiB，轻量模式
三次隐藏约 95.5、90.2、76.4 MiB；进程共享库、图形驱动和分配器缓存会影响读数，这不是所有机器的
固定内存指标。Windows/macOS 原生行为未在本次 Linux 主机验证。

2026-10-21 固定版本更新到 `2a741e7`：相对 `7abe0fc` 新增应用范围的跨窗口拖放与
桌面触摸板连续翻页。独立维护仓库已分别 fetch fork 和上游；上游 `main` 仍为
`0c51262`，新提交均来自 fork，维护目录原有未提交内容保持原状。触摸板输入在
Linux/Windows/macOS 适配器进入框架滚动事务，Pager 只消费连续横向手势，内层
同轴滚动优先，离散滚轮不翻页。应用宽屏一级页改用 Pager；Compact 保留同一节点
和页面状态，但禁用一级横滑，以免规则、连接、日志绕过「更多」入口。二级分区
本来使用 Pager，更新后直接获得触摸板横滑。Linux Debug 本地重新构建
`clash-flux` 与全部测试目标，`./run.sh --version` 通过，17 项桌面必跑回归全部
通过；生产分区测试新增触摸板翻页与离散滚轮隔离检查。隔离 XDG 目录的 GUI
启动运行 8 秒未退出。本机未验证 Windows/macOS 原生触摸板与 Android 真机。

2026-10-18 固定版本更新到 `7abe0fc`（上游官方 main 自 fork 基线 `0c51262` 起无新提交；
本次是 fork 侧经 PR #6/#7/#8 合入 `farna/main` 的 28 个提交）：macOS 多窗口预览与
输入重入修复、Linux GTK 运行时多窗口移植、X11 可选链接在两套构建系统保留、
glass/backdrop 材质与 Android/Linux 加速、Windows 渐变与路径阴影资源复用、
Windows 原生自定义 chrome 检查、macOS 退出不再模态等待。公共 API 只增不减
（window/effects/paint/theme/app 新增多窗口与材质声明，`InternalAccess::InitializeWindow`
签名变更导致测试二进制需全量重链——只重编 `clash-flux` 目标会让旧测试可执行文件
对新 so 报 symbol lookup error）。`feat/multi-window` 已并入 `farna/main`，应用暂
不消费其 API。分支验证记录：Linux Debug 增量构建与 34 项 CTest（32 通过；
`HuxerUILinuxTests` 的 RTL caret 往返与 `HuxerUIRuntimeDependencyTests` 的重定位
运行时两项失败已在旧固定点 `42f0069` 临时 worktree 复现同签名，属本机预存环境
问题而非本次回归，本机不跑 Windows/iOS 原生）。Clash-Flux 已用新 checkout 全量
重编译，`./run.sh --version` 通过，17 项 `clashflux-required` 桌面回归全部通过
（含 page_transition 分页交互），并实际启动 GUI 确认首帧渲染无崩溃。维护仓库
更新前把进行中的工作区（多为已落地内容的落地前变体）快照到
`farna/worktree-snapshot-20261018` 分支后快进 `farna/main`，未做 rebase/force push。

2026-10-18 固定版本更新到 `42f0069`：安装器同版本检测（`InstallerProductState::SameVersion`——
重开同一版 Setup.exe 此前因 Burn 的 forward-compatible 回调一律置为 NewerVersion 而误报
"已安装新版本"，真正的旧版本升级反而从未被识别；现在按版本序比较，同版本进维护页、
更新版本仍拦截降级、旧版本并入 Present）与安装器检测/命令策略的 Burn 字符串探针修正；
iOS 链接解析重构为 `cmake/HuxerUIIosLink.cmake`（生成器表达式先求值再解析目标依赖，
保留库参数与传递链接选项，含 `tests/cmake/ios_link.cmake` 用例，替代本应用上一轮贡献的
闭包实现）。分支验证记录：Linux 增量构建、196 单元 + 979 运行时用例、5 个 iOS CLI 用例、
Ninja 单/多配置链接夹具；Windows 与 iOS 原生构建在该 Linux 主机不可用。Clash-Flux 已用
新 checkout 重新编译并通过 `./run.sh --version`，桌面 17 项回归通过；iOS 链接与 Windows
安装器行为随对应平台 CI 验证。`feat/multi-window`（窗口关系/多窗口）尚未并入 `farna/main`，
应用不消费。

2026-10-17 固定版本更新到 `8fd9d0e`：合入 PR #5 的 Windows 安装器命令驱动修复
（嵌入式 related-bundle 卸载此前被"已安装新版本"页面拦下并按成功退出 0，
bundle 注册跨版本累积——实测一个产品攒了 0.3.29-0.3.35 七个条目、每次升级每个
陈旧条目弹一页；现在命令式运行绕过交互闩锁、自行收尾并保留引擎停止与 bundle
注销），新增 iOS 链接闭包修复（`HuxerUIApp.cmake` 把 app 静态库目标自己的
LINK_LIBRARIES 传递闭包解析进 link.rsp，`$<TARGET_FILE:>` 交
`file(GENERATE)` 求值；`IOS.cmake` 补 `-framework CoreMedia`）。Clash-Flux 同步
把 iOS 模拟器 CI 改为单架构 arm64（C++20 std 模块 `-fmodule-output` 不支持
多 `-arch`）并让 PacketTunnel 不再消费 app 的 link.rsp。应用已用新 checkout
重新编译并通过 `./run.sh --version`，`clashflux-required` 桌面 17 项回归通过。
iOS 链接与 Windows 命令式安装/卸载行为随对应平台 CI 验证；真机安装/升级/卸载
仍需 Windows 实机复核。

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

2026-10-19：Lib-Charts 固定到 `5fcdc5a0ab9e99125652f7ffc8712267b61e2128`。
独立维护仓库先 fetch `origin/farna/main` 与 `upstream/main`，官方基线仍为
`cf7c286`，fork 在原固定点上增加一项圆角饼图提交，审查后快进维护分支。
公共接口新增可选 `PieChartStyle::rounded_caps`，原实心/普通圆环行为保留；
首页显式启用新样式，并移除应用覆盖的静态中心百分比。独立 Charts 构建与
`charts_tests` 通过（461 checks），Clash-Flux Linux HCG/目标构建、17 项
`clashflux-required` 桌面回归、CPS 重定位/版本拒绝/缓存完整性及 TLS 回归通过。
实际 GUI 检查了首页图表与宽窄切换。Android arm64-v8a native 目标编译通过；
这些检查不代替 Android 触摸/授权或 Windows/macOS 原生验收。

2026-10-19：Lib-Charts 固定到 `e90cfa84707bdf72eb6029410bf05c420efa62fc`，
加入全零数据特例：按传入条目数等分绘制，圆角饼图中心与悬停占比仍为 `0%`；
恢复非零数据后使用真实比例，空列表保留中性空状态。首页沿用现有上传/下载数据、
颜色、72pt 图表与卡片布局，通过升级固定依赖获得新行为。独立 Charts 构建与
`charts_tests` 已通过（519 checks），覆盖 1/2/3/5 项、悬停、全零/非零切换与空列表。
应用 Linux HCG/目标构建、`./run.sh --version` 与 17 项 `clashflux-required`
回归通过（含分页、CPS 重定位/版本拒绝/缓存完整性及 DNS/TLS）。隔离数据的真实
首页已验证中英文、320/390pt 窄屏及宽窄切换，全零时呈现上传/下载等分且中心 `0%`。
