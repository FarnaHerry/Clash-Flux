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

DNS 运行回归用 `test_singbox` + 固定打包内核，CTest 的 dns_hosts_runtime /
dns_policy_runtime / dns_tls_runtime 要求 Python，TLS 回归另需 OpenSSL CLI；缺少前提导致未注册不算通过。
证书拒绝场景同时检查 TLS certificate alert 与没有 DNS payload，不能拿任意超时
冒充证书验证。测试临时信任根只留在独占测试配置，不进入生产编译器或系统信任库。

桌面 CI 必须配置 `-DCLASHFLUX_REQUIRE_PROJECT_TESTS=ON`，按
`cmake/ProjectTestGate.cmake` 核对必跑测试已注册且未禁用，CTest 使用
`--no-tests=error`。当前为 15 项；只有已记录的 MSVC C4737 编译器缺陷允许
跳过直接 ORM 测试（14 项），persistence 必跑。Python、固定打包内核、
可执行的 OpenSSL CLI 缺失必须失败；下载回归传入明确的 OpenSSL 路径及
`--require-tls`，不能用跳过证书拒绝场景换取发布通过。手机构建不启用此桌面门禁。

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
- **iOS 当前按 TODO 暂缓，不承诺支持或发布**：iOS CI 用固定 HuxerUI revision 和 iOS Simulator SDK 编译 Clash-Flux app 与 Packet Tunnel
  extension，并从与 Android 相同的 sing-box revision 构建 iOS device/Simulator
  `Libbox.xcframework`。这仍是非阻塞编译检查：只生成未签名 Simulator 构建，不生成 IPA，
  也不属于 Release 门禁；除非项目未来明确恢复 iOS 支持，否则不要把它加入 Release 目标。签名设备包和真机 VPN 生命周期仍需另行验证。
- iOS 订阅与规则集下载使用 `platform/ios/App/ClashFluxBridge.mm` 的 `NSURLSession`，因为 iOS
  curl 构建不含 TLS。默认必须保留系统证书校验；只有订阅显式启用 `allowInvalidCert` 时，
  该请求的 URLSession delegate 才接受无效服务器证书。证书跳过逻辑只维护在 Clash-Flux iOS
  桥接层，不修改 HuxerUI 公共 HTTP API，也不得改动全局 URLSession 信任设置。
- iOS 设备与 Release 签名必须使用 Apple 开发团队签发并由匹配 provisioning profile 授权的身份；
  不得使用随机值或自签名证书伪造可安装包。Packet Tunnel 的 profile 必须授权
  `packet-tunnel-provider` entitlement。签名私钥和 profiles 不入库，管理流程见 `docs/ios-build.md`。
- iOS sing-box 必须由 Packet Tunnel extension 内的 Libbox 托管；不得调用 `posix_spawn`、
  `fork` 或 CLI 子进程路径。应用与 extension 间经 App Group 原子文件共享配置，extension
  不打开或写入应用 SQLite/CoreStore。
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
  背景 `@color/ic_launcher_background`）用**黑猫探出盒子的应用 Logo**；快捷开关磁贴徽章
  `drawable/ic_qs_clash_flux.xml` + `drawable-night/ic_qs_clash_flux.xml` 用**猫爪**
  （深浅色各一版）。应用 Logo 的透明源图为 `resources/images/clash_flux_logo.png`；
  改图标只改对应那一个，别把猫爪铺到启动图标上（也不要反过来）。
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
- 桌面 Medium/Expanded 外层使用平面布局：Logo 与导航为左栏，标题与内容为右栏，保留栏间竖线与右侧标题下横线；
  页内区块和条目使用卡片，保留底色、圆角与选中填色。一级页名称显示在自定义标题栏，
  内容区不重复标题或顶部动作行；页面动作挂在本页 `PageScaffold` 的标题栏中，位于原生窗口
  控件左侧，保留页面状态与回调作用域。Compact（包含桌面窄窗口）使用底部四项导航与页内标题；桌面窄窗口保留独立系统窗口标题栏，
  规则/连接/日志从设置「更多」进入并提供返回，覆盖底部导航。宽窄切换保留页面状态；
  平台能力仍由平台判断，不得根据视口改变订阅编排权限。
- 窗口外框、标题栏和一级侧栏用主题 `background`；页面骨架内容区统一用
  `IslandTheme::base`（`surface_container_low`），卡片沿用 `raised`。宽窄屏、一级和
  二级页都复用骨架层级色，不在各页硬编码背景。明度顺序固定为外框较亮、内容区较暗、
  卡片再次提亮，浅色和深色主题都遵守此顺序。
- 可排序卡片的拖动预览要持续跟随原始抓取点；滚动网格应在滚动视口注册拖放目标，
  使卡片间隙和视口边缘的拖动继续有效并触发边缘自动滚动。
- 手机端二级页面（详情、编辑或从一级页内部入口继续进入的页面）必须覆盖一级底部导航，
  并在标题栏左侧提供返回箭头。系统返回键、返回手势与该箭头必须调用同一个返回动作，
  回到直接父级而不是退出应用；此规则适用于所有当前和未来的手机平台，不限于 Android。
  二级页使用独立的页面进入/返回（push/pop）动画，不得与一级页切换动画共用同一套效果；
  进入时保留父页面状态（滚动位置、已填写内容），返回后原样恢复。
- **页内分区（二级标签）统一使用 `SectionTabPages` 的框架 Pager**：标签栏放在
  Pager 外，点击、菜单选择与跟手横滑写回同一个受控索引。不得再叠加
  PointerIntercept 抢手势、松手换页或逐页 Offset/Opacity 入场动画；嵌套横滑先给
  内层，首尾向外滑由框架滚动事务交给外层一级 Pager，取消或不足翻页距离时回弹。
  页面根使用稳定语义 Key、有界 Column + Grow；节点内容仍由 VirtualGrid/List
  懒加载，不能用无界 IndexedPages 全量构建节点。首次展示无入场运动，切回保留
  滚动位置，动画中反向改选不能崩溃。生产组件与交互测试见
  `section_tabs.cpp` / `tests/test_page_transition.cpp`。
  每对标签栏与内容页共用 `UseSectionTabMotion()` 的保留 handle，使下划线随
  Pager 的实际呈现进度延伸/收拢；进度不得逐帧写 State 或触发内容布局失效。
- **二级分页的水平留白必须归每页所有**：调用 `PageScaffold` / `SecondaryPageScaffold`
  时启用 `fullWidthSections`，骨架通过 `SectionTabContentInsets` 提供水平边距；标题与
  标签条各自留白，`SectionTabPages` 把留白加在每个稳定页根上。Pager 视口占满骨架
  可用宽度，不在外侧套水平 Padding；内容页每侧留白为网格卡片间距的一半，统一从
  `kSectionCardSpacing` 计算。滑动时相邻页面合起来正好是一份卡片间距，翻页步长仍为
  完整视口宽度。
  页根水平 Padding 由通用分页管理，卡片自身内边距仍由卡片决定。
- **Pager 反向切换补丁**维护在 `cmake/patches/huxerui-pager-retarget.patch`：固定
  HuxerUI 在跨多标签动画中切回起始页会复用旧 drag target，导致缺少布局 slot。
  补丁清除已结束的拖动目标，并保留回弹/反向轨道中的离场页。CMake 在加入源码前
  幂等应用，所有平台源码检出步骤也应用；升级 HuxerUI 时重新校验或移除上游已修复项。
- **隐藏虚拟页不得持续使可见布局失效**：`cmake/patches/huxerui-hidden-virtual-layout.patch`
  修复未参与布局的 VirtualGrid/List 将待测量 viewport 标记逐帧传到可见祖先的问题。
  隐藏子树仍保留测量失效状态，重新显示时正常更新；只有参与布局的路径传播失效。
  CMake 在加入源码前幂等应用，所有平台源码检出步骤同步应用。
- **二级标签栏必须随内容横滑居中目标项**：使用实际标签几何计算每项居中偏移，
  先限制到滚动边界，再按 Pager 呈现进度插值；拖动、松手收敛和取消回弹期间标签条
  都要跟随，不能等索引切换完成才揭示目标。选中变化、标签尺寸变化或窗口缩放后
  重新尽量居中；点击和菜单选择也适用。保持 ScrollView 的 Key 与
  容器结构稳定，不能因溢出菜单出现/消失而重挂载并丢失偏移。手动滚动浏览其它标签时
  不持续拉回当前选中项；生产组件与交互测试见 `section_tabs.cpp` / `test_page_transition.cpp`。
- 桌面关闭按钮只由 `tray.enabled` 控制：启用且系统托盘可用时隐藏到托盘，否则确认退出。
  不再读取旧 `tray.close_behavior`，退出确认框不提供最小化到托盘操作。
- **不要在界面里加「内核未运行 / 请到设置页启动内核」这类常驻提示**：内核启停由首页
  悬浮按钮表达，用户自己清楚当前状态；这类横幅只是噪音（代理页顶部那条已删除，
  以后不要再加回来）。仅保留真正需要用户处置的瞬时反馈（操作失败 toast 等）。
- 阻塞的内核、网络、路由和系统设置操作必须放到任务线程，不能阻塞 UI 线程。
- 订阅唤醒链接统一由 `src/profile_link.*` 解析：sing-box 的 `import-remote-profile`
  与 FlClash/Clash 的 `install-config`，仅百分号解码一次，只接受 HTTP(S) 订阅。
  桌面复用单实例 IPC，Android/macOS 复用 HuxerUI application activation；收到链接
  只预填独立导入表单，由用户确认，不直接切换活动订阅。粘贴/扫码与网页入口共享解析，
  平台协议注册和限制见 `docs/profile-links.md`；不得绕过平台订阅能力或默认证书校验。
- Windows 后台系统操作必须安静执行，不得闪出命令行窗口。优先调用 Win32 API；确实
  需要启动子进程时使用 `CreateProcessW` 的 `CREATE_NO_WINDOW` 并重定向标准句柄，
  不要在后台使用 `std::system`、`_popen` 或会显示终端的 shell 启动方式。只有明确
  需要用户交互的授权流程（例如 UAC）才显示系统提示；启动、轮询和退出清理中的类似
  操作也必须遵守此规则。
- Windows CI 使用 runner 预装的 OpenSSL（`Program Files/OpenSSL`），构建前检查
  可执行文件和开发头文件；不要在发布门禁重新调用 Chocolatey 安装 OpenSSL。
- 修改完成后运行 `git diff --check`，并在回复中说明实际执行过的验证命令及结果。
- 除非用户明确要求，不要提交、打标签、推送或发布版本。

## C++ 模块与内联性能

- 命名模块中的普通类内函数不隐式 `inline`；在 `.cppm` 导出类体中包含的 `.inc`
  同样遵循此规则。接口内的小访问器、状态判断与轻量转发函数，若需要供导入方展开，
  显式写 `inline` 并保持定义在接口定义域，不能只给声明加 inline 后把定义藏在 `.cpp`。
- 普通头文件/全局模块片段中的类内函数，以及 `constexpr`、`consteval`、首次声明即
  `= default` 的函数（含比较运算符）仍隐式 inline；纯字段 DTO 不需要补构造函数。
  不把“模块不隐式 inline”误推广到所有头文件、默认比较或模板。
- `inline` 不保证调用展开，也不允许在多个命名模块单元重复定义；实现单元内部可见的
  小函数仍可由优化器自动展开。不得批量给 I/O、解析、JSON 构建、锁与大对象复制函数
  加 inline，不把 HuxerUI composable 改成 inline，也不全局启用 `always_inline` 或
  有 ABI 影响的 `-fmodule-implicit-inline` 作为捷径。
- 性能检查先核对实际 Debug/Release 优化参数与 LTO，再比较导入真实模块的独立消费
  单元的优化汇编；调用消失与端到端提速分开记录。当前本地 Debug 只有 `-g`，不能
  用它推断 Release 的内联效果。字符串/容器复制、分配、锁和系统调用需独立评估。
  项目案例、11 处修正及验证范围见 `docs/cpp-modules-development.md`。

## 资源与生命周期约定

- `UseString()` / `UseEnvironment()` 等组合期读取不能放进点击回调或任务协程；
  文件选择器筛选名称、导入默认名称等先在组合期解析为拥有型字符串/DTO，再按值捕获。
  已发生过点击“选择文件”因协程内解析本地化资源而崩溃；文件选择失败应反馈并恢复 busy 状态。

- **一次性注册 API 不得直接写在 composable 函数体里**。凡语义为“每个 Runtime
  只能连接/注册一次”的框架 API（例如 `SystemTrayHandle::OnActivate`，重复调用抛
  `std::logic_error("... already connected")`），组合函数体在重组时会重复执行，
  未捕获异常会冒泡出 `LinuxUiWindow::Run` 并 abort（表现为“点任意开关就闪退”）。
  必须选其一：用 `Lifecycle` 包装、用 `std::call_once` 保证进程内只注册一次、或放进
  ApplicationHook。`window.OnCloseRequest`、`application.OnLifecycleChanged` 这类
  框架内部已做 Lifecycle 包装/多观察者处理的 API 不受此限。
- 订阅导入会提交应用数据，结果回传不得绑定易重建的表单/页面 TaskScope。URL 导入
  使用 `src/ui/profile_import_task.h` 的 `LaunchProfileImport`：应用级任务持有工作与
  回调，UI 线程统一先结束 busy 再反馈结果；异常转换为失败反馈，拒绝重复点击。
  选择器、相机和导航延时仍使用各自页面任务生命周期，不把所有任务改为后台常驻。
  回归需让导入发起视图在 await 中真正卸载，并验证成功/异常后 loading 结束及可重试。
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
  **订阅数据保护（发布阻断项）**：数据库记录与 profile 文件共同构成用户订阅，升级、启动、
  导入、刷新和候选配置失败路径都不得清空、重建或“修复性”删除它们。数据库迁移只在
  事务内复制全部订阅行并升级 schema，失败就让启动明确失败并保留原库；禁止删库重建、
  降级成内存空表或只验证一条示例记录。刷新/编辑先写独占临时文件并完成解析/候选检查，
  成功后再原子替换；失败保留旧文件、数据库引用和当前活动配置。清理文件必须先确认没有
  任何订阅行引用；只允许响应用户明确删除的那一个 ID，禁止启动时按“文件不存在/异常”
  批量删除元数据。Hydrate 未完成前不能将空缓存当成“没有订阅”写回。每次触及以上边界，
  回归必须至少含多个 profile、不同来源、稳定 ID、被引用文件和迁移后成功重开检查；CI 失败
  不能用删除用户数据库或订阅目录作为恢复步骤。
  异步 flush 必须按 key/ID 的写入版本确认，不能在 await 后无条件清脏（包括 ABA）；
  同类 flush 串行，失败保留待写版本。用户删除文件由 persistence 在删除事务提交后
  核对持久化与当前缓存引用，再在任务线程清理，仅使用 open 显式传入的文件根；
  未配置根、越界/无法确认安全的路径保留，不扫描补删。并发回归必须让生产 flush
  在真实 SQLite 写锁后挂起，再插入新动作并核对提交/重开；不要只测试顺序写入。
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

### 空状态占位

页面/分区/列表编辑区域无内容时统一用 `src/ui/empty_state.h` 的 `EmptyState(message, icon)`，
不要各页重复声明提示布局。图标尺寸、字号、颜色、居中、换行宽度与留白由该组件管理；
提示文字和页面操作入口由调用方保留。字段缺省值、表单说明和菜单禁用项不使用页级空状态。

### 应用内菜单

代理页溢出标签入口使用 `SectionTabPickerMode::ResponsiveGroups`：按响应式宽度（Compact 底部、
Medium/Expanded 右侧）展示分组抽屉，不按桌面/手机平台硬编码。窗口层服务由
`InstallSectionPickerLayers` 一次安装；选择、标签点击与横滑共用受控索引。抽屉保留保真度角标，
提供关闭、透明外部点击区域及取消/系统返回关闭，内容滚动不替换 Pager。实现见 `section_tab_picker.h`。

同类应用内弹出操作菜单统一使用 `src/ui/action_menu.h`：普通菜单通过
`UseActionMenu()` + `ActionMenuItem` / `ActionMenuSection` 声明；自定义 hover 多级菜单
共用 `ActionMenuItemView` / `ActionMenuSurface`，只自行管理子面板打开/关闭。
圆角、hover、面板裁剪、4pt 外围留白与 2pt 项间距都由通用控件维护，不要在页面复制一套。
删除/清空等危险操作用 `.Danger()` 或 View 的 danger 参数，统一使用主题 error 色。
事件回调只创建拥有型描述，不读取 UseTheme/UseEnvironment/UseString；面板通过 Scope 在
组合期读取样式。每个独立入口持有自己的 anchor。系统托盘原生菜单继续用框架原生 MenuItem，
不改成应用 Popup；选择器等框架控件继续使用框架自身实现。使用与验收见 `docs/ui-development.md`。

### 主题颜色

`Image.Tint` 仅支持矢量图，不能用于 PNG 等位图（运行时会抛异常并导致启动崩溃）。
位图按主题选用独立资源；涉及首帧图像或主题路径的修改，编译和 version 检查之外必须实际启动 GUI。

颜色值统一配置在 `src/ui/theme_colors.h`，页面只消费 ThemeSpec 角色或集中语义颜色函数；
阴影色使用 `ThemeShadowColor`，危险按钮继承当前 ButtonStyle 并使用 `OnErrorColor`。
不得在页面写 RGB 或固定黑白界面颜色；二维码的编码黑白模块及固定图标资源是明确例外。

### 首帧主题读取

主题是首帧唯一允许同步读取的持久化配置：`AppRoot` 在构建任何可见内容前通过
`persistence::readStartupTheme` 读取一次 `ui.theme_mode`，不要先画默认主题再异步切换。
该读取使用临时只读连接，不创建文件、不迁移 schema、不改 journal、不 hydrate 或修改订阅缓存；
读取失败明确终止启动并保留原库。运行期仍从 SettingsModel 读取，订阅与其它设置仍由原 ORM
启动任务 hydrate。同步读取 API 由 `cmake/patches/huxerui-lib-sqlite-startup-read.patch` 维护，
CMake 在所有平台加入 SQLite 依赖前幂等应用，本地源码与固定 revision 下载均适用；升级时校验。

### JSON codec 边界

应用运行时 JSON 与内部 policy 使用 `src/wire_codec.h/.cpp`（固定 Glaze 9.0.0）。
头文件只提供拥有型普通 C++ DTO 和 Result；Glaze 头、metadata 和模板只留在独立
`clashflux_wire` 普通 TU，不导出到 modules 或 UI codegen，也不合入 Android Legacy
超大 TU。页面从共享模型读 typed 数据，连接快照解码与投影放任务线程；首页总量
在 WebSocket 线程解码，UI 只读整数。持久键名用显式 metadata 固定。
运行时 API 可投影未知字段；订阅与原生配置不得照搬这个跳过策略。保留 yaml-cpp、
原生 JSON DOM、专用协议解析和局部规则文本编辑，迁移见 `docs/glaze-migration.md`。

## 架构分层与保真度契约（sing-box 内核）

本项目定位：**内核贴 sing-box、输入贴 Clash 生态、产品层用 Clash 的词汇只暴露内核真有的
能力**，并把 sing-box 独有能力产品化为 Clash 客户端给不了的差异点。完整契约、当前基线与
保真度账本形态见 `docs/singbox-layers-and-fidelity.md`（两者冲突时以该文档为准）。

**桌面最终形态为唯一主订阅 + 多次订阅按规则编排**，设计见
`docs/desktop-subscription-orchestration.md`；当前已接入桌面 Clash YAML 次来源按启用规则
参与的一份配置。原生 JSON 次来源、跨来源 detour 与完整崩溃恢复仍未实现。
固定顺序为前置动作 → UserOverride → SourcePolicy → 模式规则 → MainPolicy →
MainFallback；priority 只在同层比较，再按 order。无启用规则的普通次来源不载入，
目标不可用保留匹配并执行 Reject（默认）/UseMain（默认出口，不重跑主规则）/Direct。
对象身份为 source ID + kind + 原始唯一名称，改对象名即引用失效；不要宣称已有对象 UUID。
policy format_version=2，旧 v1 保留排序后迁移；拒绝未知字段和未来版本，SQLite schema 不变。
桌面候选先经内核 check 再停止旧实例，启动失败仅在旧原生资源仍有效时恢复；
CoreSnapshot.planRevision 表示成功应用后的运行序号，预览目录不能覆盖运行来源映射。
当前按用户意图转向 L3 产品完善，L1/L2 按使用反馈补齐且保持订阅/保真度门禁。
规则编辑器的 Clash 对象目录必须只读并独立于运行/预览映射，包含未参与来源；
异步结果按请求序号、来源身份与内容修订检查，切源清空旧选择，失效引用保留原名，
不得自动选择第一项或匹配其它来源同名对象。手机、原生连接/JSON 仍只开放默认出口。
官方 GUI 交互研究见 `docs/singbox-official-gui-review.md`。
**手机端不增加多订阅编排**：保留单活动代理订阅与现有原生辅助连接限制；UI、CLI、
导入和持久化写入口都必须遵守平台能力，不能用视口宽度开放桌面功能。

- **L1 内核层（紧贴）**：官方二进制（桌面 spawn）/ libbox（Android）同版本同源码 revision，
  各平台资产分别校验 SHA256；当前稳定基线 1.14.2（`af6e64c3b69e6132ebaee0e1a3d24e93903f6709`）。
  桌面缓存按版本和资产摘要隔离；Android 浅取正式 tag 后核对固定 revision，保留真实版本标签供上游 ReadTag；
  AAR 必须带构建脚本生成的 version/revision/ABI/SHA256
  元数据，缺失、版本不符或校验失败时重新构建，不得只凭旧 AAR 文件存在就继续打包。
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
- `interval` 必须 ≤ `idle_timeout`（缺省 30m）；编译器保留长间隔并同步延长空闲超时，
  该变化须记保真度账本。`tolerance: 0` 在内核中等于默认 50ms，不能宣称零容差。
  组的 `lazy` / `timeout` / `max-failed-times` / `expected-status` 无对应字段。
- sing-box clashapi 不返回 `selectable` 与组的 `testUrl`，`/providers/proxies` 是空壳：
  依赖这些字段的 Clash 面板能力一律视为「能打开但残缺」。

- `include-all-proxies` 只追加本 Clash 来源实际编译成功的顶层节点，按原名称字节序
  排序；显式成员在前，保留重复，不加入内置 DIRECT、组或其它来源节点。展开本身
  exact；被拒绝节点另记 Group/Approx，并保留 Node 明细。尚未转换的非空 filter /
  exclude-filter / exclude-type 或 use/include-all/providers 组合拒绝候选，不能扩大
  成员范围。显式空字符串筛选和 use 空数组可用；非法布尔、重复声明、残缺 proxies
  数组拒绝。空展开只在有有效显式成员时可用，不暗加 DIRECT；空组回退仍未接入。

- `dialer-proxy` 映射原生 `detour`；编译完成后检查全部出站依赖，包括组候选成员。
  缺失目标、编译后重复 tag 或循环必须报错，不能去掉代理链后直连。桌面 Clash 来源命名空间已接入，跨来源原始 dialer-proxy 仍不支持。
- 拨号字段按平台限制：bind_interface 只给桌面；非零 routing_mark 只给 Linux
  桌面，不能因 Android 有 `__linux__` 就开放。detour 忽略物理选项、MPTCP IPv6
  差异必须记 approx。DNS bootstrap/节点 resolver 与出站组共同做循环检查。
- Android PROCESS-NAME/正则使用 package_name/package_name_regex，不写桌面进程字段；
  UID 只映射 Linux 桌面的 user_id，不能把它当作 Android app UID。
- AND/OR/NOT 和 inline provider 转换子集必须原子完成；不删除失败子条件后扩大
  匹配，不把显式声明但转换失败的 provider 换成同名国内别名。外部下载仍是缺口。
- 匹配条件共用转换器，但 route 与 HeadlessRule schema 不同：UID/IP-VERSION
  不能进入 inline provider。重名 provider 整体拒绝；payload 失败带条目位置。
  第一个有效 MATCH 终止规则列表，显式 DIRECT 不改成首个 selector；不可用兜底
  目标编译失败。REJECT-DROP 使用原生 method: drop。
- 节点专用 DNS policy 只在可用 proxy-server-nameserver 存在时生效，不能插入
  普通 DNS rules；普通/节点策略共用从右向左的固定标签 > 整层 * > 前缀 . 优先级，
  并参与依赖图检查。* 只匹配一层，. 只匹配非根子域，+. 展开为独立根/子域分支；
  后声明只覆盖同一规范化分支，不能用全局「精确优先」或字符串长度代替 trie 次序。
  编译期节点选择用同一模式元数据，生成的锚定原生 regex 不经另一个 regex 引擎解释；
  元数据不写入内核 JSON。非法部分标签通配整条拒绝，不改成更宽 suffix。
- 加密 DNS URL 的 skip-cert-verify 只接受显式 true/false，映射单个端点的 tls.insecure；
  缺省保持证书验证，不能扩散到 bootstrap、其它端点或订阅下载。重复/非法参数与
  非加密传输上的证书参数整台服务器拒绝并记账；h3 参数仅用于 HTTPS。name-cert-verify
  只改变证书 DNSName 校验、不改变 SNI，不能拿 server_name 替代后冒充等价支持。
- Clash hosts 只映射精确域名到 IP/完整 IP 数组，规范化后重名全部拒绝；异常列表
  不能只保留合法前缀。未接入的通配、别名/lan、CIDR/带点分 IPv4 的 IPv6 等逐条
  unsupported，不扩大匹配；其它独立映射可继续转换。DNS 应答只限 A/AAAA，位于
  policy 前并把 TTL 改为 10 秒；`dns.use-hosts:false` / `dns.enable:false` 不关闭
  全局 hosts 的节点、DNS 端点及托管连接解析。显式 hosts 路径必须 `disable_cache:true`，
  防普通 DNS 旧结果覆盖映射；不改节点原 server/SNI，不引用订阅指定外部 hosts 文件。
  次来源只导出被出站引用的 hosts DNS 依赖，不导入主 DNS/连接规则。
  来源命名空间化后同步 Context 的 hostsDnsTag，后置生成的前置规则不能硬编码旧 tag；
  普通 resolve 必须排除已按 hosts 解析的域名，防再次解析覆盖显式地址。
  缓存和多地址拨号选择差异记 approx；系统 hosts 开关、通配/别名仍未保真。
- 未映射组字段、provider 引用/定义必须显式记账；`no-resolve` 暂记 approx，未知
  规则修饰符整条拒绝，不得截断附加字段后当作 exact。
- 全部 Clash 节点分支均检查字段白名单与传输层子字段；证书 `fingerprint`、未映射
  组合伪装等不能静默消失，须记 unsupported 并拒绝整条节点。显式 `udp: false`
  按原生 network 限制，HTTP 的 `udp: true` 记 approx；无效布尔值或 HY2 带宽拒绝，
  HTTP 多 path 只用首项须记 approx。
  Trojan/HY2/TUIC 默认启用协议 TLS；QUIC 不注入 TCP uTLS，SOCKS 不输出无原生
  等价的 TLS 字段。TUIC disable-sni 放到 TLS 对象，不能写成不存在的出站根字段。
- Hysteria v1 当前只转换 UDP 传输子集；auth 的 Base64 字节优先于 auth-str，
  保留原始凭据与 16 位认证长度限制，不能反用原生字段优先级。跳端口缺省 10 秒，
  显式低于内核 5 秒下限时拒绝，不套 HY2 的 30 秒/范围逻辑。接收窗口两侧显式
  非零才可 exact；默认窗口保留 Clash 上限并记录初始分配差异，单边窗口仍未接入。
  HY1/HY2 带宽单位区分 Mbps/MBps；字节转比特乘 8，不能大小写折叠或溢出后保留节点。
- 凭据必须按原文读取，不能用 trim 型 `ytext()`；OpenVPN inline auth-user-pass
  按两行读取，不能按空格拆词。ALPN/WS header/传输列表异常时整条拒绝并记账，
  不能筛掉异常成员。全局客户端指纹按来源继承、节点优先；非法全局值拒绝来源，
  未启用 TLS 的显式 TLS 字段拒绝节点。random 分布和内核指纹别名折叠必须记 approx。

- SSH 只转换显式非空 username、密码、内联 PEM 私钥/口令和主机公钥/算法列表；
  不读取订阅提供的外部私钥路径，不把空用户名变为内核默认 root。双认证的密码/私钥
  尝试顺序差异必须记 approx；主机算法列表按固定原生 SSH 库校验，异常成员整条拒绝。
  SSH 原生仅 TCP，不能输出不存在的 network/TLS 字段；udp:true 记 approx。

- 编译器只读取 GEO 缓存，不删除/下载文件；`CompileResult.ruleSetResources` 明确
  输出自有 GEO 来源，启动任务负责预取、按周刷新及坏缓存清理。禁止扫描原生 JSON
  的任意 tag 拼下载路径，也禁止清理不属于当前资源清单的 .srs。Android 打包 GEO
  不参与运行时更新或清理。SRS 文件头筛查不等于完整解析，完整校验仍由内核负责。
- file rule-provider 仅从显式 `ruleProviderDir`（应用传 `cfg::dataDir()`）读取相对路径，
  canonical 后检查仍在根内，禁止绝对路径/父目录跳转/越界符号链接/非普通文件；
  读取上限 8 MiB，YAML/text 整份转换，不写入或纳入 GEO 清理。文件失败必须让
  候选编译失败并保留账本/旧订阅，不能只跳过坏条目后提交。inline 快照缺少
  Mihomo 文件监听须记 approx；MRSv1 domain/ipcidr 已接入，classical/未来版本和文件管理 UI 仍未接入。
- HTTP provider 由编译器输出 `httpRuleProviders` 清单，编译器只读缓存/任务快照，
  不联网。任务层只支持 HTTP(S) YAML/text、直连或显式 DIRECT；header 仅支持
  非空单值数组的普通 ASCII 请求头（总量 16 KiB），跨平台传输保留字段、异常/重复
  成员拒绝整份声明。其它 proxy、MRS classical/未来版本及未映射字段仍拒绝。请求头值进入完整缓存身份，
  不进入原生配置或诊断；空 header 保留旧缓存身份。带头下载禁用重定向并记 approx，
  不向跳转目标转发凭据；iOS 仍暂缓且明确拒绝带头下载。下载默认验证证书，8 MiB 上限或更小 size-limit 在
  传输时拒绝，不截断提交；Android 复用 Java TLS 桥接，不用无 TLS curl。
  先独占暂存全部来源、整份转换与候选/目标检查，通过后才原子替换各自缓存；失败
  保留旧缓存/订阅，不删坏缓存或任意 path。自有 `core/rule-providers` 缓存核对完整
  来源身份/格式版本，文件名摘要不是安全校验；未来版本/身份冲突拒绝覆盖。
  path 只作数据目录内的只读种子，不写用户文件。运行计划持有 raw 快照，回滚不
  重新下载；interval 只在应用配置时检查到期、无定时热更新，须记 approx。
  缓存提交没有多文件崩溃事务，Android 仍没有桌面 CLI 预检查，不宣称实机回滚完成。
- ASN 只由编译器声明固定 ipverse JSON 资源，任务层经 HTTP 缓存准备；完整校验编号及
  双地址族 CIDR 后展开为原生 IP 条件并记 approx，不能输出伪造 ASN 字段或丢条件。
  HTTP classical 语法校验与依赖准备分开，最终完整候选通过前不能提交任何暂存缓存。
  MRSv1 domain/ipcidr 用固定 Zstandard 1.5.7 归档解码，SHA256 在 CMake 检查；输入
  8 MiB、解压 32 MiB 与集合数量均有界，未知版本/扩展、畸形结构和不支持域名整份拒绝。
  添加/升级该构建依赖要同步 `third_party/README.md`，所有平台链接同一静态目标。
- 通用下载使用独占临时目录与 RAII 清理；先关闭并检查写入，再执行调用方的
  `DownloadOptions.validate`，最后替换目标。Windows 覆盖不能先删旧文件。
  GEO 预取、普通订阅刷新与编辑接入候选校验；本地首次导入仍可先保存为未参与来源，
  启用时检查。Android 完整检查仍在后台 libbox，不得宣称有桌面 CLI 预检查。
- 编译失败仍保留已产生的 fidelity 明细，并发布到失败状态；不能因 JSON 为空
  把已经记账的 MATCH/依赖错误误判成没有保真度问题。

升级步骤、六平台资产摘要及 AAR 来源约定见 `docs/singbox-stable-upgrade.md`。

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

L1/L2/L3 的实际完成度与验收证据维护在 `docs/l1-l2-l3-status.md`；更新时注明
日期、代码快照、已接入/未完成边界和实际执行的检查，不把构建或配置检查写成实机验收。

发布说明维护在 `docs/releases/<标签>.md`（中英文）。`v*` 标签触发 CI，在发布门禁通过且产物收集完成后一次创建带安装包的 Release；存在对应文件时优先用它，缺省才使用自动生成说明。

如果构建、运行、发布或开发流程发生变化，必须同步更新 `README.md`、`README.en.md`、本文件和相关
`docs/` 文档，保持命令与实际工程一致。

代理分组抽屉不叠加遮罩颜色；右侧抽屉通过 `SectionTabPickerInsets` 从桌面标题栏分割线下方开始，标题栏不被抽屉覆盖。底部抽屉仅局部覆盖 `BottomSheetStyle.scrim` 为透明，保留其它模态组件的样式。
