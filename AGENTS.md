# Clash-Flux AI 开发规范

本文件是仓库级 AI/agent 开发指引。任何 AI agent 在阅读、修改或评审本项目代码前，
必须先读取本文件，并遵守以下规则。

## 修改后的强制验证

每次修改完成后，无论改动大小、涉及何种文件（代码、UI、脚本还是配置），都必须先
在本地重新编译，不得依赖旧的可执行文件判断改动有效：

```bash
cmake --build build --target clash-flux
./run.sh --version
```

需要验证 GUI 启动时，再执行 `./run.sh`。`run.sh` 默认只运行已经存在的
`build/clash-flux`，不会隐式替代编译步骤；如果使用其他构建目录，必须显式指定：

```bash
CLASHFLUX_BIN=/绝对路径/clash-flux ./run.sh --version
```

编译失败、可执行文件不存在或 `run.sh` 验证失败时，不得在回复中声称改动已完成。

## 代码与工作区约定

- 修改前先检查 `git status`，保留用户已有改动，不得擅自 reset、restore 或清理无关文件。
- 优先使用 Ninja/CMake 的增量构建；UI 修改必须经过 HuxerUI codegen 和目标构建。
- HuxerUI 及其依赖源码构建所需的本地修复应维护为 `cmake/patches/` 中的补丁；
  HuxerUI 补丁接入所有对应平台的源码检出步骤，依赖补丁在 CMake 加入依赖前应用。
  升级固定版本时先确认补丁仍可应用。
- 固定的 HuxerUI Lib-Charts 尚无饼图；Clash-Flux 的 `PieChartData` / `PieChart` 扩展由
  `cmake/patches/huxerui-lib-charts-pie-chart.patch` 维护，必须在 CMake 加入该依赖前应用；
  升级 Lib-Charts 时核对上游 API，并在饼图已上游实现后移除本地补丁。
- 同步 HuxerUI 时先 fetch 上游最新 revision，再逐项审查本地差异和维护补丁；只保留
  上游尚未修复的差异。每个保留补丁须针对新 revision 通过 `git apply --check --unidiff-zero`，并接入
  所有适用平台。上游已合并的修复应删除本地补丁及对应 CI 应用步骤，所有 CI 平台统一
  使用同一固定 SHA。
- iOS CI 用固定 HuxerUI revision 和 iOS Simulator SDK 编译 Clash-Flux 的
  `clash-flux_huxerui_ios_core` 静态目标（arm64）。它是非阻塞源码编译检查，不生成
  `.app`/IPA，也不属于 Release 门禁；当前配置不含 curl TLS 或 sing-box 子进程。
  完成可分发 iOS 应用壳、Network Extension 和 sing-box iOS 接入后，再评估 iOS 发布包。
- Android Gradle 构建会按固定 revision/SHA256 生成并打包国内 GEOIP/GEOSITE
  规则集；不得跳过 `stageBundledRuleSets` 或改为运行时下载。
- Android Gradle 的 HuxerUI Java 模块必须与 CMake 选中的 native HuxerUI 来自同一源码版本：
  有源码时使用该源码的 `platform/android/huxerui` 项目，只有 CMake 回落到 SDK 时才使用 SDK AAR。
  详见 `docs/android-build.md`。
- Android 发布 APK 必须同时启用 v1（JAR/META-INF）和 v2 签名，以兼容仍依赖传统签名文件的 OEM 安装器；
  Android CI 必须使用 `apksigner verify --verbose --min-sdk-version 23` 分别断言 v1、v2 均为 `true`；
  应用 minSdk 为 24 时，默认核验会跳过 v1，不能仅检查默认命令成功。
  详见 `docs/android-build.md`。
- **Android 图标分工是固定的，不要互相替换**：应用图标（`mipmap-*/ic_launcher.png` 与
  adaptive 前景 `drawable/ic_launcher_foreground.xml`，前景 inset `@drawable/ic_launcher_mascot`、
  背景 `@color/ic_launcher_background`）用**平滑猫头吉祥物**；快捷开关磁贴徽章
  `drawable/ic_qs_clash_flux.xml` + `drawable-night/ic_qs_clash_flux.xml` 用**猫爪**
  （深浅色各一版）。改图标只改对应那一个，别把猫爪铺到启动图标上（也不要反过来）。
- Android 常驻服务不得等待 `POST_NOTIFICATIONS` 才启动前台服务或继续 VPN 授权；该权限只影响通知栏展示。
  返回 `START_STICKY` 的服务必须处理空 Intent，并从持久化状态恢复运行模式，不能猜测为 TUN。
- Android sing-box 所有者固定在 `:background`：`ClashVpnService` 持有 libbox/VpnService，
  `RuntimeControlService` 提供 AIDL/Binder 控制与状态查询。UI 进程通过 JNI 接收后台快照，
  不得再依赖跨进程不可见的 Java 静态字段。
- Android SQLite/CoreStore 数据库只由默认 UI 进程打开和写入；后台进程只消费 UI 原子提交的
  `clash-flux/core/config.json`，大体积出站/连接快照使用原子文件共享。不要在后台服务进程调用
  会写配置数据库的 CoreStore 路径，也不要用 SharedPreferences 跨进程同步运行状态。
- Android `ClashVpnService.onCreate()` 必须先同步 `startForeground()`，JNI/libbox 初始化和
  数据面启动放在服务工作线程；网络变化、亮屏/退出 Doze 时先更新 sing-box 物理接口，再关闭旧连接。
  详见 `docs/android-background-lifecycle.md`。
- HuxerUI composable 函数体内不能使用条件编译；普通 UI 源文件按项目现有 DSL 约定编写。
- 可排序卡片的拖动预览要持续跟随原始抓取点；滚动网格应在滚动视口注册拖放目标，
  使卡片间隙和视口边缘的拖动继续有效并触发边缘自动滚动。
- 手机端二级页面（详情、编辑或从一级页内部入口继续进入的页面）必须覆盖一级底部导航，
  并在标题栏左侧提供返回箭头。系统返回键、返回手势与该箭头必须调用同一个返回动作，
  回到直接父级而不是退出应用；此规则适用于所有当前和未来的手机平台，不限于 Android。
  二级页使用独立的页面进入/返回（push/pop）动画，不得与一级页切换动画共用同一套效果；
  进入时保留父页面状态（滚动位置、已填写内容），返回后原样恢复。
- **页内分区（二级标签）左右滑动必须挂在内容滚动节点上，且认领距离不晚于 6pt**：
  框架手势识别按「由深到浅」注册、Move 时取第一个 Accept 的识别器，而 Pager 的整页
  拖动在横向占优且位移 ≥ `touch_gesture_slop`(6pt) 时就 Accept——认领阈值一旦大于它，
  横滑必然先被外层 Pager 抢走（真机表现：手机端代理页一滑就整页翻走，页内分组切不动）。
  阈值与判定在 `src/ui/section_swipe.h`，单测 `tests/test_section_swipe.cpp`；两端没有
  相邻分区时不要认领，把手势让回外层 Pager（页内先翻、翻到头再整页翻）。
- **只建当前分区内容的页面（代理页）换页动画必须靠「页 Key 随分区变化 + 挂载后把
  本地进度推进到 1」**：`AnimateTo` 只在目标值变化时才有动画，新挂载的节点会直接落到
  目标值上，所以照搬订阅页那套 `AnimateTo(selected ? 1 : 0)` 等于没有动画（订阅页能动
  是因为 `IndexedPages` 把每页都留在树上）。点击、菜单选择与内容横滑共用同一个选组动作，
  依标签先后决定左右入场方向；首次展示不加无方向动画。实现见 `section_tabs.cpp` 的
  `ProxyGroupPage`，验证见 `tests/test_page_transition.cpp`（直接编译生产组件，在无窗口
  Runtime + 虚拟时间中断言左右 48pt 偏移收敛、快速反向切换与 reduced motion）。
- **二级标签栏必须自动揭示选中项**：选中变化、标签尺寸变化或窗口缩放后，使用实际
  布局几何滚到选中标签完整可见；内容横滑与菜单选择也适用。保持 ScrollView 的 Key 与
  容器结构稳定，不能因溢出菜单出现/消失而重挂载并丢失偏移。手动滚动浏览其它标签时
  不持续拉回当前选中项；生产组件与交互测试见 `section_tabs.cpp` / `test_page_transition.cpp`。
- **不要在界面里加「内核未运行 / 请到设置页启动内核」这类常驻提示**：内核启停由首页
  悬浮按钮表达，用户自己清楚当前状态；这类横幅只是噪音（代理页顶部那条已删除，
  以后不要再加回来）。仅保留真正需要用户处置的瞬时反馈（操作失败 toast 等）。
- 阻塞的内核、网络、路由和系统设置操作必须放到任务线程，不能阻塞 UI 线程。
- Windows 后台系统操作必须安静执行，不得闪出命令行窗口。优先调用 Win32 API；确实
  需要启动子进程时使用 `CreateProcessW` 的 `CREATE_NO_WINDOW` 并重定向标准句柄，
  不要在后台使用 `std::system`、`_popen` 或会显示终端的 shell 启动方式。只有明确
  需要用户交互的授权流程（例如 UAC）才显示系统提示；启动、轮询和退出清理中的类似
  操作也必须遵守此规则。
- 修改完成后运行 `git diff --check`，并在回复中说明实际执行过的验证命令及结果。
- 除非用户明确要求，不要提交、打标签、推送或发布版本。

## 资源与生命周期约定

- **一次性注册 API 不得直接写在 composable 函数体里**。凡语义为“每个 Runtime
  只能连接/注册一次”的框架 API（例如 `SystemTrayHandle::OnActivate`，重复调用抛
  `std::logic_error("... already connected")`），组合函数体在重组时会重复执行，
  未捕获异常会冒泡出 `LinuxUiWindow::Run` 并 abort（表现为“点任意开关就闪退”）。
  必须选其一：用 `Lifecycle` 包装、用 `std::call_once` 保证进程内只注册一次、或放进
  ApplicationHook。`window.OnCloseRequest`、`application.OnLifecycleChanged` 这类
  框架内部已做 Lifecycle 包装/多观察者处理的 API 不受此限。
- **本进程拥有的 OS 资源用 RAII 包装，不在多个返回分支手写释放**：服务 IPC 的 fd 用
  `src/service.cppm` 的 `UniqueFd`，Win32 HANDLE / HKEY 用 `src/win32_raii.h` 的
  `clashflux::win32::UniqueHandle` / `UniqueHkey`，curl easy handle 与 header list 用
  `src/api.cpp` 的 `CurlHandle` / `CurlHeaderList`。只有需要检查释放返回值，或所有权
  属于其它进程/进程级单例时才手工管理，并在注释里写明理由。
- **持久化边界**：settings/profiles 走 `clashflux.persistence`（`huxerui::sqlite`
  ORM）的**内存缓存 + 异步落库**，schema 在 `src/sqlite_schema.*`；**老库
  （user_version=0）由 0→1 迁移在事务内重建表并保留全部数据，应用不删任何
  文件**——schema 再变就升版本并追加 Migration 链，不得退回「不兼容就静默
  失败、降级成内存缓存」的做法（重启丢数据是已发生过的线上事故）。
  `clashflux.db`
  只是保持既有同步接口的转发门面，新代码直接依赖 `clashflux.persistence`。**
  日志不进库**（高频追加会与写事务抢锁/IO），仍直接写 `core/*.log`。需要数据库
  的 CLI 命令必须在应用运行时内执行（`clashflux.cli` 的伪 CLI：`setPendingCommand`
  → 启动任务 `cli::run` → flush → 退出），不要在运行时之外直接读写持久化层。
- **单实例**：GUI 与 CLI 都先抢 `clashflux::instance::acquireOrActivate()`。owner
  在自己的运行时里执行命令（CLI 命令隐藏窗口到托盘），非 owner 的 CLI 通过
  `clashflux::cli_ipc::tryForwardCommand()` 把命令转发给 owner 执行并回传输出与
  退出码（owner 在启动泵里 `servePendingCommands()`）；任何情况下都不得出现第二个
  Runtime / 托盘 / 持久化缓存。`version`/`help`/`service` 子命令例外：它们由
  `cli::isDirectCommand` 直发、不启动运行时——`service run` 是 systemd 拉起的
  root 守护进程，环境里没有显示服务，初始化 GTK 会直接 abort。
- 跨进程资源（detached 内核、root 服务托管的 pppd/openvpn、systemd 单元）由 pidfile /
  `pidAlive` / socket 协议管理，不属于本进程 RAII 的范畴。

### HuxerUI 应用级模型（application service）

跨页面共享数据统一走 `application_hooks` 安装的 application service：`context.Provide`
模型、组件用 `UseService<T>()` 取用，模型内持 `State`/`StateList`；由 `src/ui/common.cpp`
里**唯一的数据泵**在 UI 线程写，页面在组合期读（读即订阅，内容相等时 `State::Write`
去重）。页面不要再为共享数据挂定时器；要把模型值镜像进页面本地 `State` 时，用
`Lifecycle(setup, 模型State...)` 把模型 State 作为依赖，让变化驱动镜像。

**应用设置必须从 `SettingsModel` 读，不要在组合期直接 `setting(...)`**：persistence
缓存在首帧之后才 hydrate，直接读只会拿到默认值且之后无人再同步——真机实测表现为
「关闭窗口时」一直高亮"每次询问"、保存过的首页自定义布局在启动时丢失、环境 shell
选择回落到探测值。需要"hydrate 完成后补一次"的消费者（首页布局）以 `SettingsView::ready`
为信号补读。

以下五条都是真机/桌面实测踩出来的硬规则：

- **composable 的返回值必须挂载**。hcg 把 composable 体包成 `huxerui::Scope` 工厂，
  工厂只在对应 View 被挂载时执行；当裸语句调用并丢弃返回值，等于整个函数体不执行，
  里面的 `Lifecycle` 永不注册（表现：首页数据永远是默认值、整块空白）。副作用型
  composable 也要把返回值放进视图树，与 `CLASHFLUX_APPLICATION_EFFECTS`、
  `CLASHFLUX_PROFILE_REFRESH_PUMP` 一致。
- **应用级模型里的 `State` 成员必须带初值**（`huxerui::State<T> x{T{}}`）。huxerui 的
  `State() = default` **不创建 cell**；空 cell 的读取、写入，以及作为 `Lifecycle`
  依赖，都会抛 `HuxerUI Lifecycle dependency State is empty`——启动即 abort。
- **框架容器要求非空集合**（如 `IndexedPages` 至少一页）。首帧数据可能为空，必须先
  构造兜底视图再判断，不能等构造之后才判空——框架抛 `std::invalid_argument` 会直接
  终止进程。
- **模型驱动的“条件拉取”必须覆盖 hydrate 完成**。只靠写入口自增的修订号，会让首帧
  （hydrate 之前）读到空表的消费者永久停在空表；把修订号定义成“内容版本”（`open()`
  完成 hydrate 也 +1），并让显式 `RequestSync()` 同时强制拉取一次。
- **不可见的一级页必须“只留状态、不建内容”**。`Pager`（移动端四个一级页）与
  `IndexedPages`（桌面七个一级页）都会把**所有**页同帧挂载：隐藏页即使自己不重组，
  其已挂载子树也会跟着每一次渲染被重新测量。OnePlus 真机实测（代理页大分组）：
  四页同挂时每帧 1443 次测量请求 / MeasureStage ≈20ms、gfxinfo p50 30ms、卡顿率
  77%；只让当前页构建内容后降到 28 次 / ≈0ms、p50 8ms、卡顿率 1.5%（桌面
  measure/帧 4304 → 40）。做法：页面加 `bool active` 参数，**在全部 hook（State /
  Lifecycle / UseXxx）之后**插入
  `if (!active) return huxerui::View{huxerui::Row{}}.Key("...-idle");`
  ——页面仍被挂载，自身 State 与 Lifecycle 保留，只是不再构建重子树。已知代价：
  被门控的页面重新可见时，其内部 ScrollController 从顶部开始（huxerui 的
  ScrollConnection 重新连接时不恢复偏移），要保留滚动位置需另做处理。

- **用户动作要写透模型，不能“改完本地 State 再等泵”**。`RequestSync()/RequestRefresh()`
  只把修订号 +1，数据泵睡在 `Delay(1s/2s)` 里时**唤不醒**（tick 只在读完之后才被检查），
  于是控件会滞后一整拍，表现成“点了没反应 / 又跳回去”。约定：
  1. KV 设置（`tray.*`、`app.*`、`core.allow_lan`、`core.ipv6_enabled` 等）：`setSetting`
     是同步写内存缓存，紧接着用模型的 `Update()` 发布同一个值（UI 真值 = 缓存真值，同一
     时刻），既不需要 pending，也不需要乐观回落。
  2. 可能失败的慢操作（TUN / 系统代理 / 内核启停 / 出站模式 / 订阅激活）：动作完成时
     （协程已经回到 UI 线程）成功就用 `Update()` 写权威值——**不要重写本地 State**，
     避免二次设置与闪烁；失败才回落，且回落前做**目标值校验**：当前显示值仍等于本次
     target 才回退，否则说明用户已经点到别处、有更新的意图，不许覆盖。
  3. 被写透的字段，其权威来源必须与写入目标一致（例：局域网开关显示 `CoreView::allowLan`
     ——KV 意图，而不是内核运行时回读的 `CoreSnapshot::allowLan`），否则下一个泵节拍会把
     写透的值打回去。

## 架构分层与保真度契约（sing-box 内核）

本项目定位：**内核贴 sing-box、输入贴 Clash 生态、产品层用 Clash 的词汇只暴露内核真有的
能力**，并把 sing-box 独有能力产品化为 Clash 客户端给不了的差异点。完整契约、当前基线与
保真度账本形态见 `docs/singbox-layers-and-fidelity.md`（两者冲突时以该文档为准）。

- **L1 内核层（紧贴）**：官方二进制（桌面 spawn）/ libbox（Android）同版本同 SHA256；
  不 fork、不臆造字段（sing-box 对未知字段直接拒绝启动），能力一律以上游文档/源码为准；
  控制面走内核自带的 `clash_api`，内核没有的概念不在这一层做兼容包装。
- **L2 翻译层（处理 Clash 生态）**：Clash YAML、sing-box 原生 JSON、原生连接（PPTP /
  OpenVPN）都经 `clashflux.singbox` 编译。每条映射（协议 / 组 / 规则 / 字段）必须归入
  **exact / approx / unsupported** 之一并进入保真度账本，**禁止静默丢弃**，也禁止为
  「能跑起来」改写用户语义（替换测速 URL、关掉证书校验等）。
- **L3 产品层（Clash 词汇 + sing-box 能力）**：词汇与信息架构跟 Clash 生态，但只暴露
  内核真有的能力；做不到的不做假 UI，sing-box 独有能力优先主动暴露。

**决策流程**（新增或修改任何映射时）：① sing-box 有原生等价 → exact；② 语义不同但可
表达 → approx + 账本条目；③ sing-box 没有 → 默认降级 + 账本条目，只有「体验损失大且实现
可控」才允许应用层补齐，且必须自证内核在跑/未跑两态都有定义、失败可回滚、不引入第二个
真相来源，否则不做；④ **任何情况下不得在 UI 假装支持**。

**已知边界（不要当成 bug 去修）**：

- `urltest` 不支持手动锁定：`PUT /proxies/{name}` 只接受 `Selector`（400
  `Must be a Selector`），也没有 `fixed` 字段；`fallback` / `load-balance` 在 sing-box
  无对应语义，编译为 `urltest` 并记降级。
- `interval` 必须 ≤ `idle_timeout`（缺省 30m），否则内核启动失败；组的 `lazy` / `timeout` /
  `max-failed-times` / `expected-status` 无对应字段。
- sing-box clashapi 不返回 `selectable` 与组的 `testUrl`，`/providers/proxies` 是空壳：
  依赖这些字段的 Clash 面板能力一律视为「能打开但残缺」。

**维护**：改编译器映射或升级 sing-box 时必须同步 `docs/singbox-layers-and-fidelity.md`
的保真度基线表。判定标准：**用户订阅里的条目消失或语义改变 → 用
`ctx.note(scope, level, subject, detail, action)` 进账本；运行期事件、语义不变 → 留在
`ctx.warn()`**（如规则集缓存不可用改走在线拉取、托管 TUN 关掉 auto_redirect）。账本为
`CompileResult.fidelity` → `CoreSnapshot.fidelity`，`warnings` 只是它的自由文本投影；
账本的消费点：设置页「配置保真度」完整明细、`singbox::FidelitySummary()` 折成的一行
toast、代理页分组标签的 `!` 角标（`SectionTab.badge`）、CLI `profile check`。
**toast 只在用户动作后发**（导入 / 刷新 / 启用订阅），内核重启（启停、TUN 或模式切换
触发的重编译）不得重复提示。

## 文档同步

如果构建、运行、发布或开发流程发生变化，必须同步更新 `README.md`、本文件和相关
`docs/` 文档，保持命令与实际工程一致。
