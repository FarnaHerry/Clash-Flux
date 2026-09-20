# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Clash-Flux 是一个 **C++23 模块化 GUI 代理客户端**：复刻 Clash Verge Rev 的核心体验，
用 **HuxerUI**（组件式声明 UI）做桌面壳，内核是 **sing-box**（桌面 spawn 进程、
Android 进程内 libbox），经其 clash_api（REST + WebSocket 推送流）交互，全程
C++。订阅仍是 Clash YAML，由自带的 `clashflux.singbox` 编译器合成 sing-box
JSON。构建系统 CMake（脚手架与姊妹项目 `../apitab` 同源）。分层：UI（src/ui/*.cpp
普通源走 hcg codegen）/ 领域 store（clashflux.store.* 模块）/ 内核接入层
（core/singbox/api/stream）。

## HuxerUI 开发参考

UI 工作先读 skill：`.claude/skills/huxerui-app-development/SKILL.md`（references/
含 dsl-style、components、fundamentals、layout-and-ui 等分册）。要点：

- HuxerUI 接入“SDK 工程契约驱动、源码解析优先”：开发构建优先
  `third_party/huxerui` 源码（git clone 上游，`add_subdirectory` 编译，不入库）；
  缺 GTK ≥4.14 / libepoxy ≥1.5 / libsoup ≥3.0 开发包时自动回落已安装 SDK
  （`HUXERUI_HOME`）或 `third_party/tarballs` 的 Linux 0.3.0 离线包。强制 SDK：
  `-DCLASHFLUX_HUXERUI_FORCE_SDK=ON`。本机（Fedora）缺 libepoxy-devel，当前走
  已安装 SDK 通道。
- 源码通道需用当前源码本地构建的宿主工具（`third_party/huxerui-tools/
  linux/x86_64/{hcg,hrc}`，从 apitab 拷贝；上游 linux 预置可能落后于源码，
  pull 上游后要重编，方法见 apitab CLAUDE.md）。
- UI 层是**普通 .cpp**（不要 .cppm：codegen 只扫 .cpp/.cc/.cxx）；composable
  函数不加 `inline`；入口根在 src/app.cpp + src/ui/app.cpp。
- **composable 函数体内不能有条件编译**（hcg 不支持 #ifdef 穿行）——版本号等
  宏在文件作用域先展开成常量。
- `nlohmann::json` 经 `import nlohmann.json` 模块：**不要用 `.items()` 结构化
  绑定**（迭代代理的 `get<>` 不在导出集里，模块下编译失败），用迭代器
  `it.key()`/`it.value()`。
- 存进 `State<T>` 的类型需要 `operator==`（`= default` 即可），否则 State
  写回时编译失败。

## 构建 / 运行

```bash
cmake -B build -G Ninja            # 配置（默认 Release；调试加 -DCMAKE_BUILD_TYPE=Debug）
cmake --build build -j             # 编译
ctest --test-dir build             # 冒烟测试（test_smoke）
./run.sh                           # 启动 GUI（INTEL_FORCE_PROBE=1）
huxerui run linux                  # HuxerUI CLI 流程（构建到 .huxerui/build/linux/）
```

### 每次改动后的本地验证（强制）

任何需求实现、修复、重构或 UI 调整完成后，agent 必须先重新编译本地目标，
再验证 `./run.sh` 启动的是刚编译的可执行文件。默认流程为：

```bash
cmake --build build --target clash-flux
./run.sh
```

`run.sh` 默认使用 `build/clash-flux`，不会替代编译步骤；若使用 `build-ubsan`
或其他目录，必须通过 `CLASHFLUX_BIN` 指向该目录的可执行文件后再运行验证。
没有完成本地编译和运行验证时，不得在回复中声称改动已完成。

- 工具链：系统 GCC（本机 16.2.1）+ libstdc++，CMake ≥ 4.4（`import std` 仍是
  experimental：UUID 表在 `cmake/CxxImportStdGate.cmake`）。
- **依赖全部 vendor 在 `third_party/`**（tarball + SHA256，configure 期解包到
  `build/vendor/`，清单见 `third_party/README.md`）：HuxerUI 0.3.0、curl 8.22.0
  （REST + 订阅下载）、IXWebSocket 12.0.1（client-only，内核推送流）、
  SQLiteCpp 3.3.3（订阅/设置）、nlohmann::json 3.12.0、yaml-cpp 0.8.0（订阅
  YAML 解析）、OpenSSL 3.5.1（linux x86_64 兜底静态包）。
- **sing-box 由项目自带（桌面）**：configure 期下载官方 release（1.14.0，
  SHA256 钉死，资产表见 `cmake/singbox_bundle.cmake`，与 Android libbox 同
  版本），POST_BUILD 拷到 `<exe>/engines/sing-box`；
  `-DCLASHFLUX_BUNDLE_SINGBOX=OFF` 关闭。运行时解析优先级
  `<exe>/engines/sing-box → <exe>/sing-box → <repo>/engines/sing-box（开发
  形态）→ PATH`（`src/config.cppm::cfg::singboxBinary()`）。`engines/` 已
  gitignore。**内核发布什么桌面平台/arch 就钉什么**：linux/windows ×
  x86_64/arm64 + darwin x86_64/arm64 六组合（riscv64/loong64/freebsd 等
  壳编不到的目标不入表）。

## 架构

| 模块 | 文件 | 职责 |
|------|------|------|
| `clashflux.config` | `src/config.cppm` | 数据目录（~/.local/share/clash-flux）/ sing-box 二进制解析 / 控制器端点（127.0.0.1:29097）/ secret 生成 / 深色检测 |
| `clashflux.instance` | `src/app_instance.cppm` | 桌面 GUI 单实例锁与二次启动唤醒（Linux/macOS 文件锁+信号，Windows Mutex+Event；CLI 不拦截） |
| `clashflux.utils` | `src/utils.cppm` | 纯 string/number 帮助函数 + percentEncode / appendQuery |
| `clashflux.db` | `src/db.cppm/.cpp` | SQLiteCpp：profiles（订阅）/ settings（KV）两表 |
| `clashflux.api` | `src/api.cppm/.cpp` | sing-box clash_api REST 客户端（curl，同步阻塞、每调用独立 handle）：version/configs/patchConfigs(mode)/proxies/selectProxy/proxyDelay/groupDelay(fan-out 并发逐节点)/connections/订阅下载 |
| `clashflux.core` | `src/core.cppm/.cpp` | sing-box 子进程生命周期（`run -c <config.json> -D <workdir>`；posix_spawn + 监视线程；SIGTERM→2s→SIGKILL）+ generateConfig（委托 clashflux.singbox 编译） |
| `clashflux.singbox` | `src/singbox.cppm/.cpp` | Clash YAML → sing-box JSON 编译器（yaml-cpp 解析 + nlohmann 合成）：节点（ss/vmess/vless/trojan/hysteria2/tuic/http/socks）、组（fallback/load-balance 降级 urltest）、基础规则 + GEOIP→远程 .srs、clash_mode 三模式前置规则；不支持的条目进 warnings 不阻断 |
| `clashflux.stream` | `src/stream.cppm/.cpp` | /logs /traffic /connections 三条 WS 流（IX 自管线程，事件入槽，UI PollWhile 泵取） |
| `clashflux.sysproxy` | `src/sysproxy.cppm` | Linux 系统代理写入：KDE kioslaverc（kwriteconfig6/5 + dbus 通知 KIO）/ GNOME gsettings；阻塞 shell 调用，UI 必须 RunOnTaskThread |
| `clashflux.service` | `src/service.cppm` | 统一特权服务：root systemd 单元 `clash-flux.service` + `/run/clash-flux/service.sock` 行协议（sing-box START/STOP/STATUS/VERSION + PPTP/OpenVPN AVAILABLE/START/ROUTES/STOP；START 只传 config.json 绝对路径）；由同一 daemon 管理 Linux pppd/openvpn/ip；install/uninstall 需 root（GUI 经 pkexec 重入本二进制 `service install`），socket 仅安装用户 UID + root 可访问 |
| `clashflux.cli` | `src/cli.cppm` | 完整 CLI：`cli::run(args)`，子命令 version/service/core/mode/tun/proxy/profile/help；platform/*/main.cpp 无参 → GUI、有参 → CLI |
| `clashflux.store.core` | `src/store/core_store.cppm` | 编排单例 `coreStore()`：持有 Db/ClashApi/CoreProcess/CoreStreams；startCore/stopCore/applyMode/refreshRuntime/checkAlive；settings KV；TUN 切换 = 重新编译 config.json + 自动重启内核；内核三形态托管：**服务托管 → 接管外部实例（/version 探测 + core.pid pidfile）→ 直接 spawn** |
| `clashflux.store.profiles` | `src/store/profiles.cppm` | 订阅单例 `profilesStore()`：importUrl/importFile/refresh/activate/remove（activate/remove 触发内核重启） |
| `clashflux.openvpn` | `src/openvpn.cppm/.cpp` | OpenVPN CLI 配置校验、Linux root/Windows CLI 会话、tun 接口与内网 CIDR 路由；托管模式禁止配置自带 route/up/down 脚本 |
| `clashflux.store.vpn` | `src/store/vpn.cppm` | PPTP/OpenVPN 连接生命周期 + 全局 `VpnPolicy` 持久化；`ProfileConnectionId` 是跨引擎稳定连接引用，原生连接建连时将 IPv4 全局规则交给对应隧道接口 |
| `clashflux.ui.*`（普通 C++） | `src/ui/*.cpp` | app（通用壳 + `platform_app.cpp` 平台应用壳）/ common（岛屿原语 IslandSurface/DialogCard/页面骨架/卡片/状态胶囊）/ home/profiles/proxies/rules/connections/logs/settings 七页（平台设置在 `platform_settings.cpp`）/ task_bridge.h（协程桥） |
| `src/app.cpp` | 普通 TU | `Application{AppRoot, AppOptions}`（Custom chrome，标题栏 24pt） |
| 平台入口 | `platform/{linux,macos,windows}/main.cpp` | 无参 → 获取单实例后 `huxerui::RunApplication()`；有参 → `cli::run`（同一二进制即 CLI） |

## 多平台 / CI

- 平台细节由 `.github/workflows/build.yml` 承担（矩阵命名对齐 apitab：
  `build-<os>-<arch>`）：build-linux-x86_64（ubuntu:24.04 容器 + apt.llvm.org
  clang-21/libc++-21 + pip cmake 4.4.2）正式；linux-arm64（ubuntu-24.04-arm 原生）/
  windows-x86_64（MSVC + choco OpenSSL）/ windows-arm64（windows-11-arm +
  vcpkg OpenSSL）/ macos-arm64（macos-15 + brew LLVM + 内联 P0960 补丁）/
  macos-x86_64（macos-13）/ Android（HuxerUI CLI 打 APK，GUI 壳 + libbox 数据面）实验性
  continue-on-error。桌面 job 走 HuxerUI 源码通道（钉 commit clone 到
  `third_party/huxerui/`；上游活跃开发中，本机 pull 后同步更新 workflow 的
  HUXERUI_COMMIT，当前 371072b）。
- **Windows 自定义安装向导**（`platform/windows/package/`）：`huxerui package
  windows` 产出自含 Burn setup.exe = MSI + 托管安装器 UI（HuxerUI 写的向导，
  `UseInstaller()` 会话驱动：安装目录选择/桌面快捷方式/修复/卸载/进度/回滚）。
  字符串走资源系统（default + zh/zh-TW/zh-HK）。install 规则是安装包文件
  唯一来源：自带 sing-box 与 assets 在顶层 CMakeLists 的 Windows 打包块显式
  install；日常构建零开销（`HUXERUI_PACKAGE` 未开时函数直接返回）。
- `src/core.cpp` 按 `_WIN32` 分流：POSIX posix_spawn / Windows CreateProcess
  后端，同一 `core::CoreProcess` 接口。
- 服务模式仅 Linux；其他平台 `service::available()` 恒 false，自动回落直接
  spawn。Linux PPTP/OpenVPN 必须经统一 root 服务，不再由普通 GUI 直接 spawn
  `pppd`/`openvpn` 或执行 `ip route`；Windows PPTP 使用系统 RAS，OpenVPN 使用
  OpenVPN Community CLI，Android 后续接入 VpnService。

## 关键约定（改代码前必读）

1. **`import std;` 后禁止再 `#include` 标准头**。C/系统/第三方头放全局模块片段
   （`module;` 与 `module clashflux.x;` 之间）。普通 UI .cpp / ui.h 头用哪个
   std 设施就自己 `#include` 哪个（libstdc++ 传递包含在 libc++ 上不存在）。
2. **UI 层遵守 skill 的 DSL 风格**：普通 .cpp、composable 不加 inline、View 按值
   传递、具名 View 链式调用前 `std::move`（`.With` 等是右值限定）。
3. **受控值以应用状态为权威**；TextField 保留完整 TextEditingValue；动态兄弟用
   稳定 `.Key(...)`。**同一受控 TextEditingValue State 同一时刻只能挂载一个
   TextField**：IndexedPages 的隐藏页保持挂载，与弹窗共用表单 State 的次要页面
   在非激活时必须渲染为空（id==0/未打开），否则输入法组合值会被另一实例当作
   外部权威值，HuxerUI 抛 invalid_argument 直接崩溃。所有密码、令牌和其他秘密
   输入统一使用 `PasswordField`
   约定：`Secure()` + HuxerUI `TrailingIcon`/`OnTrailingIconClick`，复用内置的
   显隐眼睛操作；不要自绘重复的眼睛按钮，也不要把秘密值写入日志、卡片或错误
   文本。新增秘密输入框必须沿用这个约定。
4. **线程契约**（src/ui/task_bridge.h）：State 只在 UI 线程读写；api/core/store
   的阻塞方法必须 `co_await RunOnTaskThread(fn)` 派到任务线程池；WS 流与进程
   输出经 `PollWhile(interval, tick)` 泵回 UI。**事件处理器内禁止同步写会导致
   点击节点被卸载的 State**——经 `tasks.Launch` + `co_await Delay(0)` 推迟。
5. **占位不能用 Spacer().With(Frame)**（Spacer 自带 Grow(1) 会平分空间）——
   用空 `Row{}`/`Column{}`；页面根要 `Grow(1.0F)` + `CrossAlign(Stretch)`。
6. 内核 REST 全部走 `store::coreStore().api()`；UI 不直接持有 curl。
7. **岛屿风**（对齐 apitab）：极简黑白主题（app.cpp MinimalDark/Light）；
   一级岛 16pt / 二级岛 8pt 圆角，表面色经 `ResolveIslandTheme(theme)` 语义
   层级取，不直接用 surface_container_*。订阅卡交互范式：右键
   `ViewEvents::ContextMenuRequested` + `UseMenu().ShowAt`，双击
   `MultiTapGesture{.count=2}` + `MultiTapEvents::Recognized`，矢量图标着色
   用 `Image::Tint`（IconButton/Foreground 不着色 SVG）。
8. **响应式**：`UseViewportClass()` Compact(<600) 收窄侧栏(44pt)/一级岛内边距
   （PageScaffold）/首页卡片 2×2/订阅卡整宽列表；窗口最小 560×480。
9. **平台组件收束**：平台差异只在页面/组件边界用编译宏选择一个完整函数，
   例如 `CLASHFLUX_SETTINGS_PLATFORM_SECTION` 选择
   `AndroidSettingsSection` 或 `DesktopSettingsSection`；宏不要下沉成控件级
   过滤器，也不要在通用页面里维护平台能力矩阵、`PlatformControl`、全局
   `isAndroid` 标记或类似的分发状态。HuxerUI codegen 不支持 composable 函数体内
   的条件编译，`#if defined(__ANDROID__)` 应放在文件作用域，选择函数后由函数
   自己完成组合。
10. **平台命名与高内聚**：通用组件使用不带平台前缀的名字（如 `SettingRow`、
    `SectionTitle`）；平台专属函数必须带平台前缀（如 `Android...`、`Desktop...`、
    `Linux...`）。一个平台函数内部自洽管理自己的 State、TaskScope、生命周期、
    权限请求、乐观状态、错误提示和控件树；调用方只提交统一的数据/动作，不再
    同时理解多个平台的细节。新增平台差异时优先新增一个前缀函数和一个宏选择点，
    不扩大全局 UI 状态模型。
11. **平台编译边界**：平台专属实现放入对应的 `platform_*.cpp` 或组件文件的
    平台分支；新增 Android UI 源文件必须同步加入 `cmake/AndroidLegacy.cmake`
    的 legacy 源列表。核心运行时若确实需要 `PlatformKind` 等领域模型可以保留，
    但不得把领域运行时枚举重新用作 UI 控件能力分发。
12. **单实例与列表一致性**：桌面 GUI 入口必须先经过
    `clashflux::instance::acquireOrActivate()`；第二次启动只唤醒已有窗口并退出，CLI
    参数不受单实例锁影响。Android 主 Activity 使用 `singleTask` 保证回到已有任务。
    窗口的“启动时隐藏到托盘”只能绑定一次性生命周期，不得
    在组合函数每次重组时直接 `Hide()`。日志、连接、规则等信息列表统一通过
    `UnifiedListRow` 管理表面/间距/圆角；紧凑视口禁止复用桌面固定列宽。全量推送列表
    页面切换后应读取最近快照并按帧去重，不能只依赖“新事件”才能恢复显示。

## sing-box 交互要点

- clash_api：`127.0.0.1:29097`（避开其他代理软件常用的 9090/9097），secret 首启生成存 settings 表
  （`core.secret`），REST 走 `Authorization: Bearer`，WS 走 `?token=`。
  `mode_list` 自定义为小写 `rule/global/direct`——UI 与 settings 的模式字符串
  直接对齐，切换模式走 `PATCH /configs {"mode":...}`（clash_api 唯一常用的
  热更字段）。
- 运行时配置 = `singbox::compileConfig(订阅, options)` 合成 sing-box JSON，
  写 `core/config.json`，`sing-box run -c config.json -D core/` 启动。编译器
  语义（对齐 sing-box 1.14）：block/dns special outbound 已移除（REJECT 走
  规则 `action:reject`）；sniff/DNS 劫持走前置 rule action；字段集保持严格
  最小（sing-box 对未知字段拒绝启动，不要透传 clash 字段如 `udp`）。
  控制器就绪轮询 ≤30s（GEOIP 首启要拉远程 .srs 规则集，5s 会误判）。
- 订阅转换保真度（第一期）：协议 ss/vmess/vless/trojan/hysteria2/tuic/http/
  socks5；组 select→selector、url-test/fallback/load-balance→urltest；
  规则 DOMAIN/DOMAIN-SUFFIX/DOMAIN-KEYWORD/DOMAIN-REGEX/IP-CIDR(6)/GEOIP
  （PRIVATE→ip_is_private，国家码→SagerNet 官方 .srs rule_set）/MATCH；
  RULE-SET/小众协议/SS 插件等降级为 `CoreSnapshot.warnings`（设置页展示），
  不静默丢弃。原生 sing-box JSON profile 直通（合并托管设置）。
- 默认混合端口 **7899**（避开 Clash 7890 / Verge 7897 常见占用）。
- 规则页分为“订阅规则”和“全局路由”：前者按 Profile 读取 YAML `rules` 或原生连接
  `nativeRoutes`，后者保存 `vpn.global_policy`（默认主连接 + 多条域名/IP/CIDR →
  连接规则）。全局策略由 `VpnManager` 统一选举，原生连接建立时再安装对应网段；
  不把某个内核 `/rules` 快照误当成所有订阅的规则。
- 系统代理（`proxy.system_enabled`）：内核就绪后重指当前端口；stopCore 先
  摘代理再停内核（防系统指向死端口断网）。TUN（`core.tun_enabled`）：
  clash_api 不支持热更 tun——运行中切换会重新编译 config.json 并自动重启
  内核（重启失败回滚设置并恢复无 TUN 运行）；allow-lan/log-level 同理只能
  重启生效（设置页按“下次启动生效”提示）。
- 测速：单节点 `GET /proxies/{name}/delay`；sing-box 无组聚合端点，
  `ClashApi::groupDelay` 取组成员后并发 fan-out（≤8 线程）逐节点测速，合并成
  与旧 mihomo 聚合端点相同的 {节点: 延迟} JSON（失败项记 0 = UI 超时）。
  测速超时要比 curl 传输超时窄。
- 日志级别词表：UI/设置存 mihomo 风格 `silent/error/warning/info/debug`；
  配置生成映射 `warning→warn`、`silent→disabled`，WS 订阅 `?level=` 同步映射，
  `stream.cpp` 把内核推送的 `warn/trace` 归一回 UI 词表。
- 订阅下载双通道：桌面走 vendored curl（`ClashApi::downloadToFile`，支持订阅级
  代理/无效证书选项）；Android 的 curl 无 TLS（NDK 无 OpenSSL，https 会报
  Unsupported protocol），订阅导入/手动刷新/自动更新全走 HuxerUI
  `HttpClient`（平台原生栈，自带 TLS/证书/系统代理）。平台选择收束在
  `AndroidImportProfile`、`AndroidRefreshProfile` 和 Android 刷新泵等平台函数内，
  store 只建行/收尾（`profiles::createRemote` + `completeRemote` + `dueForUpdate`）。

## 里程碑状态（2026-09-05）

- ✅ M1：脚手架 + 内核生命周期 + REST/WS + 六页骨架（订阅 CRUD/代理组切换测速/
  规则列表/连接快照/日志流/设置）。
- ✅ 系统代理（KDE kioslaverc / GNOME gsettings）+ TUN 开关（设置页「系统」卡）。
- ✅ 统一 root 服务（systemd 托管 sing-box + Linux PPTP/OpenVPN，socket 行协议，UID 校验，
  pkexec 安装）+
  完整 CLI（同一二进制子命令）+ 多平台 CI（Linux 正式，Win/macOS/Android 实验）。
- ✅ 全平台内核统一 sing-box（桌面 spawn 官方二进制 / Android 进程内 libbox，
  同为 1.14.0）：订阅编译器 clashflux.singbox 下沉 C++，Android 删除 Java 侧
  转换器（SingBoxConfig.java + snakeyaml）。
- ⬜ 待做：RULE-SET/rule-providers 转换（订阅规则集保真）、流量图表增强、订阅
  合并策略增强（规则覆写）、全局策略对多实例的运行时编排、deep link
  （clash://install-config）、开机自启、规则 provider 管理、连接详情。
