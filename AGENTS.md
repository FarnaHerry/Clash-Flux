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
- 跨进程资源（detached 内核、root 服务托管的 pppd/openvpn、systemd 单元）由 pidfile /
  `pidAlive` / socket 协议管理，不属于本进程 RAII 的范畴。

## 文档同步

如果构建、运行、发布或开发流程发生变化，必须同步更新 `README.md`、本文件和相关
`docs/` 文档，保持命令与实际工程一致。
