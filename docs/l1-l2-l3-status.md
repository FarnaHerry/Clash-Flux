# L1 / L2 / L3 开发完成度复核

## 2026-10-30：Linux 自升级保留授权会话，服务安装后恢复内核

候选版本：0.3.82。RPM/Fedora 分支用户
反馈确认升级后应用退出，但未弹授权且未安装。代码确认旧流程仅以快照 ready 为退出条件，
尚未确认 Polkit 或包管理器结果；本轮没有取到用户现场日志，不能断言实际失败来自哪层。
Linux 改为保留主进程等待授权和安装，成功才退出；取消/失败恢复原窗口与同一持久化会话。
只提升 dpkg/rpm，禁用无窗口的内置终端授权代理，返回有界安装诊断，继续消费 sealed memfd。
服务设置入口安装/更新成功后清理旧客户端状态并默认启动待命；连接/TUN 等触发安装时
保留原操作票据，续跑原命令及参数，不先强制启动待命。启动时自动服务同步保持启动意图。

`cmake --build build --target test_app_update test_app_update_async test_persistence` 通过；
`ctest --test-dir build --output-on-failure --no-tests=error -R
'^(app_update|app_update_async|app_update_handoff|persistence)$'` 4/4 通过。Linux handoff 使用
测试构建替换 pkexec 入口，走生产父进程及助手代码，覆盖 DEB/RPM、授权等待期间父进程存活、
快照不可改及缓存替换、安装成功、取消、缺授权代理及包管理失败；没有执行真实提权或安装。
`ui_resources` 也通过；上述四项加资源检查共 5/5。`test_page_transition` 构建依赖主目标，
被本机 GCC 16 的 `stl_map.h:420 make_decl_rtl` 内部编译器错误阻断。直接执行
`cmake --build build --target clash-flux` 还出现 `stl_list.h` 的模块声明冲突；HuxerUI
codegen 已执行，但未产生可验收的新主目标。`./run.sh --version` 使用旧二进制仍退出 139；
`git diff --check` 通过，规范文件长度限制通过。未完成 Fedora/Polkit 实际安装、Windows
服务安装/TUN 实机转换验收；没有以模拟助手替代上述验收。

## 2026-10-30：修正待命内核与其他客户端 TUN 的共存检查

代码快照：`3d5d224` 加本轮工作树，候选版本为 0.3.81。Windows 用户确认 Clash Verge
开启 TUN 时，待命启动被“其他活动隧道 meta 正在接管 IPv4 默认路由”拒绝；关闭 TUN
即可启动。根因是候选准备仍无条件应用开放监听时的全局接管检查，不是本次端口绑定失败。
现将运行计划的 proxyListening 传给生产检查器，仅无业务入站的待命允许已有全局隧道；
开放监听、候选 TUN 的全局路由、接口名和地址冲突保护保留。未改系统路由或其他客户端。

Linux/GCC 16 用 `-std=c++23 -O2` 独立编译生产 network_conflict.cpp 与现有回归头：
旧检查器配新增用例稳定失败于 IPv4 待命，新检查器通过全部判定及真实只读 OS 快照。
`python3 tests/test_network_conflict_namespace.py <临时回归驱动>` 6/6 通过，使用独占
user/network namespace 的真实 TUN，覆盖 IPv4/IPv6 默认及两半默认路由、地址重叠、离线
残留；同时断言待命放行、开放监听仍拒绝、接口与地址冲突不因待命标志被豁免。

正式 test_runtime_evidence/test_singbox 目标已构建；`ctest --test-dir build
--output-on-failure --no-tests=error -R '^(runtime_evidence|singbox|proxy_standby_runtime)$'`
中前两项通过，proxy_standby_runtime 在回环 URLTest 超时，单独重跑仍失败；未放宽断言。
用正式 `build/test_runtime_evidence` 重跑上述真实 TUN namespace 脚本也为 6/6 通过。

`cmake --build build --target clash-flux test_runtime_evidence test_singbox` 最初在下载
SweetLine 子模块时遇到 GitHub TLS unexpected EOF，临时使用 HTTP/1.1 且保留证书校验后
恢复。发现本机干净的 HuxerUI checkout 仍为 a954965，按已有项目 pin 同步到 5dd4009，
未改变固定版本或用户配置。重编主目标仍在未改动的代理页/common.cpp 触发 GCC 16.2.1
`stl_map.h:420 make_decl_rtl` 内部编译器错误，单线程重试未解决。HuxerUI codegen 已执行，
主目标未构建成功。同步框架前 `./run.sh --version` 输出旧产物 v0.3.68；同步并重编框架后，
旧应用尚未重新链接，版本命令退出 139，不能混用旧二进制与新框架，也不能计作新代码版本
验证。候选版本再次运行 `cmake --build build --target clash-flux`，仍在 GCC 16.2.1 的
`stl_map.h:420 make_decl_rtl` 触发内部编译器错误，日志为 `/tmp/clash-flux-v0.3.81-build.log`；
`./run.sh --version` 仍因旧应用与新框架 ABI 不匹配退出 139。版本号人工核对为 CMake 0.3.81、
Android versionName 0.3.81 / versionCode 159；`git diff --check` 通过。2026-10-30 推送标签
`v0.3.81`（提交 `41ed2fb`）后，Release CI run 41022896006 的 Linux x86_64/arm64、Windows
x86_64、macOS arm64、Android、Windows 安装器及 publish-release 全部通过；APK 的 v1、v2
签名和正式证书 SHA-256 均通过核验。GitHub Release 发布了 10 个平台安装包。iOS Simulator
诊断 job 因 HuxerUI 链接依赖 `SweetEditor3p::Simdutf` 无法解析而失败，按策略不阻塞 Release。
Windows Clash Verge + Clash-Flux 双客户端 TUN 实机验收仍未完成；本机 GCC 16 编译与旧二进制
版本命令也仍失败，详见上述记录。

## 2026-10-30：桌面内核待命、代理监听与接管解耦

代码快照为 `d993688929490b935cd96e8db52d629afda2a7d9` 基线上的本轮工作树，版本仍为
0.3.79；未提交、打标签、推送或打包。完整状态与原语见[桌面代理生命周期](desktop-proxy-lifecycle.md)。
桌面默认启动无业务入站的内核，Connect 开放监听并应用独立的系统代理/TUN 选择，
Disconnect 清理接管及原生连接后回到待命。两种接管均关闭时可手动使用本地端口，
首页动画/时长依据代理会话；待命与重配不把保存意图冒充实际接管。Android 启停策略不变。

Windows / MSVC 18 Release：`cmake --build build --target clash-flux test_singbox
 test_page_transition` 通过，包含 HuxerUI codegen；相关 test_persistence、test_service_protocol、
test_vpn、test_routing、test_compensation 也已构建。
`ctest --test-dir build --output-on-failure --no-tests=error -R
 '^(proxy_standby_runtime|singbox|service_protocol|vpn|routing|compensation|persistence|project_test_gate|ui_resources|page_transition)$'`
10/10 通过（54.38 秒）。最初未加载开发环境的门禁检查因 PATH 中缺少 Ninja 失败，
加载 MSVC 开发环境后重跑通过。`./run.ps1 --version` 输出 Clash-Flux v0.3.79；
`git diff --check` 通过，AGENTS.md 与项目 SKILL.md 长度限制符合要求。

新增 proxy_standby_runtime 使用固定 sing-box 1.14.4、真实原生/兼容控制接口和回环 HTTP
目标，对 Clash YAML 与原生 JSON 验证待命→监听→待命：待命仍能 URLTest，业务端口拒绝
连接；监听时实际 HTTP 请求通过本地代理。编译回归核对出站、DNS、路由不因暂停监听而改变。
没有重启用户正在运行的 GUI，没有修改真实系统代理、建立 TUN 或拨号；SCM/WFP/物理 DNS
管理员回归、系统代理与 TUN 组合实机验收、Linux/macOS/Android 构建和设备验证未覆盖。

## 2026-10-30：准备 v0.3.79 补丁版本

版本 `0.3.79`，Android versionCode `157`（`0.3.78` / `156` 的递增版本）。发布说明覆盖
主题预览、代理单节点测速、全局规则保存、Windows/Linux 服务生命周期及 Android 后台修复；
README 中英文入口已更新。没有生成安装包或 Android 正式签名包。

Windows/MSVC 将全量目标构建到独立 `build/release-candidate-0.3.79` 输出目录，避免覆盖
正在运行的开发窗口；`CLASHFLUX_BIN` 指向候选文件时 `./run.ps1 --version` 输出 v0.3.79。
Windows 25 项隔离回归全部通过（含 `project_test_gate` 与 `cps_dependencies`）；Android
宿主策略测试 `:app:testRuntimeStartPolicy` 与 `:app:testVpnUptime` 通过。
未在当前日常系统执行会安装 SCM 测试服务、安装 WFP 过滤器或修改物理 DNS 的 3 项测试；
Android Release APK 签名/跨版本安装与其它平台完整 CI 仍需发布流水线验收。仅准备本地
提交及 tag，不推送、不触发线上 Release。

## 2026-10-30：补齐订阅页 hydrate 门禁，修复启动回归

下节验证遗漏了真实 GUI 启动。用户随后报告开发版出现透明窗口，Windows 事件日志
也记录相同开发路径在 10:08 的两次 ucrtbase/c0000409 异常退出。静态调用链确认：
订阅页在首帧（包括不可见挂载）无条件调用 VpnStore::init，新增 ready 检查抛出的
异常未被页面任务捕获；HuxerUI 对未捕获 Task 异常执行 terminate。

订阅页现监听 ProfilesModel::loaded，完成 hydrate 后才发起初始化；生产异步封装
LaunchHydratedVpnSnapshot 将 worker 异常转成反馈并保留已有模型快照。
新 headless UI 回归实际消费该封装和 RunOnTaskThread，覆盖首帧不提前读取、hydrate
唤醒、工作线程异常后 Runtime 存活与重试成功。版本保持 0.3.78，数据库与订阅文件不变。

Windows/MSVC `cmake --build build --target clash-flux test_page_transition` 通过，含订阅页
与测试 HuxerUI codegen；`ctest --test-dir build --output-on-failure --no-tests=error
-R '^(page_transition|ui_resources|persistence)$'` 3/3 通过（44.17 秒）。直接启动
`build/health-evidence/clash-flux.exe`，核对实际进程路径、首页首帧、订阅列表与全局规则
可见；结束本轮自行启动的诊断进程后再次启动，首页正常，原有一条全局规则仍在 SQLite。
该重启检查不是托盘正常退出清理验收；没有手动启动代理，未进行真实 TUN/Android/Linux
验收。开发版仍显示既有的服务版本同步提示（非 Program Files 安装不自动更新服务），
不影响本轮窗口绘制与交互。基础主目标构建、版本输出及 diff 检查通过。

## 2026-10-30：全局规则重启读取与保存任务修复

代码快照：`09af459` 加当前工作区，版本保持 0.3.78。用户报告退出重开后全局规则消失；
只读检查本机数据库确认 `settings["vpn.global_policy"]` 仍有一条规则，未输出规则内容或
修改用户数据。现有两张业务表为 profiles/settings，全局策略以 JSON 存储，并未缺少规则表。

发现规则页在订阅 hydrate 前可读取策略，VpnStore 将空回退值通过 call_once 固定为
已加载缓存。现增加页面 loaded 门禁和生产 HydratedOnce ready 检查，未就绪或解码失败
可重试。全局规则保存迁到应用任务域，离页不取消在途保存；工作线程拥有错误字符串，
递归保存调度改为弱引用，避免自持有环。既有数据库与策略格式不变，无数据迁移或清理。

Windows/MSVC 执行 `cmake --build build --target clash-flux test_persistence test_page_transition`
通过，包含规则页 HuxerUI codegen。`ctest --test-dir build --output-on-failure --no-tests=error
-R '^(persistence|page_transition|ui_resources)$'` 3/3 通过（86.22 秒）。新增回归使用真实 SQLite
和生产策略 codec，覆盖过早读取后成功 hydrate、解码失败重试、真实写锁期间连续编辑及
ABA、关闭重开后规则/修订号/三个来源及所有被引用文件保留。

最终 `cmake --build build --target clash-flux`、显式 CLASHFLUX_BIN 的
`./run.ps1 --version`（v0.3.78）与 `git diff --check` 通过；根规范及项目技能限额通过。
未操作已安装服务或真实网络，未运行真实 GUI 重启验收、完整 required 套件或 Android/macOS/Linux
完整构建；上述回归不等于实机 VPN 验收。

## 2026-10-30：修复桌面生命周期并统一 Windows/Linux 服务入口

代码快照：`09af459` 加当前工作区，版本保持 0.3.78。修复下方审查的三个缺口：析构只
清理本对象负责且非 CLI 驻留的服务内核；Linux 替换子进程必须确认旧 PID 已释放，失败
不 spawn、不覆盖旧身份；启动等待超时走统一 `stopCore`，停止失败保留所有权和 PID 文件，
启动原因与清理错误同时可见。驻留进程停止失败不再提前清除 adopted 标记。

按 Windows/Linux 统一服务定位，应用层共用 `service::required()`、CoreStore 所有权和
启停流程，两端 STATUS 也只由同一公共函数解析。Linux 普通启动不再回落无服务 spawn，
包括 root GUI；底层 `CoreProcess::start` 和 `spawnDetached` 同样拒绝绕过服务。
服务缺失时沿已有显式安装授权流程，启动本身不自动提权。命令继续复用同一 Runtime/缓存
或向已有实例转发；保留一次性命令的服务驻留语义，没有另建 CLI 数据面。显式 STOP 对
可达服务幂等提交，即使子进程已退出也让服务清理所属路由；失败保留清理责任，未知 STATUS
不被当作停止确认。详细契约见[桌面内核生命周期](runtime-health-review.md#桌面内核所有权与失败清理)。

Windows/MSVC 执行 `cmake --build build --target clash-flux test_runtime_health
test_service_protocol test_runtime_evidence test_rule_provider_download test_page_transition`
通过，包含 HuxerUI codegen；`ctest --test-dir build --output-on-failure --no-tests=error
-R '^(service_protocol|runtime_health|runtime_evidence|native_control_runtime|page_transition|ui_resources)$'`
6/6 通过（132.18 秒）。新回归覆盖停止失败阻断替换、旧 PID 保留、未释放 PID 拒绝、
重试与 spawn 失败；实际内核 API 回归同时执行应用/CLI 数据面绕过拒绝断言。

WSL Fedora Linux 44 / Clang 22.1.8：`test_service_protocol` 与 `test_runtime_evidence`
以独占 `/tmp` 输出目录编译执行通过；新增替换回归使用自有真实子进程并在结束回收，
不接触真实服务/路由。另用仓库外脚本 `clashflux-linux-service-module-compile.py`
按实际 libc++ 模块编译生产 service、config、pptp、vpn、compensation 接口及 service 对象；
`clashflux-linux-core-owner-test.py` 进一步编译生产 nlohmann/singbox/core 模块和 core.cpp，
链接有界检查程序验证 service 必需、CoreProcess/CLI detached 拒绝，执行通过。该独立链接
使用 section GC，仅计为生产模块和拒绝门禁验证，不计为完整 Linux GUI 构建或应用运行验收。

`cmake --build build --target clash-flux`、`./run.ps1 --version`（v0.3.78）、
`git diff --check` 通过；根 AGENTS 与修改技能均在行数/字符上限内。未替换已安装服务，
未执行完整应用 CLI 退出/30 秒超时到真实服务的故障注入、真实 TUN/DNS/Codex 或双客户端
切换验收，也未运行完整 required 套件、Android/macOS 构建或签名发布。

## 2026-10-30：桌面生命周期审查（Linux 业务故障原因未确认）

本节保留修复前审查证据；三个缺口的后续修复与实际验证见上节。

代码快照：`09af459` 加当前工作区，版本 0.3.78。本轮只审查与运行隔离回归，未修改
桌面生产代码或操作真实服务、系统代理、路由。用户报告 Linux 控制器显示已连接但 Codex
不可用，换 Clash Verge 有时恢复；后续确认使用 TUN，且仅 Codex 不通。发行版、Codex
桌面/CLI 形态、进程代理环境和现场报告尚未取得，不能归因。局部业务失败使规则/出口、
目标 DNS 与进程代理环境更值得优先对照，但不能因此排除 TUN 的目标相关选路或 IPv6 问题。

静态审查发现三个待修缺口：

- `CoreStore` 析构在 Linux 会查询并停止已有 root 服务内核，而不要求本对象取得所有权；
  `serviceDetached_` 只在 Windows 生效。无现存 GUI、单独执行状态查询的 CLI 可能在退出
  时停止旧服务内核，CLI detached START 的驻留契约同样受到影响。
- `service_daemon.inc` 的 START 忽略 `stopEngine` 返回值；旧子进程停止失败时仍 spawn
  并覆盖 `childPid`，丢失旧实例监管，可能发生监听/网络资源冲突。
- `startCoreUnchecked` 等待原生控制 30 秒超时后，非服务分支只调用本地 `process_.stop()`，
  未覆盖 detached/adopted PID；服务分支忽略 STOP 失败仍清除所有权标记。失败后界面状态
  与实际后台资源可能不一致，应保留清理失败及所有权并走统一停止路径。

这些是代码路径证据，尚未对三个失败分支执行完整应用故障注入，也未证明触发了用户现场。
现有 Running 由所属进程/鉴权原生控制就绪发布，业务目标另由主动诊断经 mixed 入站检查；
控制器连接、节点延迟或统计帧不能证明 Codex 的 TUN/DNS/出口链路正常。

Windows/MSVC 构建 `test_service_protocol test_runtime_health test_runtime_evidence
test_rule_provider_download` 通过；执行 `ctest --test-dir build --output-on-failure
--no-tests=error -R '^(service_protocol|runtime_health|runtime_evidence|native_control_runtime)$'`
为 4/4 通过（62.38 秒）。WSL Fedora Linux 44 / Clang 22.1.8 用独占 `/tmp` 输出目录
直接编译并执行生产 `test_service_protocol.cpp`，以及 `test_runtime_evidence.cpp` +
`runtime_network_probe.cpp` / `health_history.cpp` / `network_conflict.cpp`，两项通过；包含
loopback DNS 错误/取消、真实系统只读查询、现场历史/重开和脱敏，不是真实桌面 TUN 验收。
基础检查 `cmake --build build --target clash-flux`、`./run.ps1 --version`（v0.3.78）及
`git diff --check` 通过。没有 Linux 完整应用重建、真实公网 Codex、桌面系统代理、实际服务
切换或跨客户端 TUN/DNS 验收结论；上述缺口仍待修复。

同日 TUN 专项补充：WSL 执行 `python3 tests/test_network_conflict_namespace.py
/tmp/clashflux-desktop-audit-20261030-n_41wmpd/test_runtime_evidence`，由脚本自行隔离
user/network namespace、建立真实测试 TUN；IPv4/IPv6 默认和成对 /1 路由、自身豁免、
地址占用与接口 down 六种场景全部通过。未修改宿主路由。配置审查确认托管 TUN 使用
auto_route、严格路由和 auto_detect_interface，并保留 DNS sniff/hijack 前置动作；没有依据
此次局部失败直接改 strict_route、放宽 TLS、全局直连或替换用户节点/规则。
此回归不覆盖故障机器的实际 DNS 服务、业务规则、出口或 Codex 长连接。

## 2026-10-30：Android 重开启动权限与停止收尾（完整构建受阻）

代码快照：`09af459` 加当前工作区，版本保持 0.3.78。保留从最近任务划走 Activity 后
后台 VPN 继续运行的行为，也保留按持久化模式进行真正的系统 sticky 恢复。
UI core-only 请求只保留当前真实运行/启动中的 VPN，不再依据旧持久标记升级为 VPN；
无 TUN 运行时仅在内存中过滤 TUN inbound，`openTun` 再检查当前请求与取消状态。
显式停止、撤权和 libbox 停止回调统一走后台收尾；数据面关闭失败仍尝试关闭 TUN，
保留失败句柄和服务供重试，确认释放后才移除通知。缺失服务对象不再把错误快照默认为停止。

宿主 JVM 的 `:app:testRuntimeStartPolicy`、`:app:testVpnUptime`、`:app:testAppUpdatePolicy`
通过，前两项接入 Android `preBuild`。使用 JDK 17、Android SDK/NDK 29、固定 libbox AAR
及同源 HuxerUI 执行 `:app:compileDebugJavaWithJavac`：先补齐本机 CMake 4.4.2 PATH，
再通过仓库外 init script 将 native staging 缩短到 `C:/tmp/clashflux-android-20261030`，
绕过 Ninja 260 字符限制。后续在 SweetEditor 静态库 post-build 阻塞：拼接出
`sweeteditor/temp_extract/C:/.../libsimdutf.a`，Windows 报 Invalid argument；未修改依赖源码或
跳过正式 native 门禁，因此完整 Java/NDK/APK 构建未通过。

另外使用仓库外 `clashflux-android-java-diagnostic.init.gradle` 的
`:app:compileLifecycleJavaDiagnostic`，按正式 debugCompileClasspath、SDK、AIDL/R 和全部
应用 Java 源码独立编译通过；仅 BuildConfig 使用取自实际 Gradle variant/CMake integration
plan 的编译专用 fixture，产物在 `build/android-lifecycle-test`，不打包、不替代正式构建。
核对固定 libbox revision 的 CommandServer/StartedService 关闭实现：控制面 close 不代替
closeService，数据面关闭失败时仍保留该 owner。Windows 主目标
`cmake --build build --target clash-flux`、`./run.ps1 --version`（v0.3.78）与
`git diff --check` 通过。`adb devices` 无设备；未完成真机划走重开、实际 TUN、通知/系统 VPN
图标、撤权、sticky 恢复及关闭故障注入验收，不计为平台通过或可发布。

## 2026-10-30：单节点刷新图标与圆形反馈对齐

代码快照：`09af459` 加当前工作区，版本保持 0.3.78。单节点测速改为 refresh 图标，
使用框架 IconButton 的 16pt 方形命中区域与圆形状态层，14pt 图标居中；62×16pt 延迟槽保留，
按钮贴最右端，左侧空白仍触发整卡选线。移除自定义 Row 按钮造成的反馈圆圈与图标偏移。
按钮保留挂载，测速期间与关于页检查更新共用 `RefreshIconRotation`（0.9s、线性循环），
完成立即复位、reduced motion 静止；切页返回时先建立零角度再恢复循环。

Windows/MSVC 执行 `cmake --build build --target clash-flux test_page_transition` 通过，
包含 HuxerUI codegen；首轮因开发版占用输出文件链接失败，用户退出后重建成功。
`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'`
两项通过（45.23s）。生产组件回归断言方形按钮位于槽右端且垂直居中、状态切换不改变布局、
左侧空白不会触发测速、点击按钮不选线、实际旋转后呈现几何变化，以及切页返回继续旋转。
`./run.ps1 --version` 输出 v0.3.78；`git diff --check` 通过。
本轮还在独占临时 APPDATA 原生窗口检查未测速卡片及右端刷新按钮悬停，圆形反馈与图标重合，
不再在左侧空槽出现独立圆圈；隔离实例已关闭。未测试真实节点网络、手机实机或 VPN/服务接管。

## 2026-10-29：节点 hover 单独测速

代码快照：`09af459` 加当前工作区，版本保持 0.3.78。桌面非组节点卡片 hover 时，
右下角延迟文字由现有 speed 速度表图标替代；62×16pt 槽位在文字、按钮、加载指示器之间保持几何，
离开卡片恢复结果。独立点击只调用该运行 tag 的 delay API，不选中线路；请求及结果归应用测速模型。
同节点拒绝重复请求，不同节点可分别测速，与整组扫描互斥；切页继续，换计划丢弃旧句柄和旧结果。
Android 的原生组测速保持现状，本轮未增加 Android 单节点入口或后台接口。

Windows/MSVC 执行 `cmake --build build --target clash-flux test_page_transition` 通过，
包含主目标及 HuxerUI codegen；初次链接因开发版占用输出文件失败，用户退出后重建成功。
`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'`
两项通过（22.49s）。新增无窗口回归直接使用生产 `ProxyNodeTile` 与单测调度，真实鼠标 hover/点击，
覆盖只替换当前节点、移入图标保持 hover、槽位几何、子按钮不触发整卡选线、不同节点独立、
组卡片不显示入口、切页保留、异常后重试、旧计划结果隔离及退休卡片句柄拒绝。
`./run.ps1 --version` 输出 v0.3.78，`git diff --check` 通过。
未执行原生窗口像素检查、真实节点网络测速、Android 构建/实机或 VPN/服务接管验收。

同日追加空延迟专项：回归用例将单测首节点改为从未测速，并使用与生产相同的空 Row 延迟占位，
验证空槽 hover 显示图标、独立点击、加载及返回结果。Windows/MSVC 主目标和测试构建通过，
同一 CTest 命令两项通过（29.09s）；版本仍为 v0.3.78，差异空白检查通过。
另以独占临时 APPDATA 实际启动最终二进制，导入两个仅用于预览的本地 SOCKS 节点，
保持内核停止，实际检查选中/未选中卡片：两者初始均无 ms，鼠标悬停均显示速度表图标，
移向另一张卡时前一张恢复空延迟。未复现“无 ms 导致 hover 不显示”；分组类型卡片按设计无单测入口。
隔离实例已关闭，未执行节点网络请求或改动用户订阅。

## 2026-10-29：测速进度与结果跨页面保留

代码快照：`09af459` 加当前工作区，版本保持 0.3.78。测速批次及节点状态移入
`ProxiesModel::tests`，应用任务域负责整组请求与原生结果等待；节点卡片只观察共享状态。
切页卸载卡片后任务继续，返回时恢复进度或已完成结果；虚拟列表未挂载节点同样参与，
桌面最多 8 个并发请求。请求结束检查批次与运行计划，停止/换计划清除旧缓存，拒绝旧结果覆盖。

Windows/MSVC 执行 `cmake --build build --target clash-flux test_page_transition` 通过，
包含主目标及 HuxerUI codegen。执行
`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'`
两项通过（24.35s）。新增无窗口回归使用生产测速模型及请求调度，真实卸载/重挂发起视图，
覆盖进行中恢复、离页完成、12 节点整批/8 并发、重复点击、异常清理后重试、
旧请求晚到、运行计划变化失效和原生快照离页完成。测试通过独立 gate 控制测量结束，未访问真实节点。
`./run.ps1 --version` 输出 v0.3.78，`git diff --check` 通过。
未运行真实节点网络测速、手机/libbox 实机或 VPN/服务接管验收。

## 2026-10-29：窄屏颜色编辑二级页

代码快照：`09af459` 加当前工作区，版本保持 0.3.78。Compact 的添加颜色及右键编辑颜色
通过 ThemeColorEditorRoutePage 压入 NavigationStack，返回直接 Pop 到主题页；表单与宽屏弹窗
共用 ThemeColorEditor 的 HEX/HSB 状态及保存逻辑，二级页通过 CompactPageActionContent
固定底部添加/保存按钮并避让系统安全区。页面进入后放大窗口保留页面身份与输入状态。
颜色值统一使用 MD3 Outlined 输入框，页面与弹窗继承同一主题样式。

Windows/MSVC 主目标、test_page_transition 与 HuxerUI codegen 构建通过；
`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'`
两项通过（25.18s），覆盖共用导航/二级表单布局与资源回归。
`./run.ps1 --version` 输出 v0.3.78，`git diff --check` 通过。
解锁后实际启动独占临时 APPDATA 实例，检查约 424pt 窄窗添加/右键编辑二级页、底部按钮、
宽屏弹窗以及两种形式的 MD3 描边输入框；输入 #AA4477 后最大化再还原，输入值与 HSB 保留。
添加保存直接返回主题页并选中新颜色；编辑为 #123456 后用顶部返回，原 #AA4477 保留且未新增颜色。
隔离实例已关闭。未执行手机系统返回/手势或软键盘、移动端构建、VPN 或发布验收。

## 2026-10-29：订阅选中描边与主题预览卡

代码快照：`09af459` 加本次工作区，版本保持 0.3.78。订阅实际卡片与拖动预览通过
`SelectableTileStyle::Subscription` 使用 2pt 选中描边；模式卡片改为 48pt 高的横向
图标加文字，主题色按候选真实 tokens 展示微型界面及选中标记，不显示颜色名称或 HEX，
保留无障碍名称、编辑/删除与添加入口。预览内容列铺满，选中勾使用固定高度槽避免内容上下跳动。

Windows/MSVC 主目标及 test_page_transition 构建通过，包含 HuxerUI codegen。
`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'`
两项通过（24.77s）；覆盖 320/800pt 深浅主题的矩形模式卡、图标文字顺序、预览内部几何、
选色标记更新、选中内容不偏移、预设/自定义色无可见名称或编码及添加入口。
`./run.ps1 --version` 输出 v0.3.78，`git diff --check` 通过。
Windows 解锁后以独占临时 APPDATA 实际启动最终构建，检查宽窗深浅主题、约 424pt 窄窗排列、
选色反馈及通过弹窗新增自定义色，确认预设与新建色卡都不显示名称或 HEX；隔离实例已关闭。
未执行移动端构建、手机实机交互、VPN 或发布验收。

## 2026-10-29：HuxerUI 同步自定义字体支持

代码快照：`1dcd040` 加原有工作区，应用版本保持 0.3.77；HuxerUI 独立依赖与各平台
CI 固定到已发布的 `5dd4009a3f1bb70a4d617fbedf3e5631a7b49a3d`，包含官方
`03885df` 的自定义字体和主题排版支持。复用另一个任务完成的合并，不新增框架提交；
保留现有页面修改、扩展库固定版本与 Android Java/native 同源约束。

字体公共类型布局变化后，Windows/MSVC 使用原构建配置执行
`cmake --build build --clean-first --parallel 6`，主程序、UI codegen 和测试目标全部通过。
`./run.ps1 --version` 在终端输出 v0.3.77；`git diff --check` 通过。
`ctest --test-dir build --output-on-failure --no-tests=error -L clashflux-required --parallel 3`
实际为 26/28 通过（63.88s），包含生产 page_transition、ui_resources、persistence、
CPS、DNS/TLS、个人规则集运行、升级检查/异步/交接与 VPN 回归。
`windows_service` 报 `Service pipe unavailable (Win32 2)`；`windows_physical_dns_wfp`
无法添加 WFP fixture sublayer。两项保留在门禁中，未将本机环境限制记作通过。

框架维护副本完整重编后，25 项 CTest 全部通过；Android 渲染/JNI 源码的 NDK
ARM64/API 23 语法检查和 JDK 17 下的 `:HuxerUI:compileDebugJavaWithJavac` 通过。
应用以独占临时 APPDATA 启动，实际检查深色首页文字、流量图和零数据圆环图，随后关闭
隔离实例。未运行应用 Android APK、其它平台原生构建、真实 VPN 或安装/发布验收。

## 2026-10-29：新建订阅类型选择弱化

代码快照：`1dcd040` 加本轮工作区，版本保持 0.3.77。新建订阅页面与弹窗共用的
类型分段控件改为选中项透明底色、primary 文字和原有细描边；局部继承当前控件样式，
选中交互状态层改用 primary，保留原有尺寸、焦点、类型切换与表单状态。
Windows MSVC 执行 `cmake --build build --target clash-flux test_page_transition --parallel 4`
通过，包含 HuxerUI codegen；
`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'`
2/2 通过（73.02s）。`./run.ps1 --version` 输出 v0.3.77，`git diff --check` 通过。
未进行原生窗口人工视觉检查或 Android 真机验收；未提交或发布。

## 2026-10-29：窄屏运行诊断二级页

代码快照：`1dcd040` 加本轮工作区，版本保持 0.3.77。运行诊断在 Compact（含桌面窄窗）
经统一 NavigationStack 进入二级页，覆盖根页胶囊导航，复用 SecondaryPageTransition 与返回 Pop；
宽屏入口保留弹窗。页面无弹窗宽高上限，不重复显示内容标题/关闭按钮，放大后保留页面身份。
页面/弹窗分别用 UseState 保留完整目标输入，采集、取消、报告、复制和保存复用原应用模型与任务域。

Windows MSVC 执行 `cmake --build build --target clash-flux test_page_transition --parallel 4`
通过，包含 HuxerUI codegen。新增输入回归初期因等待聚焦光标完全静止而超时，改为正常帧推进后，
`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'`
2/2 通过（71.83s）。检查 320/420/960/390pt 页面内容尺寸、输入保留和无重复标题/关闭按钮；
原有诊断三语、报告滚动、导出/取消、短窗弹窗及导航栈回归继续通过。
`./run.ps1 --version` 输出 v0.3.77，`git diff --check` 通过。
未进行原生窗口人工检查、Android 真机返回/手势或实际网络诊断验证；未提交或发布。

## 2026-10-29：来源后缀统一按数量判断

代码快照：`1dcd040` 加本轮工作区，版本保持 0.3.77。移除代理显示名判断中的
desktop 参数及平台条件编译，各平台统一按对象目录中的不同实际 source ID 数量显示后缀。
手机沿用单活动订阅能力约束，自然显示原名；不改变运行 tag 或多订阅能力边界。
Windows MSVC 构建 `clash-flux` / `test_page_transition` 通过（含 HuxerUI codegen）；
`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'`
2/2 通过（22.14s），覆盖空目录、内置对象、单来源多对象和多来源的通用判断。
`./run.ps1 --version` 输出 v0.3.77，`git diff --check` 通过。
未进行 Android 真机、原生窗口人工或真实多订阅 VPN 验证；未提交或发布。

## 2026-10-29：代理名称按实际多来源显示后缀

代码快照：`1dcd040` 加本轮工作区，版本保持 0.3.77。桌面代理页及托盘线路菜单
仅在当前运行/预览对象目录包含多个不同 source ID 时加订阅名后缀；单来源显示原始名称，
不统计已保存但未参与的订阅、同来源的多个对象或无来源的内置对象。手机保持原名。
只更新显示标签，运行 tag、选择/测速参数与稳定 Key 保持原样。
Windows MSVC 构建 `clash-flux` / `test_page_transition` 通过，包含 HuxerUI codegen；
初次链接被运行实例锁定，按已有授权关闭构建目录中的调试实例后重试通过。
`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'`
2/2 通过（52.41s），覆盖空目录、单来源多对象、多来源、手机及原始名称标点保留。
`./run.ps1 --version` 输出 v0.3.77，`git diff --check` 通过。
未进行原生窗口人工检查、真实多订阅 VPN 或 Android 真机验证；未提交或发布。

## 2026-10-29：宽屏订阅更新时间底部对齐

代码快照：`1dcd040` 加本轮工作区，版本保持 0.3.77。宽屏订阅卡的相对更新时间
移至进度条下方左侧，底行高 16pt，与窄屏一致；卡高保留 180pt，拖动预览和加载骨架同步。
移除卡片上的自动更新间隔提示，保留参与状态，自动更新配置和调度不变。
运行调试实例退出后重试 `cmake --build build --target clash-flux test_page_transition --parallel 4`
通过，包含 HuxerUI codegen；`ctest --test-dir build --output-on-failure --no-tests=error
-R '^(page_transition|ui_resources)$'` 为 2/2 通过（21.40s）。
`./run.ps1 --version` 输出 v0.3.77，`git diff --check` 通过。
未进行原生窗口人工检查、Android 真机或完整平台验证；未提交或发布。

## 2026-10-29：订阅地址精简与长期有效

代码快照：`1dcd040` 加本轮工作区，版本保持 0.3.77。宽屏订阅卡和拖动预览只显示
域名/IP 及显式端口，IPv6 保留方括号，省略协议、用户信息、路径、查询和片段；
按实际可用宽度省略，原始地址仍供下载与编辑。覆盖域名、IPv4、IPv6、显式端口、用户信息和无效地址。
初次链接因运行中的调试 EXE 被 Windows 锁定而失败；获用户授权后关闭指定实例，再次构建通过。

用户给定订阅以默认 TLS 验证只读获取，HEAD 返回 501，GET 返回 200 且元数据包含空的 `expire=`；
响应正文丢弃，完整地址、token 和订阅内容未写入仓库。显式空/0/-1 到期值现在归一为 -1，
宽窄卡片及拖动预览显示「长期有效」；缺失/畸形值仍为未知，正数继续显示到期日期。
沿用现有有符号 expire_at 列，SQLite schema 不变，历史 0 不批量改写，下一次成功刷新识别。

Windows MSVC 执行 `cmake --build build --target clash-flux test_page_transition test_persistence --parallel 4`
和最终主目标构建通过，包含 HuxerUI codegen 与三语资源生成。
`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources|persistence)$'`
3/3 通过（30.48s），包含永久/有限/未知到期解析、真实 SQLite 锁竞争和重开、多来源/稳定 ID/文件保留，
以及长期有效文案的卡片布局。`./run.ps1 --version` 输出 v0.3.77，`git diff --check` 通过。
未进行原生窗口人工检查、Android 真机或完整平台验证；未提交、打标签或发布。

## 2026-10-29：窄屏更新时间移至左下角

代码快照：`1dcd040` 加本轮工作区，版本保持 0.3.77。Compact 订阅的相对更新时间
改在进度条下方左对齐，独占 16pt 底行；名称恢复单行。窄屏卡高从 88pt 调整为 108pt，
实际卡片、拖动预览、加载骨架和 VirtualGrid 行高估算共用该高度，卡距保持 6pt。
Windows MSVC 执行 `cmake --build build --target clash-flux test_page_transition --parallel 4`
通过（含 HuxerUI codegen）；`ctest --test-dir build --output-on-failure --no-tests=error
-R '^(page_transition|ui_resources)$'` 2/2 通过（21.61s）。检查更新时间位于进度条下方、
左边缘对齐且不超出卡片，并覆盖响应式骨架尺寸、虚拟滚动和菜单点击。
`./run.ps1 --version` 输出 v0.3.77，`git diff --check` 通过。
未进行原生窗口人工检查、Android 真机或完整平台验证；未提交或发布。

## 2026-10-29：窄屏订阅使用相对更新时间

代码快照：`1dcd040` 加本轮工作区，版本保持 0.3.77。根据用户补充要求，名称下方改为
「更新于 刚刚 / 5分钟前 / 2小时前 / 3天前」等相对时间，实际卡片与拖动预览复用
现有 `FormatRelativeTime`；缺失仍显示「未拉取」。布局、卡高及到期日期格式保持原样。
Windows MSVC 构建 `clash-flux` / `test_page_transition` 通过（含 HuxerUI codegen）；
`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'`
2/2 通过（61.21s），包含相对时间文案的标题槽几何回归。
`./run.ps1 --version` 输出 v0.3.77，`git diff --check` 通过。
未进行原生窗口人工检查或 Android 真机验证；未提交或发布。

## 2026-10-29：窄屏订阅最后更新日期

代码快照：`1dcd040` 加本轮工作区，版本保持 0.3.77。Compact 订阅卡在名称下方显示
本地日期「更新于 YYYY-MM-DD」，无更新时间显示「未拉取」，PPTP 保留原连接信息。
实际卡片与拖动预览共用日期槽，加载骨架增加对应占位；保持 88pt 卡高及既有流量/到期位置。
Windows MSVC 执行 `cmake --build build --target clash-flux test_page_transition --parallel 4`
通过，包含 HuxerUI codegen；`ctest --test-dir build --output-on-failure --no-tests=error
-R '^(page_transition|ui_resources)$'` 为 2/2 通过（21.23s），检查窄屏及宽窄往返时日期
位于名称下方、不覆盖状态/菜单/流量，卡高、虚拟滚动及菜单点击保持正常。
`./run.ps1 --version` 输出 v0.3.77，`git diff --check` 通过。
未进行原生窗口人工检查、Android 真机或完整平台回归；未提交或发布。

## 2026-10-29：订阅加载骨架响应式尺寸

代码快照：`1dcd040` 加本轮工作区，版本保持 0.3.77。订阅加载改用生产
`ProfileLoadingSkeleton`，与实际列表共用列宽策略、卡高、行距和分页留白；
桌面宽屏最小列宽 340pt、卡高 180pt，Compact 单列高 88pt、行距 6pt，手机宽视口保留单列。
首载的窄窗/手机也显示骨架，标题搜索栏沿用正常页面，Compact 滚动尾部避让悬浮导航。

Windows MSVC 执行 `cmake --build build --target clash-flux test_page_transition --parallel 4`
及最终主目标增量构建通过，包含 HuxerUI codegen。
`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'`
2/2 通过（25.27s）。生产骨架在无窗口 Runtime 检查 320/599/600/695/696/1044/1600/400pt
宽窄往返的实际卡片几何、列数、行列间距和边距，并检查深浅色流光不引发重组或布局变化。
`./run.ps1 --version` 输出 v0.3.77，`git diff --check` 通过。
未进行原生窗口人工检查、Android 真机验收或完整平台回归；未发布新版本。

## 2026-10-29：HUI fork 更新与 VMess 节点订阅

代码快照：`54e96c8` 加本轮未提交工作区，应用版本保持 0.3.76。
读取 fork 最新 farna/main 并核对官方 upstream/main：HUI 固定到
`b08302858fe7f3d6ff6d9c0b00a09c1ad0e6524b`，包含官方 `5523172` 基线。
此轮只移除显式 Retire API；应用无旧调用，继续普通 Close、应用任务域和最后窗口关闭后保持运行。
本机独立 checkout、CI pin 与 Android Java/native 来源一致，未改框架、维护分支或扩展源码。

共享编译器新增明文/Base64 VMess JSON 节点列表；规则投影、对象目录、预览、主/次来源共用转换。
保留原订阅字节，不改下载地址、数据库或文件提交协议；坏条目、未知协议/字段整份拒绝，不保留合法前缀。
默认生成 selector/MATCH 记字段 approx，服务商非标准 net=tls 兼容为显式 TCP+TLS 并记节点 approx。
详细字段和界限见[节点订阅](node-subscriptions.md)，保真度基线与 README 同步。

Windows MSVC 执行 `cmake --build build --parallel 4` 全量通过，HUI DLL 与所有应用测试目标重建，
包含应用 HUI codegen；保留框架 renderer 的两处 C4244 和扩展已有 C4819 警告，未贴应用补丁。
`ctest --test-dir build --output-on-failure --no-tests=error -L clashflux-required` 为 26/28（193.47s）：
windows_service 报 Service pipe unavailable / Win32 2，windows_physical_dns_wfp 报 add WFP fixture sublayer；
本机令牌非管理员。这两项未验收，其余含 page_transition、singbox、persistence、CPS、真实 DNS/TLS
及内核运行回归通过。补强 HTTP/2/gRPC 字段和生成组重名测试后单独重跑 singbox，0.66s 通过。

用户给定订阅以当前应用 UA 只读获取到临时目录；通过生产 test_singbox --compile-config 后逐条比对，
46/46 节点的服务器/端口、UUID、alterId、TLS、Host/SNI 与 WS 路径全部保留，34 WS + 12 TCP/TLS 别名，
一个 selector 包含全部节点，默认不关闭证书校验。固定 sing-box 1.14.4 check 通过。
地址、凭据及生成配置不入仓库，没有启动这些节点或 VPN，未改用户数据库。
NDK 29 arm64/API 24 采用实际编译参数和新生成 Android Legacy core/header，完整 core 单元语法检查通过；
不是 Android 全量链接或 APK/设备验收，此前 SweetEditor Windows 归档路径限制仍未修复。

隔离原生夹具链接生产 HomeTotalCard、应用对象与新 HUI DLL，实际检查零流量 0%、非零 25%/75%，
以及关闭最后窗口后由应用任务重建新 HWND；不挂应用数据泵，不打开用户数据库/内核。
第二轮主题背景复核被用户物理 Esc 中止，立即停止 Computer Use，未将该轮记为通过。
`cmake --build build --target clash-flux`、`./run.ps1 --version`（v0.3.76）、`git diff --check`
及规范文件限额检查通过。未提交、打标签、推送或发布。

## 2026-10-29：统一 Release 安装包名称

代码快照：`54e96c8` 加本轮未提交工作区，版本保持 0.3.76。Windows 对外安装器由
`clash-flux-Setup-<version>.exe` 改为 `clash-flux-<version>-windows-x86_64.exe`，
与 ZIP、Linux/macOS 归档、Linux 包和 Android APK 的版本/平台/架构顺序一致。
同步 CI 暂存、收集、存在性检查和应用更新匹配；仅在新名缺失时兼容旧名，新名非法或重复不回退。
HUI/WiX 内部生成名称、旧 Bundle 清理及线上已发布资产未改，README 与发布技能同步。

Windows MSVC 实际执行 `cmake --build build --target clash-flux test_app_update test_app_update_async --parallel 4`
通过，HUI codegen 通过；`ctest --test-dir build --output-on-failure --no-tests=error -R '^(app_update|app_update_async|app_update_handoff|project_test_gate|ui_resources|persistence)$'`
6/6 通过（17.61s），含新旧名称、优先级、重复/非法拒绝及 URL 完全匹配回归。
28 个 required 测试注册且无禁用；本轮未运行全部 28 项。
隔离假产物执行实际 CI PowerShell 暂存与 WSL Bash 收集脚本，11 个发布文件名全部匹配，
缺少新名安装器时收集明确失败；该检查不运行 WiX 或真实安装器。
`./run.ps1 --version` 输出 v0.3.76，`git diff --check` 和发布技能行数限额通过。
未执行实际 CI、跨平台重打包、签名或安装；未提交、打标签、推送或发布。

## 2026-10-29：Android 应用内 APK 更新

代码快照：`54e96c8` 加本轮未提交工作区，版本保持 0.3.76 / Android versionCode 154，未签名、发布或安装。
Android arm64 接入已有 HUI 流式下载与应用级模型；应用作用域 Java PlatformModule 在工作线程
校验大小、SHA-256、正式证书、包名、versionName、递增 versionCode 和最低 SDK。
允许安装来源后保存配置、停 VPN/核心、冻结并排空持久化，通过私有只读 Provider 打开系统安装确认；
拒绝/取消保留 APK 和同一持久化会话供重试，系统负责进程替换。不卸载、不清除订阅或数据库。
Android Legacy 配置输入改为规范绝对路径，消除新增原生单元的 Ninja 重复输出；未改框架或扩展 fork。

实际检查：JDK 17 / SDK 36 下 Gradle `:app:testAppUpdatePolicy`、`:app:compileDebugAidl`、
`:app:processDebugResources`、`:HuxerUI:bundleLibCompileToJarDebug` 和 Camera Java 构建通过，
包含真实 Manifest 合并。新增 Java 平台模块、Provider、Policy 和 MainActivity 等 14 个业务单元
及真实生成的 AIDL 用 `javac --release 8` 编译通过；使用上述 HUI/libbox/R 类路径，隔离编译的
BuildConfig 常量取自实际 CMake integration plan 和 Gradle 版本；未编译无关 QrPhotoDecoderModule，
这不是完整 Gradle Java/APK 构建。

NDK 29 arm64/API 24 的新原生模块和下载暂存单元通过 `-Wall -Wextra -Werror` 语法检查。
`cmake --build build/android-update-arm64 --target clash-flux --parallel 4` 的全量构建在现有
SweetEditor 合并 simdutf 归档时失败：其脚本将 Windows `C:/...` 拼入 `temp_extract/C:/...`，
Windows 拒绝创建目录；`-k 0` 后仍被同一依赖阻塞。未绕过归档或修改依赖缓存冒充链接通过。
另按该构建的真实 Ninja compdb 命令单独编译六个受影响单元：src/app.cpp、下载暂存、Android
平台模块及 HUI codegen 后的 app/platform_app/settings_page，全部通过；真实资源也由 HRC 生成。
Android 全量链接/APK、正式签名跨版本覆盖、来源授权/取消和数据重开仍待验证；adb 当前无设备。

Windows MSVC 主目标与 HUI codegen 通过，版本命令输出 v0.3.76；针对 app_update、app_update_async、
app_update_handoff、page_transition、persistence、ui_resources、project_test_gate 的七项回归通过。
首次裸环境 project_test_gate 因 PATH 无 Ninja 失败，在 VS 开发环境单独重跑通过。
规范文件行数/根字符限额与 `git diff --check` 通过。桌面真实安装/UAC/Polkit 仍未执行。

## 2026-10-29：应用内下载与升级交接

代码快照：`54e96c8` 加本轮未提交工作区。应用内更新接入 HuxerUI 平台 HttpClient，
检查用 SendAsync、包下载用 SendStreamAsync；应用级模型/任务保留下载状态，支持进度、取消、
SHA-256 校验和失败重试。Windows x64 选择 Burn 安装器；Linux 按包数据库选择 DEB/RPM，
经 pkexec 请求原生授权。确认安装后隐藏交互窗口、保存配置并停止代理，冻结晚到写入并排空待写版本；
独立助手确认就绪后主进程退出，助手等待退出完成再安装。准备失败恢复原窗口及持久化会话。
Windows 助手复制到独占缓存，使用静态 CRT 和只读包锁；Linux 助手封存 memfd 快照后交给包管理器。
未改变应用版本、数据库 schema、依赖 pin 或服务信任边界；其它平台继续手动下载。

Windows MSVC x64 Release 实际执行 `cmake --build build --target clash-flux test_app_update test_app_update_async test_page_transition test_persistence --parallel 4`，主目标/助手/HuxerUI codegen 通过；
`ctest --test-dir build --output-on-failure --no-tests=error -R '^(app_update|app_update_async|app_update_handoff|page_transition|persistence|ui_resources|project_test_gate)$'`
7/7 通过（48.50s）。必跑门禁 28 项均注册且启用，Darwin 不要求 Windows/Linux 专属升级测试；
本轮未执行完整 28 项。`./run.ps1 --version` 在 PTY 中输出 v0.3.76，`git diff --check` 通过。
持久化回归包含真实 SQLite 写锁下冻结、晚到设置/增删/选中拒绝、失败恢复及三个来源/引用文件重开保留。

Fedora 44 WSL 使用 g++ 编译生产下载暂存/摘要代码、wire codec 与 app_update，0 failures；
独立助手以 `-Wall -Wextra -Werror` 编译并运行 Linux 交接回归，摘要拒绝、等待存活父进程和 sealed
快照拒绝写入通过。Windows 交接回归使用真实助手及无提权假安装器，验证父进程退出前不安装、
文件锁拒绝改写和退出后才运行安装器；未调用真实安装包、UAC、Polkit 或包管理器。

原生隔离夹具 `%TEMP%/clashflux-update-review-98a885661b014807835e42186ebe1d87/probe.cpp`
链接生产关于页与更新模型，将检查参数模拟为 0.3.75，经真实 HUI 平台请求下载 v0.3.76 安装器
36,812,935 字节，应用校验并进入 Ready；独立 SHA-256 复核为
`569404560c2418c902dbc3f114f282badcd76c224556f6153675316f59cab70c`，与 Release 资产一致。
使用电脑操作技能查看浅色 960×800 窗口的安装入口与确认框，点击确认后测试桩收到一次回调；
夹具随后关闭，未操作用户订阅/系统代理/服务，未安装真实 Release。
未覆盖真实跨版本安装/回滚、Linux 授权窗口、深色/窄屏原生验收、完整 Linux GUI 构建、
Android/macOS/iOS 构建、签名或发布；未提交、推送或发布。

## 2026-10-28：v0.3.76 发布验证

代码快照：`7fcc273` 加本轮工作区，准备提交并标记为 `v0.3.76`；HuxerUI 固定 `ce2a671832152bf2376d331cb81f20c59f0a6875`。
Windows/MSVC Release 全目标构建及 HuxerUI codegen 通过；完整 required CTest 25 项中 21 项首次通过，
`project_test_gate` 与 `cps_dependencies` 在补齐测试子进程 MSVC/Ninja PATH 后另行通过，合计 23/25。
`windows_service` 报 Service pipe unavailable (Win32 2)，`windows_physical_dns_wfp` 添加 fixture sublayer 失败；
需管理员服务/WFP 环境，留待 Release CI 确认。`./run.ps1 --version` 输出 Clash-Flux v0.3.76，`git diff --check` 通过。
本机未重复执行安装器跨版本升级、回滚、卸载或 Android 签名验收；等待推送后的 Release CI 及资产检查。

2026-10-28，基于 `98da670` 未提交工作区后续：按用户反馈将浅色中性表面从暖白微调为冷白，
统一 FluxLightColors：外框/标题/侧栏 #FAFAFB、内容 #F1F2F4、卡片 #FEFEFF，文字、描边与遮罩
改为配套冷中性色；深色和强调色不变。次要文字在三种主要表面的对比度最低约 4.71:1。
恢复 UI 对象的头文件依赖跟踪后重新编译全部应用 UI 与 page_transition 消费方，主目标构建通过；
page_transition/ui_resources 2/2 通过（23.09s）。隔离原生设置页夹具使用当前生产组件及新调色板启动，
通过电脑操作工具只读查看实际窗口，确认内容冷灰与亮卡片层次；未点击会写用户设置的控件。
`cmake --build build --target clash-flux test_page_transition --parallel 4`、`./run.ps1 --version`
（v0.3.74）及 `git diff --check` 通过。未做手机、其它平台或完整 required 门禁，未提交或发布。

2026-10-28，基于 `98da670` 未提交工作区：桌面设置 AppOptions.quit_on_last_window_closed=false，
轻量托盘改用正常窗口关闭，不再调用 fork Retire；普通模式仍隐藏窗口，最小化保留原生任务栏行为。
应用后台任务使用 UseApplicationTaskScope，显式退出保留清理后 Quit。当前固定 HuxerUI SDK 已含上游选项，
无框架 pin 或 fork 源码变更。隔离原生窗口夹具 `%TEMP%/clashflux-window-lifetime.cpp` 验证普通隐藏不卸载、
托盘不可用时拦截关闭、三次轻量关闭/重建、同一模型和无窗协程存活、显式 Quit，以及默认选项关闭末窗退出。
不把该夹具当成生产托盘菜单或 macOS/Linux 实机验收；当前仍保留轻量模式开关，移除开关的产品提议尚未实施。

同轮将桌面六平台资产、Android libbox 和 iOS 源码 pin 对齐 sing-box 1.14.4 / c005f353，
处理新内核拒绝重复策略组成员及 Android useProcFS 接口移除，更新保真度基线；详情与 AAR 摘要见
[稳定升级记录](singbox-stable-upgrade.md#2026-10-281143--1144)。Windows 主目标、HuxerUI codegen 与相关测试目标
构建通过，10 项 UI/编译器/真实内核回归全过（94.81s），版本命令 v0.3.74，打包内核 1.14.4 / c005f353。
构建缓存恢复时的 MSVC include 前缀乱码导致 .inc 增量依赖遗漏，已修复本地生成的编译器元数据前缀并重新配置，
强制重编生产 singbox.cpp 及测试消费方后回归通过，不修改工程工具链设置。
Android arm64 新 AAR 实际构建、来源/摘要 stage 和固定 GEO stage 通过；变更 owner 方法独立 javac 编译通过。
完整 Android Java 构建在 CMake 查找前置步骤失败，未产 APK；未验证真机 VPN、服务安装更新、其它桌面平台、
iOS 或完整 required 门禁。未提交、推送或发布。

2026-10-28，接续下方关于页工作，基于 `98da670` 未提交工作区：重启后 cl/mspdb 组件已统一为 36260，恢复原 Release/health-evidence/required 测试配置，重新配置与主目标/HuxerUI codegen 构建成功。关于右侧箭头已移除，公共桌面返回按钮左侧留白改为 8pt。修复更新检查使用临时 State 导致重组重置的问题，改用 UseState；修复响应式重建后滚动位置丢失，记录用户滚动偏移并在新容器挂载时恢复；宽屏关于实际限宽 640pt 并居中。用户反馈二级页直接切换首页崩溃，日志为 shared keys 重复；首页翻面标记现按卡片稳定 ID 唯一，回归包含二级/嵌套页直接切换到多张真实 HomeCardSurface 的首页。最终执行 `cmake --build build --target clash-flux test_page_transition --parallel 4` 成功，`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'` 2/2 通过（12.32s），`./run.ps1 --version` 为 v0.3.74，`git diff --check` 通过。
生产组件临时夹具 `%TEMP%/clashflux-profile-form-review/about_probe.cpp` 在深浅色、320/599/600/960pt 共 8 组合通过：可滚动上下留白、无尾箭头、宽屏 640pt 居中、跨断点页面身份/返回及设置原滚动位置；刷新图标持续旋转、完成停止、reduced motion 静止；真实页面在测试运行时显示请求失败并可再次检查。未把该测试运行时结果视为真实 GitHub 联网成功证据。电脑操作工具恢复后只读查看了用户已打开的主程序暖白设置页；隔离原生夹具补齐同名资源包后成功启动，后续用户按 Esc 停止电脑操作，不再操作窗口。最终产品首页切换与新按钮位置的自动原生 GUI 验收未完成；未做 Android/其它平台或完整 required 门禁，未提交或发布。

2026-10-28，基于 `98da670` 未提交工作区后续：设置及关于页通过 ScrollablePageContent 将 16pt 顶部/24pt 底部留白放入滚动内容，保留水平缩进与 Compact 胶囊避让；设置末尾简化为关于入口，新增独立关于页（Logo、版本、简介、仓库、免责声明及只读稳定 Release 更新检查）。更新 codec 与数字版本比较、错误/非稳定响应和 Release URL 校验已加入 page_transition。初版 Windows 主目标/HuxerUI codegen 构建通过，page_transition / ui_resources 2/2 通过（15.47s）；生产临时夹具尚未完成全组合验收，不计为通过。
用户随后要求刷新图标在检查期间旋转、GitHub 仓库入口使用 GitHub 标记，以及修正宽屏关于标题栏；代码已接入保留 Rotation（reduced motion 停止）、带 MIT 许可的 Octicons 资源及公共桌面 PageScaffold。用户选择温润暖白，统一 FluxLightColors 为外框 #FBFAF7、内容 #F4F2EE、卡片 #FFFEFC，保留强调色与深色配置；次要文字/内容背景对比度约 4.81:1。
最终验证被本机 Visual Studio 更新阻塞：用户要求尝试不重启，重新加载开发环境并配置时，最小 C 编译即报 C1902；实查 cl/c1/c2/mspdb140 为 36260，mspdbcore/mspdbsrv 仍为 36252。适配 SDK 路径时 CMake 自动清理缓存，已从 build/CMakeCache.before-hui-startup.txt 恢复原构建设置并补回 Release、health-evidence 输出、required 测试 ON、原 OpenSSL/Python 路径；原 VS Ninja/26100 SDK 路径恢复可用，下一步需工具链完成更新后先重新配置再构建。最新旋转/GitHub 图标/标题栏/暖白改动尚未完成主目标编译或 GUI 验收，不以初版的通过记录替代。最新字符串检查覆盖 414 个文案、GitHub SVG XML 检查及 git diff --check 通过；run.ps1 --version 为已有产物 v0.3.74。未重启系统、未提交或发布。

2026-10-28，基于 `98da670` 未提交工作区后续：设置分组标题统一移到卡片外并按内容首列对齐；组面板零内边距，行内保留空间，首尾行贴边，由整组圆角裁切绘制和命中。共享设置行通过 SettingsRowInsets 仅在设置组内取得行距，订阅表单的默认几何保持中性。Toast 使用现有 HUI 能力统一下移 8pt、缩到 98%、200ms 淡出，减少动画时禁用位移/缩放；用户明确不改 fork，未实现上下独立方向。窗口级订阅错误/刷新失败提示移到主题容器内取得 Toast，并随样式变化更新唤醒绑定。
桌面 Compact 胶囊/渐隐层移入 NavigationStack 的首页层 `CompactRootPageSurface`，语言/主题/报告/规则编辑等二级页直接覆盖父页；移除 secondaryOpen/secondaryOwner 与各路由的手工显隐/清理，返回保留同一胶囊挂载状态。`UsePrimaryPageNavigation` 在一级选择改变时清理逻辑二级路径；订阅内联表单的展示状态与 push/pop 独立。用户已初步手工认可导航行为。
Windows MSVC x64 Release 执行 `cmake --build build --target clash-flux test_page_transition --parallel 4` 与 HuxerUI codegen 通过；`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'` 最终 2/2 通过（11.07s）。新增回归覆盖 Toast 自动/主动退场的真实位移、缩放、透明度、卸载/静止和 reduced motion；胶囊 push/pop 的挂载保留、返回过程中状态可用、二级页稳态覆盖、嵌套与跨断点、一级切换清栈。回归不将被导航覆盖父页的逻辑 visible 标记等同于冻结过渡画面的逐像素验收。
生产临时夹具 `%TEMP%/clashflux-profile-form-review/settings_group_probe.cpp` 在深浅色、单/多行、320/400/960pt 与鼠标/触屏共 24 组合验证标题外置/缩进、首尾无空带、边缘点击和圆角外拒绝，并覆盖虚构根级错误 Toast 消费/展示/退役；语言夹具 8 组合再次通过，仅检查打开/返回，无数据库写入。未执行本轮完整产品原生动画验收、Android APK/实机及完整 required 门禁；未提交或发布。

2026-10-28，基于 `98da670` 未提交工作区后续：首页出站模式移除手绘单选圈/整行状态层，生产 `HomeModeChoices` 使用框架 RadioButton，与语言单选同一行为，文字区域可选、32pt 状态层围绕圆圈；保留 18pt 圈和首页紧凑布局。MD3 通用反馈在强调色写入后统一重建，hover/focus/press/ripple 透明度归 theme_colors.h 常量；填充按钮保留 on_primary 对比角色，图标按钮/Chip/Tabs/分段/菜单/Select/对话框反馈从统一 helper 取色。
语言入口改为跨平台响应式：320/599pt 进共享 LanguageRoutePage，600/960pt 开弹窗，共用真实 LanguageChoices；桌面路由生命周期驱动二级页及底栏状态。Windows MSVC x64 Release 的 `cmake --build build --target clash-flux test_page_transition --parallel 4` 与 HuxerUI codegen 通过；`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'` 2/2 通过（10.91s），新增深浅/蓝绿强调色、鼠标/触屏下的实际局部圆形反馈、无整行半透明反馈、标签点击单次更新及 busy 拒绝回归。
临时夹具 `%TEMP%/clashflux-profile-form-review/responsive_language_probe.cpp` 链接真实入口/选择页，在深浅色与四种宽度共 8 组合验证页面/弹窗、跨宽窄保留页面、返回与导航所有权清理；只打开和关闭选择器，不选择语言、不写用户 store。用户手动验证并认可反馈效果。自动 GUI 工具因 kernel assets 路径错误未能初始化，未将自动组件回归计为原生 GUI 验收；本轮未做 Android APK/实机或其它平台构建，未提交或发布。Toast 消失动画尚处方案讨论，本轮未修改 Toast。

2026-10-28，基于 `98da670` 未提交工作区后续：用户确认全宽反馈示例后，设置页主题/语言/入站端口/运行诊断和手机及桌面 Compact 的更多入口共用全宽行；标题、说明与非整行控件维持原对齐。整行仍读取 Material 基线的 theme.interactions.indication，本次未修改涟漪配色；应用仅替换 colors 不会自动重算这组拥有型反馈颜色。
Windows MSVC x64 Release 执行 `cmake --build build --target clash-flux test_page_transition --parallel 4`，主目标及 HuxerUI codegen 通过；`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'` 2/2 通过（10.02s）。生产组件临时夹具覆盖默认入口与显式留白入口，在深浅色、320/400/960pt、鼠标/触屏左右边缘共 48 次点击均恰好触发一次，行宽与卡片相同、文字保持原对齐。未做本轮完整产品 GUI 或 Android APK/实机验收；未提交或发布。

2026-10-28，基于 `98da670` 未提交工作区：设置页仅「网络 → 运行诊断」试用全宽点击/Indication。网络卡片的水平留白下放到标题和各行；诊断行的留白位于可交互 Row 内，其它行保持原位置和范围。共享 Card 的可选 contentPadding 默认仍取主题值，不改其它卡片。
Windows MSVC x64 Release 执行 `cmake --build build --target clash-flux test_page_transition --parallel 4`，主目标/HuxerUI codegen 通过；`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'` 2/2 通过（10.03s）。临时夹具 `%TEMP%/clashflux-profile-form-review/settings_row_probe.cpp` 链接生产 SettingNavigationItem/SettingsSectionCard，在深浅色、320/400/960pt 验证整行与卡片同宽、文字对齐不变，以及鼠标/触屏左右边缘共 24 次点击均恰好触发一次；不连接数据库或网络操作。未做完整产品原生窗口效果或 Android 实机验收，等待用户确认这一个示例的视觉效果后再推广；未提交或发布。

2026-10-28，`v0.3.74` 发布候选：汇总手机订阅统一列表/紧凑卡片、手机代理原名、策略组卡片选中态、窄屏固定底部保存与桌面窄窗编辑导航修复、首页双击翻面彩蛋，以及订阅更新失败提示。版本为 CMake `0.3.74`、Android `versionName=0.3.74` / `versionCode=152`。
翻面后由背面独立命中层隔离正面控件，覆盖显式 Enabled(true) 子按钮原位置的单击不触发、双击翻回及控件状态保留。翻转加强为 0.62 秒，包含抬起/侧向弧线/2.5pt 侧边与 retained 明暗变化，减少动画时直接切面，静止后不持续调度。Windows MSVC x64 Release 执行 `cmake --build build --target clash-flux test_page_transition --parallel 4` 与 HuxerUI codegen 通过；`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'` 2/2 通过（24.98s），覆盖深浅色、鼠标/触屏、减少动画、实际绘制中间帧、背面原控件位置输入和状态保留。`./run.ps1 --version` 输出 v0.3.74，`git diff --check` 通过。
构建曾被运行中的旧 exe 占用，保留旧进程、将原产物移至同目录备份后重新链接通过。Windows 入口中开发机绝对路径的临时 terminate 日志改动保留在工作区，不纳入发行提交。本机未执行本轮完整 required 门禁、原生窗口动画验收、Android 真机/签名包或其它平台构建；tag 发布流水线仍需完成跨平台门禁、安装包及 APK 签名检查。

2026-10-28，`586d1e2` 未提交工作区后续：手机订阅页改为跨来源的单列虚拟列表，不挂类型标签或类型 Pager，搜索与拖放沿用共享工厂；桌面窄窗仍按类型分页。手机代理显示名使用来源对象目录的原始名称，不附订阅名，不截断用户名称中的分隔符，运行 tag/选择/测速目标保持原值；桌面继续限定来源。策略组下拉选择器直接复用 `SelectableTile`，其实现抽到 `selectable_tile.cpp` 并同步桌面测试和 Android 兼容源清单，统一选中底色、文字、描边、圆角和交互。
首页普通卡片新增鼠标双击/触控双击翻面彩蛋，再双击翻回；背面暂为空白。局部 SharedBounds 过渡按竖直中线收窄后展开，持续 0.46 秒，不改变 HomeGrid 占位；正面保留挂载状态，背面时禁用并隐藏正面输入/无障碍内容。每卡片状态独立，启动/运行猫咪控制卡排除，原有单击、按压反馈及控件行为保留；reduced motion 直接切面，完成后不持续调度帧。
Windows MSVC x64 Release 执行 `cmake --build build --target clash-flux test_page_transition --parallel 4`，主目标与 HuxerUI codegen 通过；`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'` 为 2/2 通过（41.05s）。新增回归覆盖混合来源原顺序/跨类型搜索/空结果、桌面类型顺序、手机 320/400/960pt 无分类 Pager/边距/滚动保留、代理原名和桌面来源限定、深浅色选择器的紧凑布局/选中语义/单次关闭，以及鼠标/触屏翻面、实际绘制的侧边中间帧、布局稳定、正面控件状态保留、独立卡片、猫咪排除和静止后停止动画。
临时 CMake 脚本 `%TEMP%/clashflux-android-mobile-ui-review.cmake` 生成 Android 兼容源，确认统一列表包装及共享可选卡片源已接入。`./run.ps1 --version` 输出 v0.3.73，`git diff --check` 通过。以上为 Windows 构建/无窗口组件回归与 Android 兼容源生成，没有执行本轮 Android APK/真机、完整产品原生窗口动画或完整 required 门禁；未提交、推送或发布。

2026-10-28，`586d1e2` 未提交工作区后续：Compact 订阅卡统一 88pt、行距 6pt；卡片、虚拟网格和拖动预览共用 `profile_card_layout.h` 的尺寸，预览取来源槽位宽度并补偿标题行中手柄的居中位置。窄屏隐藏链接、刷新按钮、描述、更新时间和自动更新频率，名称按实际宽度省略，参与状态移到标题行；菜单仍提供更新，刷新中保留忙碌提示。流量/到期文字缺失时保留空槽位，无流量总额时进度条保持满格；宽屏维持 180pt 及原信息。PPTP 同高并保留连接状态/开关，Windows 单连接说明进入编辑表单。
Windows MSVC x64 Release 执行 `cmake --build build --target clash-flux test_page_transition --parallel 4`，包含 HuxerUI codegen；`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'` 两项通过。新增生产紧凑布局回归覆盖四种流量/到期组合的同高/槽位、长名称与按钮避让、宽窄往返、菜单点击及百条虚拟滚动；空字符串不创建 Tooltip，防止占位触发框架校验。临时夹具 `%TEMP%/clashflux-profile-form-review/profile_cards_probe.cpp` 链接真实应用对象，在中英文、深浅色、忙/闲、remote/local/PPTP、四种元数据及 320/400/599/960pt 下共 384 个组合通过，检查真实卡片高度、进度条边界及窄屏隐藏内容；夹具使用虚构数据，不访问用户订阅或内核。
`./run.ps1 --version` 输出 v0.3.73，`git diff --check` 通过。默认高并行曾因 MSVC C1060 内存不足失败，降低并行度后重编通过。未执行完整 required 门禁、完整产品 GUI/原生拖动或 Android 真机验证；未提交、推送或发布。

2026-10-28，`586d1e2` 未提交工作区后续：桌面 Compact 的订阅新建、编辑信息与编辑文件页统一由实际显示的 `IndexedPages` 表单驱动底部导航/渐隐层的隐藏；返回、保存关闭、一级页切换、跨宽窄断点和卸载时同步恢复。独立表单状态不再借用 NavigationStack 的 route 所有权，避免宽屏切走后留下新建页的导航隐藏标记。
Windows MSVC x64 Release 执行 `cmake --build build --target clash-flux test_page_transition`，主目标与 HuxerUI codegen 通过；`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'` 为 2/2 通过（12.49s）。新增无窗口生产导航状态 hook 与响应式壳层回归覆盖三种表单、320/400/599/960pt 往返、返回恢复、一级切换、卸载/重挂及独立 route 状态保留。`./run.ps1 --version` 输出 v0.3.73，`git diff --check` 通过。首次构建因临时 OpenSSL 前缀缺失失败，随后从本机 vcpkg 缓存恢复，两个库文件 SHA256 与原 CPS 外部输入记录一致；没有更改依赖配置或版本。
本轮未执行完整 required 门禁、完整产品 GUI 或 Android 真机验证；当前 GUI 仍为旧进程，完全退出并重启后才能使用本次构建。未提交、推送或发布。

2026-10-28，`586d1e2` 未提交工作区：修复桌面窄屏打开「编辑信息」的终止异常。共享 `ProfileFlowPage` 不再携带只允许用于 `NavigationStack` 页面根的 `PageTransition`；编辑信息/文件的导航 route 单独添加动画，`IndexedPages` 直接挂载的表单不添加。所有共用底部创建/保存按钮使用 `ui.h` 的 `kCompactPrimaryActionHeight`（40pt），布局回归引用同一常量，表单独立滚动且按钮保持在底部安全区上方。
Windows MSVC x64 Release 执行 `cmake --build build --target clash-flux test_page_transition`，包含 HuxerUI codegen；`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'` 为 2/2 通过。`./run.ps1 --version` 输出 v0.3.73，`git diff --check` 通过。独立夹具链接真实应用对象、使用虚构表单数据，在旧对象上复现相同 PageTransition 异常；修复后 320/400/599pt 下的编辑信息/文件 IndexedPages 与导航 route 共 36 个组合打开/返回、40pt 按钮实际几何及底部位置检查通过。夹具位于本机 `%TEMP%/clashflux-profile-form-review`，未读写用户数据库、订阅或内核。
本轮没有运行完整 required 门禁、完整产品 GUI 或 Android 真机验收；现有 GUI 仍使用旧进程，需要重启后使用新构建。未提交、推送或发布。
2026-10-28，同一 `d43769c` 未提交工作区后续：订阅规则在所有视口采用相同布局，一行来源/查看模式/更多工具栏，规则首行显示类型、匹配内容和出口，次行显示状态与插入位置。来源按名称占宽并封顶 240pt，查看模式与操作预留空间；移除右侧固定状态列，英文模式标签缩短。单行文本在最终分配宽度内省略，保留完整提示与语义标签；仅在内容/样式或宽度变化时重新测量，不写逐帧 State、不改框架。JSON 编译预览在 Compact 进入独立二级页，使用共享只读文档填满剩余高度并提供复制，桌面非 Compact 保留弹窗。
Windows 执行 `cmake --build build --target clash-flux test_page_transition`；Linux 执行 `cmake --build /tmp/clashflux-refresh-clang21-20261028 --target clash-flux test_page_transition --parallel 8`，主目标和 HuxerUI codegen 均通过。两端执行 `ctest --test-dir <build> --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'`，均 2/2 通过（Windows 15.17s，Linux 6.25s）。新增生产布局回归覆盖中英文、深浅色、320/420/599/600/960/1600pt 和缩放返回、长来源名/匹配/出口省略、控件避让、来源与模式选择、操作点击、保存忙态及千行虚拟列表的真实滚动保留；共享只读文档增加 320pt 短窗/长窗与 960pt 往返时的剩余高度、文档身份和全文保留检查。
Windows 在独立临时原生窗口挂载生产工具栏与规则内容，检查 960pt/320pt 的单行工具栏、长文本省略和操作位置；只使用测试数据，不接触安装版数据库或内核。完整应用的 JSON 二级导航与真机移动端尚未手工验收；没有将组件预览计作完整产品 GUI、VPN 或发布验收。两端版本检查输出 v0.3.72，`git diff --check` 通过；Linux 仍按下述方式仅在内存中规范化 `run.sh` 的 CRLF。未执行完整 required 门禁，未提交、推送或发布。

2026-10-28，同一 `d43769c` 未提交工作区后续：订阅规则来源和卡片「编辑规则」共用应用级可编辑来源清单，沿既有工作线程的有界格式识别读取一次前缀，保留 Clash YAML / 原生 sing-box JSON，过滤 PPTP、OpenVPN 和没有可读来源文件的记录。选择失效时回到可编辑主来源或首项；全部不可编辑则仅显示空状态，不放无效选项、不展示不支持编辑的占位行，也不改全局规则的连接目标。
Windows 执行 `cmake --build build --target clash-flux test_page_transition`，Linux 在下述 Clang 21 临时构建目录执行同名目标，主目标与 HuxerUI codegen 均通过；两端 `ctest --test-dir <build> --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$'` 均为 2/2 通过。新增应用生命周期内的模型回归覆盖分类未就绪、PPTP/非可编辑文件过滤、JSON 保留、稳定 ID/顺序、默认主来源、保留有效选择、删除后的回落和仅剩不支持来源的空状态。版本检查仍为 v0.3.72，`git diff --check` 通过；未做桌面 GUI/真机 VPN 验收，未提交或发布。

2026-10-28，基于 `d43769c` 的未提交工作区：手动刷新按订阅 ID 共享忙态和完成版本，只有该订阅的规则内容显示骨架；切到其它已缓存订阅直接显示其快照，全局规则、规则集和来源选择器独立加载。应用级任务覆盖切页、重复点击、异常重试及旧版本完成回调；规则快照按 ID 缓存，更新结束后等待匹配版本的新快照。
订阅 HTTP 下载在独占暂存中等待，不持有运行计划锁；候选检查前重新核对来源 URL、文件、下载选项和原内容，保留最新选择/名称/排序。检查至文件替换、运行交接与落库仍持锁；来源已编辑/删除时拒绝迟到结果，不恢复旧行或覆盖新文件。
Windows MSVC x64/Release 执行 `cmake --build build --target clash-flux test_page_transition test_persistence`，主目标与 HuxerUI codegen 通过；`./run.ps1 --version` 返回 0，输出 v0.3.72。
`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources|persistence)$'` 为 3/3 通过；新增回归覆盖 A/B 独立骨架、切到 B 缓存、发起视图真实卸载、并发操作互不清忙、失败旧内容/重试和版本 ABA。
Linux 在 Fedora 44/WSL 的独立 `/tmp/clashflux-refresh-clang21-20261028` 中，以临时解压的 Clang 21.1.8、系统 libc++ 22 和 CMake 4.4.2 构建；未改系统默认编译器、应用依赖 SHA 或框架源码。`cmake --build /tmp/clashflux-refresh-clang21-20261028 --target clash-flux test_page_transition test_persistence --parallel 8` 通过，同一 CTest 筛选为 3/3 通过。
`python3 tests/test_profile_selection_runtime.py /tmp/clashflux-refresh-clang21-20261028/clash-flux` 通过：真实 Runtime、SQLite 和固定 sing-box 下覆盖单候选、候选失败保留、慢响应期间另一来源选择先完成、最新选择保留、文件编辑/明确删除后迟到下载拒绝，以及其它 ID/文件与停止态配置保留。
Linux 使用显式 `CLASHFLUX_BIN` 验证 v0.3.72；当前 Windows checkout 的 `run.sh` 带 CRLF，直接执行报 `bash\r`，因此只在内存中规范化 LF 后执行同一脚本，未修改脚本。`git diff --check` 通过。
工具链边界：本机 Clang 22.1.8 在未改动的 `pptp.cpp` 发生前端内部崩溃（退出 132，O0 诊断亦复现）；GCC 16.2.1 在 `home_page.cpp` 消费 `profile_rules.h` 时报告 DTO 重复声明，两次构建均未计为通过。Linux 持久化独立目标亦在 Clang 22 下通过；最终应用与竞争回归采用项目 CI 的 Clang 21 主版本。Windows CLI 隔离回归因当前会话已有安装版占用固定单实例互斥而在首次 `profile list` 超时，未关闭该安装版；该回归在 Linux 隔离数据目录完成。
本轮未执行完整 required 门禁、Linux 实机 TUN/桌面 GUI、SCM/VPN 交接、Android/macOS/iOS 构建或签名发布验收；没有提交、推送或发布。行为约定见 [UI 开发约定](ui-development.md)。

2026-10-26 `v0.3.70` Windows 发版候选实机验证：本地生成的安装包从 0.3.69 静默升级到 0.3.70，安装器返回 0，
但升级后 `ClashFluxService` 被旧 MSI 删除。MSI 日志显示卸载中的 0.3.69 包即使设置了 `UPGRADINGPRODUCTCODE`、
且 `ServiceCleanup` 组件 Action 为 Absent，仍执行 Stop/Delete ServiceControl。通过新安装的程序显式重装服务后恢复运行；
数据库完整性及 profiles/settings 逻辑摘要未变化。真实失败回滚和静默卸载未验收；该已知问题已写入发布说明。
日志和数据库快照保存在本机 `%LOCALAPPDATA%\ClashFlux-InstallValidation\v0.3.70-20261026`，不随提交发布。

2026-10-26 规则集剩余功能（v0.3.70 发布快照）：全局规则现在可引用共享规则集，
显示名称而持久化稳定 ID，规则层级/出口/不可用策略保持不变；删除保护覆盖全局和订阅的禁用记录。
桌面可导入本地 SRS，经固定内核有界反编译为独立 inline JSON；源文件不改，失败保留表单/旧库。
桌面远程集合可显式重新下载：轮换单资源原生缓存身份，经候选检查和已有重启/恢复路径应用；
未运行或未引用仅排队，不伪报下载成功。本次操作回执由应用模型持有，不冒充原生周期更新时间。
Windows MSVC x64/Release 主目标及 UI codegen 通过；HuxerUI 固定到 `3a6c18e`；版本检查输出 v0.3.70，git diff --check 通过。
定向 CTest `singbox|routing|vpn|persistence|page_transition|ui_resources|project_test_gate|runtime_health|native_control_runtime|personal_rule_sets_runtime`
10/10 通过。新增 personal_rule_sets_runtime 纳入必跑门禁；本机 25 项 required 均已注册/启用，
直接 ORM 仍按已有 MSVC C4737 硬错误例外跳过，未执行全部 25 项。
固定 sing-box 1.14.3 的隔离运行覆盖 11 个场景：本地 inline、全局引用、生产 SRS 导入、远程 JSON/SRS、
同身份缓存复用、强制获取新内容、坏内容强制更新拒绝、离线新缓存、旧缓存恢复和 TLS 证书拒绝。
生产导入函数另覆盖 Unicode/空格路径、畸形/缺失/超限文件、原文件保留与独占暂存清理。
实际 GUI 在独立测试库验证全局选择/保存、本地 SRS 文件选择和 JSON 编辑/保存、未引用远程更新排队，
以及 1080→500pt 窄窗、独立全局编辑页；退出后只读重开 SQLite 核对稳定引用、SRS 内容和更新代次，
两条订阅和源文件均保留。修正运行夹具提前流水发送 SOCKS/HTTP 的时序问题后，运行回归与 CTest 通过。
手动证据在本机 LocalAppData/ClashFlux-RuleValidation/20261026/remaining-runtime-corrected；GUI 使用 remaining-gui。
测试应用和本次原生工具/loopback 服务已退出，没有修改真实用户库或启用 TUN/系统代理；真实 SCM 运行交接、
Android/Linux/macOS 构建/设备、原生周期刷新与完整平台门禁未验收。手动更新及 SRS 文件导入暂只对桌面开放。
该轮结束时没有提交或发布；功能契约见[订阅个人规则](subscription-personal-rules.md)。
收尾另限制旧 GEO EOF 自愈：只在全部文件资源可归属编译器清单时修复自有 GEO 文件，
不删除共享原生 cache.db；生产归属判定覆盖个人远程、订阅本地、inline 与自有资源组合。

2026-10-26 订阅个人规则与规则集资源库（v0.3.70 发布快照）：
个人修改绑定 profile ID，直接编译 sing-box JSON；规则页增加「订阅规则 / 全局规则 / 规则集」，
共享资源库支持远程 JSON/SRS、本地导入/手写 JSON，以及个人规则引用。资源改名保留稳定 ID，
启用引用才加载，删除保护包含其它订阅与禁用记录；远程 URL/格式改变使用新的原生缓存身份。
Windows MSVC x64/Release 主目标及 HuxerUI codegen 构建通过；定向 CTest
`singbox|routing|persistence|page_transition|ui_resources|project_test_gate` 6/6 通过。
固定 sing-box 1.14.3 的隔离运行回归通过本地 inline、远程 JSON、远程 SRS、服务端离线后 SRS
缓存复用，以及不受信任 TLS 证书拒绝；每个成功场景同时核对集合命中拦截与无关流量直连。
实际运行发现并修正 HTTP client 绕到空 direct outbound 的启动期错误，配置 check 单独未能发现。
回归脚本为 `tests/test_personal_rule_sets_runtime.py`，当前手动执行，尚未注册到 CTest。
隔离 GUI 验证新增本地 JSON、个人规则引用/REJECT 保存、切源隔离，以及桌面宽/窄窗、独立编辑页；
退出后重开 SQLite 核对资源/引用已落库、另一订阅未变、远程订阅原文与受控刷新文件逐字节一致。
真实用户数据库、订阅和 VPN 数据面未改动；Android/Linux/macOS 构建与实机、运行期定时刷新、
手动强制刷新、资源状态展示、全局规则集合引用及本地 SRS 导入未验收或尚未提供。
详见[功能与持久化边界](subscription-personal-rules.md)。

2026-10-26 `v0.3.69` 本机真实升级：0.3.68 → 0.3.69 安装成功，旧 Burn Bundle 迁移及订阅数据保留通过；旧 MSI 卸载阶段删除服务，升级服务保留失败，随后恢复服务。真实卸载、中途失败回滚和 VPN 数据流未验收；完整机器专属证据保留在本地，不随发布提交。

2026-10-26 Windows 安装器修复：应用 `ac301e5` 加工作区、框架 `6ebc621` 加隐藏 HWND 修复，在独立源码快照和 MSVC x64/Release 下完成主应用与安装器干净构建，版本命令返回 v0.3.68/0；框架公共头文件检查、4 个通用套件和安装器 10 个用例的 88 条断言均通过。原增量目录的 Runtime 视口异常在干净构建中未复现。应用 `legacy_bundle_cleanup`、`project_test_gate`、`page_transition`、`persistence`、`ui_resources`、`cps_dependencies` 六项通过；配置测试在正确加载 MSVC/Ninja 环境后重跑通过。

应用独立 checkout 固定到 `abd07a3f19d2680ade6093075608f73b42a9b3a3` 后，主目标和版本检查通过。原增量目录中部分框架对象仍早于变更后的公共头文件，分页和持久化测试异常退出；保留目录与源码、按现有 MSVC 编译命令重编译框架及测试库对象，再由常规构建刷新依赖和链接后，上述六项回归全部通过。

WiX 成功生成包含清理 DLL 的 MSI，安装表确认清理为 commit、Impersonate=no，且排除卸载；DLL 仅依赖系统 DLL。独立 per-user 测试产品的 quiet 安装、升级时 embedded 卸载及最终 quiet 卸载均退出 0，结束后无测试注册、依赖 Provider 或标记文件。升级日志含 Burn 重复调用已删除旧测试缓存的非致命 0x80070003，未视为无警告验收。真实 Clash-Flux 安装记录、缓存与用户数据未修改；历史迁移范围见 [HuxerUI fork](huxerui-fork.md#windows-legacy-bundle-migration)。

框架 SDK 完整安装被测试配置的 HUXERUI_ENABLE_PROFILING=ON 检查拒绝，未重配为发布 SDK，不能宣称 SDK 安装通过。真实 Clash-Flux 跨版本升级、失败回滚、签名发布和其它平台未执行。框架修复已发布到 farna/main 的 `abd07a3f19d2680ade6093075608f73b42a9b3a3`，应用 CI 和独立 checkout 同步固定该 SHA；应用版本仍为 0.3.68，没有发布新版本安装包。

2026-10-25 Android 编译产物复用（`159e674` 加工作区）：把运行参数规范化前移至
规则集发现前，统一保留 TUN 宽松路由和 IPv6 设置，移除检查回调与启动计划的无条件
完整重编译。无 HTTP 依赖与固定快照各需一次发现编译；普通单轮 HTTP 新快照仍需
发现与展开两次编译，ASN 多轮发现仍按新增输入重编译；拒绝检查、候选提交和后台
libbox 校验边界保留。增加原生准备与 Java
libbox 启动的分阶段计时；桌面 GEO 预取变化及目录变化仍触发重编译。
回归核对完整 JSON/保真度相等、编译次数、失败保留快照；实际验证结果见本次记录，
没有手机性能测量，不把减少调用次数写成真机加速倍数。

本批已执行 Windows `cmake --build build --target clash-flux test_singbox
test_rule_provider_download`；`ctest -R '^(singbox|rule_provider_download)$'
--output-on-failure --no-tests=error` 两项通过，下载回归带明确 OpenSSL 路径及
`--require-tls`。Android NDK 28.2 的 arm64/API24 C++23 语法检查通过，覆盖生产
vpn/singbox/rule_provider_cache 接口的兼容转换及平台规范化 helper；不等同于完整 APK 构建。

2026-10-25 多客户端网络冲突检查（`159e674` 加工作区）：桌面候选准备/应用前只读
检查活动 TUN 接口、双地址族网段重叠及默认路由接管；自身豁免绑定已鉴权运行实例。
Android 在 VPN 授权及建隧道入口检查系统所有权，Windows 服务补真实退出码和脱敏分类。
Windows/Linux 只读 OS 与判定用例、Android SDK 编译及所有权判定已执行；macOS、
Android 真机双客户端尚未验收，iOS 继续暂缓，自动选址与独立 DNS 冲突判定仍待办。
管理员服务回归及剩余边界见[实施记录](windows-tun-coexistence-plan.md)。

2026-10-25 Windows/Linux PPTP 对齐（`a826169` 后的未提交工作区）：两端共享
候选拒绝后的运行计划保护及断开确认；Linux 服务补三态会话证据与真实传输地址回传。
Windows 拨号捕获租约按会话暂存/提交/释放，准备或路由应用失败保留旧租约。
隔离回归覆盖真实 Linux 子进程/loopback 接口、共享租约事务失败/异常与计划撤出门禁；
详细变更及平台验证边界见 [PPTP 交接](native-pptp-handoff.md)。本批无真实 PPTP
服务器、Windows SCM/RAS、macOS 或手机验收，不扩展平台协议支持。
本机 Linux 已执行 `cmake --build build --target clash-flux`、`./run.sh --version`
（v0.3.61）、全部测试目标构建与 `ctest --test-dir build --output-on-failure
--no-tests=error -j 2`（21/21），`CLASHFLUX_REQUIRE_PROJECT_TESTS=ON`，OpenSSL 为
`/usr/bin/openssl`；`git diff --check` 通过。Windows 编译和专属 SCM/RAS 回归未执行。

2026-10-25 原生控制与运行健康联合补齐：基于 `1a1659b` 加未提交工作区，
吸收 `d3a3813` 草稿中的原生 gRPC-Web 及 codec/固定内核回归；启动确认改用
原生版本/实例身份，兼容接口与业务目标仍分别采证。PPTP IPC/RAS 状态未知
保留会话，确认断开才自动清理；修复退出检查发布竞态、通知消费者/复制异常
卡住推送、并发观察者 ID 与通道 Reset 旧代次复用。Android 新代次清除旧统计能力。
桌面完整构建与 HuxerUI codegen 通过；20 项普通 CTest 和 2 项管理员服务/WFP
回归通过，Java RuntimeHealth 编译/回归通过。新增原生控制与 Windows DNS/WFP
进入必跑门禁（本机 MSVC 已记录 ORM 豁免后 22 项）。
已正常退出旧版、更新受保护服务并运行新构建，完成“双辽”真实拨号与三分钟稳定观察：
340 次原生实例采样无缺失且实例不变，12 次经实际 mixed 入站的目标请求均为 HTTP 204。
首次拨号 6908 ms、旧内核运行期间准备 1840 ms、交接到控制器就绪 3019 ms。
断开验收暴露订阅卡“断开中…”缺少资源而退出，现已补齐中英文/默认资源，
增加中英文及默认语言回退的真实资源包挂载回归。内网目标未提供，不能据此确认内网可达。
完整边界与实机最终结果见 [运行健康实施复核](runtime-health-review.md)。

2026-10-24 Windows PPTP 工作区与远端运行健康功能合并：快进至 `1a1659b`，
保留本地未提交的 PPTP 拨号期间主 TUN、物理 DNS 原生出口和配置预备/交接实现。
`resumeNativeRouting` 的交叉修改同时保留健康暂停与拨号前实际状态恢复；
远端 runtime epoch、状态证据、流修复和诊断 UI 均保留。
修复新增健康目标在 Windows 下的头文件 `max` 宏污染。
本地 CMake 4.4.2/MSVC Release 全量构建通过，HuxerUI codegen 通过；
`ctest --test-dir build -E '^windows_service$' --parallel 4 --output-on-failure --no-tests=error`
18/18 通过（含 runtime_health、windows_physical_dns、DNS TLS、persistence、CPS），
管理员独立执行 windows_service 1/1 与 `test_windows_physical_dns --require-wfp` 通过。
未把构建/隔离 WFP 回归当作“双辽”实机拨号验收；完整配置仍需一次主内核交接。
实现、限制与验证入口见 [PPTP 连接交接](native-pptp-handoff.md)。

2026-10-24：移除旧 OpenVPN 独立连接兼容（下文较早记录描述当时的行为）。
OpenVPN 只保留普通 remote/local 来源；移除独立启停、批量勾选、旧编辑器和会话快照。
用户确认没有需要转换的旧独立配置，不新增迁移或删除用户数据；独立拨号只保留 PPTP。

2026-10-12 L3 增量：OpenVPN 导入并入普通来源。
代码快照为 `3b83bbb8fb15` 加当前未提交工作区；远程/本地/文本自动识别 .ovpn，
新建弹窗与页面移除独立 OpenVPN 类型，桌面弹窗复用创建页面表单；PPTP 保留。
普通 .ovpn 可作为主来源，桌面次来源按启用规则加载隔离 endpoint；源原文不转换保存，
手机仍保留单活动普通来源。旧 OpenVPN 记录归本地分区，保留 ID、凭据、内网路由、
引用和辅助操作，不改 schema 或启动迁移。未知/外部文件/未转换指令拒绝并保留账本。
JSON/.ovpn 不显示 YAML 规则编辑菜单，文件编辑保存后请求刷新格式能力。
Linux 本地执行 HuxerUI codegen 与 `cmake --build build --target clash-flux test_singbox
test_page_transition test_persistence test_routing test_rule_provider_download` 成功；
`ctest --test-dir build -L clashflux-required --output-on-failure --no-tests=error` 17/17，
`./run.sh --version`（0.3.29）与 `git diff --check` 通过。
独占临时 XDG 数据目录中的应用运行时 CLI 验证无后缀 URL .ovpn 导入、本地 .ovpn
设为主来源并引用远程次来源、四份新旧来源重开、旧配置保真度报告与失败刷新保留
原字节/所有 ID/活动来源/旧凭据和路由；生成主配置经固定 sing-box 1.14.2 check。
未建立真实 VPN 连接，未执行本批 Windows/Android/macOS 构建或线上 CI。
实现与限制见 [OpenVPN 导入](openvpn-import.md)。

2026-10-12 L3 修复：原生 VPN 连接成功后卡片未及时同步。
代码快照为 `3b83bbb8fb15` 加当前工作区改动；连接与断开改为逐条完成后携带
权威快照回到 UI 线程，通过 `VpnModel::Publish` 立即发布，不等一秒泵或整批结束。
数据泵按发布序号拒绝在途旧快照，初始化完成也直接发布。PPTP/OpenVPN 共用此路径。
`page_transition` 使用生产发布模型及 Lifecycle → StateList → VirtualGrid 链路，
验证首条连接在后续连接未完成时刷新、失败/断开立即同步、旧轮询拒绝、相等结果
仍使旧请求失效与外部变化正常轮询。Linux 本地 HuxerUI codegen 与构建通过；
`cmake --build build --target clash-flux test_page_transition test_vpn`、
`./run.sh --version`、`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|vpn)$'`
（2/2）与 `git diff --check` 通过。未执行 Windows 真实 PPTP 拨号或本批线上 CI。

2026-10-04 订阅切换性能：修复大 MRS 来源反复构造 YAML 节点、逐条 JSON 后再合并的
开销，改为有界字符串集合直接累计原生字段数组；固定快照未变化时不重复编译，
桌面没有 GEO 预取依赖时复用完整编译结果执行内核 check。没有跳过规则转换、
身份核验或候选检查，也没有修改数据库 schema、订阅映射和缓存提交边界。
新增回归覆盖 4096 条域名全部保留、原生 OR 分支数量有界，以及固定快照被内核
拒绝时仍完成检查且保留输入/旧缓存。
kitty 的独占离线副本（11 个规则集、2 份 ASN，当前 Debug 构建）准备耗时
**5.33s → 2.06s**，峰值 RSS **211904 → 64556 KiB**；两次生成的完整 JSON 相同，
49 条 fidelity 保留，生成配置通过固定 sing-box 1.14.2 check。
复制包含多来源及被引用文件的应用数据到独占 XDG_DATA_HOME，由应用 CLI
`profile use 7` 切换 kitty，停止内核状态下约 **4.50s** 完成；另一次应用运行重开后
确认五条订阅仍在且 kitty 为 main，所有复制的订阅文件与用户原文件相同。
未修改用户订阅或当前 main；该耗时不包含运行中内核重启或手机/其它桌面实机验收。
验证：`cmake --build build --target clash-flux test_singbox test_rule_provider_download -j2`、
`./run.sh --version`、`ctest --test-dir build -L clashflux-required --output-on-failure --no-tests=error`
（15/15）、隔离 `test_singbox --prepare-config` / 应用 CLI 与固定内核 check、`git diff --check`。


2026-10-04 L2 增量：基于 `7bbc149c81b728f9e1717440a29fff447ba13fb0` 加本地未提交改动，
修复 classical inline/file/HTTP provider 拒绝合法 `IP-CIDR,…,no-resolve` 的问题。
IPv4/IPv6 及嵌套 IP 条目保留匹配并记 Rule/Approx；未知/空/重复修饰符仍整份拒绝，
不改变订阅存储或候选提交边界。`singbox` 回归包含三条 provider 路径和失败位置，
`rule_provider_download` 回归通过；脱敏样本
`examples/classical-provider-no-resolve-2026-10-04.yaml` 编译后通过固定 1.14.2 内核 check。
这是本地转换与配置检查，尚未验收用户真实订阅的完整导入或手机数据面。

2026-10-04 L2 第二批增量（同一代码快照）：补 ASN 网段快照和 MRSv1 解码。
`IP-ASN` / `SRC-IP-ASN` 包括 classical 与逻辑子条件，完整双地址族数据转为原生
CIDR 并记 approx；MRSv1 domain/ipcidr 支持 ASCII 精确域名、子域、整层通配与
IPv4/IPv6 区间，损坏/未知格式、超限整份拒绝。HTTP 依赖分轮准备，完整检查前
不提交任何缓存，回归覆盖 ASN 失败与内核拒绝时旧缓存逐字节保留。
用户提供的真实订阅在独占临时目录准备 **11 个规则集 + 2 份 ASN 快照**，生成
配置通过固定 sing-box **1.14.2 check**；没有修改用户订阅、活动配置或数据库。
这是 Linux 本地转换和内核配置验收，不能写成 GUI 完整导入、数据面联网或手机实机验收。
命令：`cmake --build build --target clash-flux test_singbox test_rule_provider_download -j2`、
`ctest --test-dir build -R '^(singbox|rule_provider_download)$' --output-on-failure`（2/2），
`build/test_singbox --prepare-config <private-yaml> <isolated-fixtures> <private-json>`、
`build/engines/sing-box check -c <private-json>`、`./run.sh --version`、`git diff --check`。
补充检查：`cmake --build build -j2` 成功；`ctest --test-dir build --output-on-failure`
共 38 项，其中本项目必跑 15 项全部通过。HuxerUI 附加测试有 9 项因可执行文件缺失
未运行，`HuxerUIAndroidHttpTests` 与 `HuxerUIRuntimeDependencyTests` 失败（后者
报告 relocated runtime segmentation fault）；因此不能宣称全量 CTest 通过。
上述 HuxerUI 附加测试不等同于本批的 ASN/MRS 转换或 Android 实机验收。

MRS 固定 Zstandard 1.5.7 静态依赖已接入平台共用 CMake；Android 兼容 TU 跟踪新
解码片段。本批未执行 Android/Windows/macOS 编译，不把通用接入写成平台验收。

2026-10-04 产品修复：URL 导入后的按钮 loading 不结束。
现场 GUI 的本次订阅已持久化成功、相关缓存已提交、后台任务线程空闲；修复导入
结果与页面生命周期耦合的路径，使用应用级 `LaunchProfileImport` 完成回传，统一
先清除 loading 再调用 UI 完成反馈，异常变为失败结果，重复点击拒绝。桌面弹窗、
窄屏旧表单、独立表单与 URL 快捷入口已接入。没有删除/刷新用户已有订阅。
`page_transition` 生产封装回归验证 await 中卸载发起视图、完成后按钮恢复、异常反馈
和失败后重试通过；本批重新执行 HuxerUI codegen + clash-flux 构建。
验证：`cmake --build build --target clash-flux test_page_transition -j2` 成功；
`ctest --test-dir build -L clashflux-required --output-on-failure --no-tests=error` 15/15；
`./run.sh --version` 与 `git diff --check` 通过。
现场仍运行旧 GUI，不能把它写成新构建的真实链接 GUI 导入验收；需重启加载修复。

日期：2026-10-03。检查基线为 **v0.3.18**；后续 L2 工作已并入 **v0.3.19**。
本批代码快照为 `7bbc149c81b728f9e1717440a29fff447ba13fb0`（v0.3.19）加本地
未提交的 file/HTTP rule-provider、单值请求头、精确 hosts、加密 DNS 证书参数、策略通配、组节点展开、L3 目标目录及共用页面布局增量，应用版本仍为 0.3.19；本批未发布。设置页 Material 3 重构已于 2026-10-04 单独回退。
v0.3.18 包含凭据/TLS 修复及 VLESS encryption、旧 WS 字段兼容修复；随后补
L2 UDP 编码、VMess 填充、HY2 混淆/跳端口、SS 插件/UOT 版本，以及逻辑路由
嵌套 GEO/RULE-SET、Hysteria v1 UDP 子集与带宽单位修复、SSH 认证/公钥子集。
以下明确区分当前批次与历史验证，不把本地检查等同于线上 CI 或全平台实机验收。

固定内核为 sing-box **1.14.3**（2026-10-19 升级；下文历史批次中 "1.14.2 check" 为当时的验证记录），revision
`7054cac5124465ffdb145870d7c01885566711b1`；职责和约束以
[分层契约](singbox-layers-and-fidelity.md)为准，桌面最终模型以
[订阅编排设计](desktop-subscription-orchestration.md)为准。

最近几批改动的合并视图、架构复审和下一步排序见
[2026-10-03 项目复审](project-review-2026-10-03.md)。复审发现的 **R01–R03 三项
订阅保护问题**（启动失败空缓存继续运行、异步清脏竞态、文件删除引用/落库边界）
与 R04 provider format 类型缺陷已在后续开发修复并补隔离回归，详见该文末更新。
成功迁移和本地回归不等于全平台实机或完整崩溃事务已经验收。

后续已补桌面 CI 必跑测试门禁：加入组展开实际运行回归后严格核对 15 项（已记录的
MSVC 缺陷仅豁免直接 ORM，保留 14 项），缺失 Python/固定内核/OpenSSL 或漏注册/禁用
均失败。Linux 本地 15/15 通过，门禁自身 22 场景通过；上一批实际项目 4 类缺失配置
验证已通过。本批组展开另有 8 个固定内核 API/实际 SOCKS 转发场景，11 份公开样本
编译及内核 check、Android Debug 构建通过；未执行线上 Windows/macOS CI 或手机
实机验收。实现及命令见复审文末。

## 判定口径与结论

“已接入”表示代码实现存在；“部分完成”表示仍缺字段、生命周期或产品入口；
“待实现”表示目标尚未接入；“不做”表示固定内核没有对应语义；iOS 单列为暂缓。
验证证据另分代码复核、构建、配置检查、交互/实机四种，不互相替代。

| 层 | 当前结论 | 可量化范围 | 距离完成的主要缺口 |
|---|---|---|---|
| L1 内核层 | 固定版本、资产校验、桌面/Android 控制链已接入；平台运行验收部分完成 | 官方桌面资产表 6 项；实际桌面 CI 4 项；本机验证 Linux x86_64 与 Android arm64 构建 | 其它平台实机、网络/权限/后台生命周期、故障恢复验收 |
| L2 翻译层 | 常用转换与结构化账本已接入；补齐协议子集后继续接入 file/HTTP provider、请求头、hosts 与 DNS 证书/通配，R04 已修复；整体仍部分完成 | Clash 代理协议类型 12/15；端点编辑转换 1/5；类型覆盖不表示字段完整 | provider 剩余生命周期/格式、更多 DNS/协议字段 |
| L3 产品层 | 常用客户端页面、桌面编排首阶段、订阅唤醒已接入；最终产品形态部分完成 | 桌面 7 个一级页、手机 4 个一级页；支持的原生端点编辑器仍只有 OpenVPN client | 完整编排恢复、来源/连接追溯、原生 DNS/端点/规则集编辑、跨平台交互验收 |

不计算总完成百分比：内核有某个协议、编译器能生成它、界面能编辑它、真机能稳定
运行它是四件不同的事。`unsupported` 的显式拒绝解决诊断边界，不算新增协议支持。

### 开发重心调整（2026-10-03）

按用户意图，从继续补齐所有 L2 字段转向 **L3 产品完善**。常用 L1/L2 路径已能支撑
当前客户端使用，不能据此标成全部完成；订阅保护、转换保真度与候选失败保留仍是门禁，
使用中发现的问题继续补 L1/L2，并保留上述未完成清单。

本批先完成桌面编排目标目录，后续优先来源定位/规则追溯、代理页搜索与浏览状态、
保真度明细的定位动作，再按固定内核能力建设原生 DNS/规则集/端点入口。
多订阅编排不扩展到手机；完整崩溃恢复与其它平台实机验收继续独立跟踪。

## L1：内核层

| 项目 | 完成度与证据 | 边界 |
|---|---|---|
| 版本/revision 固定 | 已接入。CMake、Android Gradle、libbox 脚本、CI 使用相同 pin；本地 `sing-box version` 返回 1.14.2 和上述 revision | 本轮不查询或宣称它仍是线上最新版本 |
| 桌面资产与缓存 | 已接入。6 个官方归档分别固定 SHA256，缓存按版本/资产摘要隔离；本机 Linux 归档摘要匹配，缓存二进制与 `build/engines/sing-box` 一致 | 6 个资产映射不等于 6 个客户端发行目标均已构建 |
| Android libbox 来源 | 已接入。正式 tag/revision 核验，AAR 元数据核验版本、revision、ABI、SHA256；本地 arm64 AAR 摘要重新核对通过 | 元数据与构建来源核验不替代真机运行版本/行为验证 |
| 桌面运行与控制 | 已接入。官方进程 + `clash_api`；启停、模式、TUN、selector、测速、连接和日志有调用路径 | 网络权限、系统代理恢复和各平台退出清理仍需实机验收 |
| Android 所有权 | 已接入。`:background` 持有 VpnService/libbox，AIDL/Binder 回传快照；数据库由 UI 进程拥有，配置以原子文件共享 | 本轮只构建 APK，未重新验证 Doze、进程重建、切网、后台长时运行 |
| 候选配置检查 | 桌面已接入固定内核 `check`，通过后才停止旧实例；Android 由后台 libbox 启动流程检查 | Android 没有桌面 CLI 预检查；`check` 不证明远端握手或真实流量行为 |
| 恢复与事务 | 部分完成。有本进程上一份计划和失败恢复，旧原生资源失效时不恢复旧引用 | 无跨 SQLite/文件/路由的持久事务和崩溃 journal；手机异步失败回滚不完整 |

代码证据：[资产缓存](../cmake/singbox_bundle.cmake)、
[AAR 核验](../platform/android/app/build.gradle)、
[libbox 构建](../platform/android/build-singbox-libbox.sh)、
[桌面候选与启动](../src/store/core_store_lifecycle.inc)、
[候选检查/GEO 预取](../src/store/core_store_helpers.inc)、
[Android 服务](../platform/android/app/src/main/java/dev/farna/clashflux/ClashVpnService.java)。

实际工作流矩阵为 Linux x86_64、Linux arm64、Windows x86_64、macOS arm64、
Android arm64，另有 Windows 安装器门禁。Linux arm64 是非阻塞实验项；
Windows/macOS/Android 的构建门禁已启用。Windows arm64 和 macOS x86_64 当前没有
对应 job。以[工作流实际 matrix](../.github/workflows/build.yml)为准，不能从其历史
“六项全部入矩阵”注释推导当前支持范围。本次没有查询这些 job 的最近一次运行结果。

**iOS 暂缓**：Simulator/Packet Tunnel/Libbox 编译配方是非阻塞诊断，未签名、不生成
IPA、不进入 Release 门禁；不计为完成的平台支持。

## L2：翻译层

| 领域 | 已完成范围 | 未完成/近似范围 |
|---|---|---|
| 代理协议 | SS、VMess、VLESS、Trojan、Hysteria v1 UDP 子集、HY2、TUIC、HTTP、SOCKS、AnyTLS、Snell v4、SSH，共 12 类 | ShadowTLS、Tor、Naive 的 Clash 转换未接入；12 类仍非全部字段覆盖，HY1 窗口/快速打开/伪装等边界见本批说明 |
| TLS/传输 | TCP、WS、gRPC、HTTP/h2、HTTPUpgrade；SS simple-obfs HTTP/TLS 与 v2ray-plugin WebSocket/TLS/mux 子集；SNI、ALPN、客户端指纹及部分 Reality 字段；F02/F03 异常值/未启用字段显式拒绝 | 其它 SS 插件和插件自定义 headers/证书/ECH 等仍未映射；random 指纹分布及 PSK/PQ 别名折叠记 approx |
| 策略组 | selector/urltest、初始选择、间隔/容差、本来源 include-all-proxies 静态名称排序展开；过长间隔保留并延长 idle_timeout、记近似 | 非空筛选、代理集合/混合展开、空组回退未接入；fallback/load-balance 只能近似成 urltest；锁定、lazy/timeout 等不能伪装支持 |
| 拨号/依赖 | `dialer-proxy`→detour，组成员与 DNS resolver 共同检查循环；接口、mark、TFO、MPTCP 按平台处理 | detour 忽略物理选项、MPTCP IPv6 差异记 approx；跨来源原始代理链未开放 |
| DNS | local、UDP/TCP、DoT/DoH/DoQ/h3、bootstrap；加密 DNS 显式 skip-cert-verify；完整域名/整层 */前缀 . 与 +. 的普通与节点专用 policy；精确 hosts IP/完整列表用于 DNS、节点/端点及连接解析 | 更多 URL 参数/name-cert-verify、规则集 policy/其它模式、fallback-filter、hosts 通配/别名/系统开关、FakeIP、direct follow-policy 转换未完成；缓存/多 IP 选择、多服务器/fallback/直连解析时机有近似 |
| 普通路由 | 域名/正则、源/目标 CIDR、端口/范围、TCP/UDP、IP-VERSION、GEO、桌面进程；Linux UID、Android 包名/正则 | 平台不适用字段拒绝；Android owner 查询失败/共享 UID 的实际行为未验收 |
| 逻辑与 provider | AND/OR/NOT 有界子集，路由支持嵌套 GEO/已转换 RULE-SET；domain/ipcidr/classical inline、file、HTTP YAML/text 快照与单值 ASCII header；MRSv1 domain/ipcidr、ASN 快照；整份转换/拒绝重名 | HeadlessRule 不接受 route 的 UID/IP-VERSION/GEO/RULE-SET；HTTP 指定代理/多值或传输保留 header/MRS classical/未来版本、文件监听与其它未映射条件仍待实现 |
| 规则集下载 | 自有 GEO 清单、桌面按周刷新；Android 固定打包 CN；HTTP 直连任务下载/候选校验/缓存快照，失败留旧；file 只读 | HTTP 定时热更新、MRS classical/未来版本、文件监听、管理 UI 和多缓存崩溃事务未实现；SRS 文件头筛查不是完整解析 |
| 规则终止/失败 | 首个有效 MATCH 终止；DIRECT 不改成 selector；无效兜底失败；REJECT-DROP 使用原生 drop；失败仍返回已产生账本 | 保留原生内核语义，不提供停核后的系统 kill switch |
| 原生配置/连接 | 原生 JSON 保留用户字段并合并托管项；OpenVPN `.ovpn`→openvpn-client；PPTP 走平台补充路径 | JSON 不是完整可视化编辑器；原生 JSON 次来源不支持；WireGuard/Tailscale/OpenConnect/OpenVPN server 编辑转换未接入 |
| 保真度闭环 | `CompileResult.fidelity`→CoreSnapshot→设置明细、动作 toast、组角标、CLI；节点键名与本批 TLS/传输值均有拒绝边界 | 本批回归封住 F01–F03，不证明所有其它字段和原生配置合并都已完成审查 |

代码证据：[节点](../src/singbox_proxy.inc)、[DNS](../src/singbox_context_dns.inc)、
[规则/provider](../src/singbox_rules.inc)、[主配置/托管边界](../src/singbox_compile.inc)、
[多来源](../src/singbox_sources.inc)、[原生入口](../src/singbox.cpp)、
[OpenVPN](../src/singbox_yaml_openvpn.inc)。端点 1/5 指编辑和转换入口，PPTP 不计入
sing-box 端点分母；WireGuard 已从 outbound 移除，不能算作未接入的出站协议。

### 本轮已经关闭的问题

- 旧协议的 `fingerprint` / `shadow-tls-opts` 不再静默消失，逐字段记 unsupported，
  整条节点拒绝；引用被拒绝节点的 MATCH 失败，保留账本。
- 显式 `udp: false` 在有对应原生限制的协议上转 `network: tcp`；HTTP 的 UDP 意图记
  approx；无效布尔/HY2 带宽拒绝，HTTP 多 path 取首项记 approx。
- Trojan/HY2/TUIC 默认 TLS、QUIC 不注入 TCP uTLS、TUIC `tls.disable_sni` 与 ALPN/SNI
  共用转换已修复。SOCKS TLS 及 QUIC 不支持的 uTLS/Reality 明确拒绝。
- 固定 1.14.2 内核 `check` 已接受当前 **11 类协议**组成的配置；这没有证明服务器握手。
- F01–F03 修复及永久回归已接入，见下表；同时修复 OpenVPN inline 凭据按空格拆词的问题。

### F01–F03：初始复现与首批修复（2026-10-02）

下表保留修复前的生产编译器复现。新增永久回归在修复前得到 48 项失败；修复后
连同额外边界测试通过。所有样本使用占位凭据，诊断不输出凭据内容。

| ID / 状态 | 修复前输入与观察 | 修复后行为与回归 |
|---|---|---|
| F01 / 已修复 | Trojan、SS 的 `password: ' sample '` 输出变成 `"sample"`，编译成功、fidelity=0。旧分支凭据经 `ytext()` 被 trim | SS/Trojan/HY2/TUIC/HTTP/SOCKS 的 password、username 和 HY2 obfs-password 按 scalar 原文读取；保留 AnyTLS/Snell 原文行为。类型异常拒绝并记账；可选空 HTTP/SOCKS 凭据保留。OpenVPN inline 按两行读取，只去 CRLF 分隔，不拆词/去引号/trim；缺行或需交互的空凭据明确失败并记来源账本 |
| F02 / 已修复 | TLS VLESS 的 `alpn: {protocol: h2}` 被删；VMess WS 的 `headers: {Host: [example.test]}` 被删；均保留节点、编译成功、fidelity=0 | ALPN scalar/list 完整校验，协议名 1–255 字节，空数组保留；WS headers 必须为 scalar 键值对象，拒绝重复键和异常成员。HTTP/h2 path/host、gRPC/HTTPUpgrade 字符串及 WS early-data 参数也校验；异常整条拒绝，不保留半份传输配置 |
| F03 / 已修复 | 根级 `global-client-fingerprint: firefox` + 未设节点指纹的 Trojan 输出 chrome，fidelity=0。未启用 TLS 的 VLESS 显式 ALPN/client-fingerprint 也被省略且 fidelity=0 | 每个 Clash 来源独立继承全局指纹，节点优先，均省略时保留 chrome 基线；非法全局值拒绝整份来源并保留账本。未知节点指纹、未启用 TLS 的显式 TLS 字段拒绝；tls:false 不再因 SNI/Reality 偷开 TLS。QUIC 不继承 TCP uTLS；random 分布和内核折叠别名记 approx |

相关位置：`singbox_yaml_openvpn.inc::ytext/ylist`、
`singbox_proxy.inc::appendTls/appendTransport/convertProxy` 和 Clash 文档入口。
永久测试在 [test_singbox.cpp](../tests/test_singbox.cpp)；多来源同名节点分别继承
firefox/safari，并验证无全局值的次来源不会继承主来源指纹。不能因这批问题关闭
就宣布整个 L2 契约完成；更多协议/DNS/provider 的字段范围仍按上表继续推进。
对照样本：Trojan 的未映射证书/组合字段产生 2 条账本且节点被拒绝；根级 `hosts`
产生 1 条 unsupported，因此没有把 hosts 错记成另一个静默丢弃缺陷。

## L3：产品层

| 项目 | 完成度 | 当前行为与缺口 |
|---|---|---|
| 常用页面/控制 | 已接入 | 首页、订阅、代理、规则、连接、日志、设置；真实 selector 切换、测速、模式、TUN、系统代理有领域动作，手机使用自身能力限制 |
| 设置页 | 已恢复原布局 | 2026-10-04 单独回退 Material 3 重构，恢复通用／内核／关于与原控件；共用标题、顶部栏及侧栏间距改动保留，见 [回退与历史记录](settings-material3.md) |
| 应用模型/持久化 | 已接入，R01–R03 已修复；跨平台与崩溃恢复仍待验收 | 异步 SQLite、旧库多来源迁移重开；新增失败拒写、按版本确认、提交后引用检查清理及真实 SQLite 锁交错回归，见项目复审 |
| 分页/虚拟化 | 部分完成 | 二级 Pager 跟手、取消、反选、标签居中与有界大组已有生产回归；隐藏一级页保留 hook 状态并卸载重内容，其重新可见时的内层滚动恢复仍有已知代价 |
| 保真度呈现 | 已接入，受 L2 输出完整性限制 | 设置完整明细、用户动作后的摘要、组 `!`、CLI 明细；账本漏记时这些消费点无法自行发现问题 |
| 唯一主订阅 + Clash 次来源 | 首阶段已实现 | 主来源 Clash/原生 JSON；普通 Clash 次来源仅由启用规则参与；同名对象隔离，无引用不载入；手机保持单活动普通订阅 |
| 编排规则编辑 | 首阶段已实现 | UserOverride/SourcePolicy、priority/order、启停、Default/Group/Node、Reject/UseMain/Direct；桌面宽/窄编辑器已接入 Clash 来源的可搜索目录（含未参与来源），切源清空选择，失效旧引用保留；手机、原生连接/JSON 仍只开放默认出口 |
| 编排身份/持久化 | 部分完成 | policy v2、source ID + kind + 原始唯一名称；不是对象 UUID；对象改名会使引用失效，独立 binding 表待做 |
| 应用/回滚 | 部分完成 | 候选 check、成功应用运行序号、预览与运行来源映射分离、进程内失败恢复；无崩溃 journal、跨重开上一有效计划和手机异步事务恢复 |
| 来源/连接追溯 | 部分完成 | 代理、托盘、首页、连接链显示可读来源；连接有内核规则文本，尚不能定位到具体 L3 policy rule ID |
| 网页唤醒订阅 | 已接入，平台验收部分完成 | sing-box/FlClash/Clash 别名与专用协议；独立预填表单、粘贴/扫码、桌面单实例 IPC；Linux 冷启动预填和并发转发已验证，Android 构建/manifest 核对；其它系统与真机 intent 未验收 |
| sing-box 独有能力编辑 | 部分完成 | OpenVPN client 为原生连接；原生 JSON 可导入；WireGuard/Tailscale/OpenConnect/OpenVPN server、原生 DNS、通用 rule_set 编辑器未完成 |
| Android 应用路由 | 部分完成 | package_name/regex 输入及后台 owner 查询已实现；应用选择 UI 未做，API 29 前后、共享 UID、查询失败需真机验证 |

编排代码证据：[规则领域](../src/vpn.cppm)、[路由](../src/routing.cppm)、
[规则编辑器](../src/ui/rules_page.cpp)、[来源编译](../src/singbox_sources.inc)、
[订阅候选/激活](../src/store/profiles.cppm)、[来源映射](../src/store/core_store_state.inc)、
[连接展示](../src/ui/connections_page.cpp)。唤醒格式与接入见
[profile-links.md](profile-links.md)。

本次定向检查确认：同名双来源编译成功（参与来源 2 个）；禁用唯一引用后为 1 个；
次来源不可用时分别生成 reject、主默认出口或主 DIRECT，匹配条件仍保留；原生 JSON
次来源明确失败并记账；`a→b→a` detour 循环明确失败。不能再把首阶段写成“完全未实现”，
也不能把这些配置检查称作已完成切主、刷新、故障注入与真实流量的端到端验收。

## 前序 L2 批次：UDP / VMess 填充 / HY2（2026-10-02）

- VMess/VLESS `packet-encoding` 与历史 `packet-addr`/`xudp` 映射，保留各协议默认
  和输入优先级；VMess `global-padding`、`authenticated-length` 直接写原生字段。
- HY2 salamander/gecko 按实际类型映射，缺密码与未知混淆拒绝；密码单独出现不再
  擅自开启混淆，非活动参数记账。Gecko 包尺寸按固定内核默认和上限校验。
- HY2 ports 原子解析，单端口输出原生 singleton 范围，支持只有 ports 的节点；
  hop-interval 支持秒数/范围，缺省 30 秒、最小 5 秒限制和不生效字段均明确处理。
- 新永久回归在实现前复现 43 项失败；修复后 `test_singbox` 及项目 9 项测试通过。
  更新的 [配置样本](examples/layers-audit-2026-10-02.yaml) 当批包含 10 协议、11 个节点（后续 HY1 批扩至 11 协议、12 节点），
  生产编译器输出通过 sing-box 1.14.2 `check`（退出码 0）。
- 三份本地订阅的独立测试副本保留节点及代理组字段；为满足现有 harness 的国内
  规则断言，替换 rules 并去掉 rule-providers 声明。节点保留 32/32、30/30、34/34，
  三份输出均通过固定内核 check；这不验证原订阅全部规则/provider。私有凭据未入库。
- `cmake --build build --target clash-flux test_singbox`、`./run.sh --version`
  （v0.3.18）通过；Android `:app:assembleDebug --offline --no-daemon` 重新构建成功。
  `git diff --check` 通过。本批未提交、打标签、推送或发布；未做远程线路握手或真机安装。

字段及保真度细节见[分层契约](singbox-layers-and-fidelity.md)。L1 固定版本不变，
L3 没有新增编辑入口；类型覆盖仍为 10/15，不能把字段补齐计作新增协议或整体完成。

## 后续 L2 批次：SS 插件与 UOT 版本（2026-10-02）

- 接入内核内置 simple-obfs HTTP/TLS 和 v2ray-plugin WebSocket/TLS/mux 子集，
  保留 Clash 的 host/path/mux 默认，选项字符串按 SIP003 转义，TLS false 不输出启用键。
  未映射参数、重复键、异常类型和缺 mode 整条拒绝并记账；不调用外部插件进程。
- 修复 SS UOT 默认版本差异：此前 `udp-over-tcp:true` 写布尔值会走原生 v2，
  现在显式写 v1，与 Clash 缺省/0 的有效版本一致；输入显式 v1/v2 均支持。
- 永久回归实现前复现 16 项失败，修复后通过；`ctest` 指定的 9 项项目测试全部通过。
  并行样本检查还复现了 harness 共用 /tmp 目录互相覆盖的问题；规则集测试现在
  独占临时目录并由 RAII 清理，8 个并行测试进程全部通过。
  [8 节点样本](examples/ss-plugins-2026-10-02.yaml) 全部保留，生成配置通过固定
  sing-box 1.14.2 `check`。之前的 UDP/HY2 样本仍通过同一编译器回归。
- 三份订阅节点/组的规范化副本再次保留 32/32、30/30、34/34，输出均通过内核
  check；测试仅替换 rules、去除 rule-providers，未改源订阅或验证全部原规则，凭据未入库。
- Linux `cmake --build build --target clash-flux test_singbox`、`./run.sh --version`
  （v0.3.18）、Android `:app:assembleDebug --offline --no-daemon` 和 `git diff --check`
  通过；未进行远程握手/真机安装，未提交、打标签、推送或发布。

复现配置检查：

```bash
build/test_singbox docs/examples/ss-plugins-2026-10-02.yaml \
  platform/android/app/build/generated/rule-set-assets/rules \
  /tmp/clash-flux-l2-ss-audit.json
build/engines/sing-box check -c /tmp/clash-flux-l2-ss-audit.json
```

## 后续 L2 批次：逻辑路由嵌套 GEO / RULE-SET（2026-10-02）

- AND/OR/NOT 路由接入 GEOIP/GEOSITE、private/lan 和已转换 inline RULE-SET 引用；
  顶层和子条件共用资源转换，保持原生 logical/mode/invert，动作只位于最外层。
  未声明的国内别名仍记 approx；失败声明不能替换成同名国内别名。
- 子条件失败时整条逻辑规则拒绝，保留保真度明细，RAII 撤回本条新增 rule_set、
  自有 GEO 资源及 tag 缓存；既有资源保留，后续有效规则仍能生成。未知路由目标
  在解析子树前拒绝，不产生无用 GEO 资源。
- 子条件不推导全局 CN DNS 分流，避免 NOT/复合匹配扩大 DNS 策略。HeadlessRule
  继续拒绝 GEO/RULE-SET，不把路由字段写进 inline classical；Android 本地 GEO
  与手机单来源限制保持。桌面双来源回归确认嵌套引用正确改写，缓存 tag 保持原名。
- 永久回归在实现前复现 5 项失败，修复后全部通过；项目 `ctest` 9/9 通过。
  [公开逻辑样本](examples/logical-rules-2026-10-02.yaml) 的 6 条逻辑规则、3 份 inline
  provider 和 2 份 GEO 资源全部生成，输出通过固定 sing-box 1.14.2 `check`。
  UDP/HY2、SS 插件样本仍通过同一生产编译器和固定内核检查。
- 三份私有订阅再次使用独占临时副本，仅替换 rules 并去除 rule-providers；节点/组
  字段保留，32/32、30/30、34/34 节点均生成且配置 check 通过。副本已清理，
  不验证源订阅全部规则/provider 或远程握手，凭据未入库。
- Linux `cmake --build build --target clash-flux test_singbox`、`./run.sh --version`
  （v0.3.18）、Android `:app:assembleDebug --offline --no-daemon` 和 `git diff --check`
  通过。未进行真机安装，未提交、打标签、推送或发布。

复现配置检查：

```bash
build/test_singbox docs/examples/logical-rules-2026-10-02.yaml \
  platform/android/app/build/generated/rule-set-assets/rules \
  /tmp/clash-flux-l2-logical-audit.json
build/engines/sing-box check -c /tmp/clash-flux-l2-logical-audit.json
```

该逻辑批次时 L2 类型覆盖为 10/15；外部 provider、hosts/FakeIP、更多 DNS/TLS
字段及 L3 原生编辑入口不在此批交付范围。

## 后续 L2 批次：Hysteria v1 UDP 子集 / 带宽单位（2026-10-02）

- 新增 Hysteria v1 的 UDP 转换，协议类型覆盖为 **11/15**；认证、XPlus 混淆、
  正整数带宽、协议 TLS/SNI/ALPN、严格 ports 和 10 秒 hopping 默认已接入。
  auth 的 Base64 字节优先于 auth-str；凭据保留原文并验证 65535 字节线上长度。
- 接收窗口按实际输入生态的 stream/connection 方向映射；成对非零窗口 exact，
  默认窗口保留 15/64 MiB 上限但初始分配不同，明确记 approx；单边窗口组合尚未接入。
  非 UDP 伪装、fast-open:true、显式空 ALPN、证书/ECH/指纹约束及兼容 up-speed/
  down-speed 等字段拒绝整条节点，不省略后继续连接。
- HY1/HY2 共用带宽转换，修复旧 HY2 将 `3 MBps` 当作 `3 Mbps` 的问题，现在
  写 24 Mbps；大小写、类型、正数及乘法范围检查不允许静默回落自动带宽。
- 永久回归在实现前复现 16 项失败，修复后通过，并新增认证 padding/CRLF/二进制、
  16 位长度边界和失败 MATCH 保留账本回归。项目 `ctest` 9/9 通过。
  [HY1 样本](examples/hysteria1-2026-10-02.yaml) 5/5 节点保留；总协议样本扩充为
  11 类、12 个节点，全部生成。两者及 SS 插件、逻辑路由样本均通过固定 1.14.2
  内核 `check`；这些配置检查不验证远端握手或真实流量。
- 三份私有订阅独占临时副本继续保留 32/32、30/30、34/34 节点，配置 check
  通过；仅替换 rules 并去除 rule-providers，未改源订阅或验收全部原规则，副本已清理。
- `cmake --build build --target clash-flux test_singbox`、`./run.sh --version`
  （v0.3.18）、Android `:app:assembleDebug --offline --no-daemon`、`git diff --check`
  通过。本批未真机安装、提交、打标签、推送或发布，L1 固定内核不变，L3 无新增编辑器。

复现配置检查：

```bash
build/test_singbox docs/examples/hysteria1-2026-10-02.yaml \
  platform/android/app/build/generated/rule-set-assets/rules \
  /tmp/clash-flux-l2-hy1-audit.json
build/engines/sing-box check -c /tmp/clash-flux-l2-hy1-audit.json
```

本批结束时尚未完成 ShadowTLS/SSH/Tor/Naive 的 Clash 转换；更多 HY1/HY2 字段、外部
provider、DNS/hosts/FakeIP 与 L3 编辑入口继续按契约推进。

## 后续 L2 批次：SSH 认证 / 主机公钥子集（2026-10-02）

- 新增 SSH 的 Clash YAML 转换，协议类型覆盖为 **12/15**；密码、内联 PEM 私钥、
  加密私钥口令、主机公钥和固定原生库的 20 种主机算法偏好列表已接入。
  所有认证字段保留原文，列表完整校验，不删异常成员后保留部分配置。
- 双认证完整保留两种凭据，密码/私钥尝试顺序差异记 `Node/Approx`；无私钥的非空
  口令记不生效。显式缺失/空用户名拒绝，避免被原生默认 root 改写。
  外部私钥路径、显式空算法列表和其它未映射字段拒绝整条节点。
  SSH 不输出原生不存在的 TLS/network 字段，请求 UDP 则明确记 TCP-only 降级。
- 通用 dialer/platform 限制不变。永久回归覆盖缺失/循环 detour、失败 MATCH 保留
  账本、双来源同名 SSH 的认证字段和 detour 命名空间隔离；首组回归在旧实现复现
  9 项失败，修复后通过，另补所有算法、列表与来源边界。项目 `ctest` **9/9** 通过。
- [公开 SSH 样本](examples/ssh-2026-10-02.yaml) 5/5 节点保留，包含专门生成的公开
  测试私钥、加密私钥、固定公钥、双认证和代理链；固定 1.14.2 `check` 通过。
  内核还正确拒绝了错误解密口令与无效主机公钥两个临时候选。
  总协议样本为 12 类、13/13 节点，HY1 5/5、SS 插件 8/8 及逻辑规则样本再次通过 check。
- 三份规范化私有订阅副本保留 32/32、30/30、34/34 节点，并通过内核 check；只替换
  rules、去除 rule-providers，源订阅未改，独占临时副本已清理。这不验证全部原规则。
- `cmake --build build --target clash-flux test_singbox`、`./run.sh --version`
  （v0.3.19）、Android `:app:assembleDebug --offline --no-daemon`、`git diff --check`
  均通过；Android 构建日志为 `/tmp/clash-flux-l2-ssh-android-build.log`。
  未验收远端 SSH 握手、真实流量或本批真机生命周期；L1 pin 不变，L3 未新增编辑器。

复现配置检查与构建：

```bash
build/test_singbox docs/examples/ssh-2026-10-02.yaml \
  platform/android/app/build/generated/rule-set-assets/rules \
  /tmp/clash-flux-l2-ssh-audit.json
build/engines/sing-box check -c /tmp/clash-flux-l2-ssh-audit.json
# 在 platform/android 下运行：
JAVA_HOME=/opt/android-studio/jbr ANDROID_HOME=$HOME/Android/Sdk \
  HUXERUI_HOME=$HOME/.local/share/HuxerUI \
  ./gradlew :app:assembleDebug --offline --no-daemon
```

当批仍缺 ShadowTLS/Tor/Naive 的 Clash 转换、HTTP 高级下载字段/MRS 和更多 DNS/协议字段；2026-10-04 MRSv1 domain/ipcidr 增量见文首。
保真度分类与上游依据见[契约](singbox-layers-and-fidelity.md#ssh-认证与主机公钥子集2026-10-02)。

## 本批增量：本地 file rule-provider（2026-10-03）

基于 `7bbc149c81b728f9e1717440a29fff447ba13fb0`（v0.3.19）工作区，未提交、未发布；
L1 pin 与 L3 编辑器范围不变，协议类型覆盖仍为 **12/15**。

- `type: file` 的 YAML/text domain/ipcidr/classical 子集接入原生 inline rule_set，
  任意名称和逻辑 RULE-SET 引用共用现有转换与命名空间。来源整体 approx，因为
  不监听文件，不会热更新；再次应用配置才读新内容。
- `ruleProviderDir` 明确由应用数据目录传入，候选检查/启动/诊断共用根；禁止绝对
  路径、父目录跳转、越界符号链接和非普通文件，最多只读 8 MiB，不写源文件或
  纳入 GEO 下载/清理。YAML 仅接受唯一 payload 的单文档映射，text 忽略整行注释。
- 缺文件、坏 YAML、未映射字段、无效成员会让候选编译失败，保留结构化失败账本。
  同名 file/inline 无论声明顺序均拒绝，重复字段不取第一项；不提交有效前缀或换成
  同名 CN 别名。接入既有候选检查，不新增订阅数据库/schema 或修改订阅保护边界。
- 永久回归覆盖三个 behavior、两种格式、CRLF/无末尾换行、文件更新快照、路径/
  符号链接边界、整份转换失败、重复字段/来源、多文档、NUL 与大小上限。
  本地项目 `ctest` **9/9** 通过，包括多 profile 迁移后重开保护回归。
- [公开样本](examples/file-providers-2026-10-03.yaml)包含 3 个 file 集合、共 6 个
  headless 条件，另加 GEO 2 集合；编译及固定 sing-box 1.14.2 `check` 通过。
  原 12 协议、SSH、HY1、SS 插件和逻辑资源五份样本再次通过编译及内核 check。
- 三份私有订阅的独占临时副本保留 **32/32、30/30、34/34** 节点并通过 check，
  源文件摘要未变，临时目录已清理。只替换 rules、去除 rule-providers，不验证原
  订阅完整规则或远端握手，不入库私有凭据。

复现公开样本：

```bash
cmake --build build --target clash-flux test_singbox
./run.sh --version
ctest --test-dir build --output-on-failure \
  -R '^(smoke|profile_links|page_transition|singbox|vpn|routing|compensation|sqlite_orm|persistence)$'
build/test_singbox docs/examples/file-providers-2026-10-03.yaml \
  platform/android/app/build/generated/rule-set-assets/rules \
  /tmp/clash-flux-l2-file-audit.json
build/engines/sing-box check -c /tmp/clash-flux-l2-file-audit.json
git diff --check
```

以上本地检查通过，`run.sh --version` 返回 v0.3.19。Android 使用上一节相同环境
运行 `:app:assembleDebug --offline --no-daemon`，日志为
`/tmp/clash-flux-l2-file-android-build.log`。本批未新增运行中坏文件回滚的实机验收，
未验证真实 DNS/分流流量或 Android 私有目录文件管理。该 file 批次尚无 HTTP 下载，
后续直连子集见下一节；MRS、文件监听和规则集 UI 仍待实现，不能据此宣称完整生命周期。
保真度分类、路径约定与上游依据见[分层契约](singbox-layers-and-fidelity.md)。

## 本批后续增量：HTTP provider 下载与缓存（2026-10-03）

仍基于 v0.3.19 / `7bbc149c81b728f9e1717440a29fff447ba13fb0` 加未提交增量。
版本与内核 pin 不变，L2 协议仍为 **12/15**，L3 未新增规则集管理入口。

- HTTP(S) YAML/text domain/ipcidr/classical 子集支持直连或 `proxy: DIRECT`；
  `httpRuleProviders` 清单与 GEO 分开，编译器不联网。任务层下载到独占目录，
  全部规则转换、策略目标检查与桌面 check 后才提交自有缓存。
- 缓存核对完整来源身份和版本，缺失/坏内容不触发删除或重建订阅；无可用内容
  就拒绝候选，已有验证内容时坏更新/断网沿用旧值。运行 options 持有 raw 快照，
  新缓存不覆盖旧计划，回滚不重新下载。不修改数据库/schema/用户订阅文件。
- 来源整体 approx：默认下载直连、path 只读种子、自有缓存布局、默认 8 MiB 上限、
  size-limit 超限拒绝而非截断、interval 仅在应用配置时检查，未接入定时热更新。
  header、其它指定 proxy、MRS/未映射字段拒绝整份，不静默改成可跑的下载。
- 原子替换复用 `api::CommitFile`；默认验证证书。Android 使用有界 Java 下载桥接
  和显式直连参数，普通订阅默认参数保留；后台 libbox 原生核验边界不变。
- 永久回归覆盖首次准备、缓存重读、过期断网/坏更新留旧、完整候选失败零提交、
  多来源部分转换失败零提交、来源身份隔离、future version/身份冲突、限额、只读
  种子、raw 快照回滚、任务异常与独占目录清理。
- 新增 `rule_provider_download` 真实 curl 回归 **11 个场景**：正常下载、整份规则
  失败、200 错误页、已知/未知长度超限、截断响应、503、非 HTTP 重定向、正常
  重定向、断网、自签证书默认拒绝；请求失败时两个稳定来源文件保持原样。
  本地项目测试 **10/10** 通过（含多 profile 迁移后重开保护回归）。
- [HTTP 离线样本](examples/http-providers-2026-10-03.yaml)采用 3 个只读种子、独占
  临时缓存、禁用网络，生成 3 个集合/6 个 headless 条件；固定 1.14.2 check 通过。
  另外六份协议/SSH/HY1/SS/逻辑/file 样本再次通过编译与内核检查。
- 三份规范化私有订阅副本保留 32/32、30/30、34/34 节点并通过 check，源文件
  摘要不变，临时目录清理；只替换 rules/去除 provider，不验证原订阅全部规则或握手。

实际构建与检查：

```bash
cmake --build build --target clash-flux test_singbox test_rule_provider_download
./run.sh --version
ctest --test-dir build --output-on-failure \
  -R '^(smoke|profile_links|page_transition|singbox|rule_provider_download|vpn|routing|compensation|sqlite_orm|persistence)$'
build/test_singbox docs/examples/http-providers-2026-10-03.yaml \
  platform/android/app/build/generated/rule-set-assets/rules \
  /tmp/clash-flux-l2-http-audit.json
build/engines/sing-box check -c /tmp/clash-flux-l2-http-audit.json
git diff --check
```

上述检查通过，版本输出 v0.3.19；Android 同环境 `:app:assembleDebug --offline --no-daemon`
通过，日志 `/tmp/clash-flux-l2-http-android-build.log`。没有新增运行内核真实分流、
手机下载/TLS/断网恢复实机验收；多文件缓存提交中途故障没有统一崩溃事务，手机异步
原生失败恢复仍不完整。不能据此宣称 provider 生命周期或 L2 全部完成。本批未发布。

## 本批后续增量：HTTP 请求头（2026-10-03）

仍为 `7bbc149c81b728f9e1717440a29fff447ba13fb0` / v0.3.19 加未提交增量；
L2 协议覆盖保持 **12/15**，本轮推进 provider 字段，未新增 L3 UI。

- `header` 支持普通单值 ASCII 数组映射，如 `User-Agent: [my-client/1]`、
  Authorization、X-Token；保留原值和内部空格，总量上限 16 KiB。异常类型、多值、
  空值、大小写无关重名、控制字符、非 ASCII、首尾空格和传输保留字段整份拒绝。
- 桌面 curl 与 Android JNI/Java 传递同一编译器资源清单；系统证书验证保持开启。
  带头下载不跟随重定向，单独 Field/Approx 记账；无头下载的重定向行为不变。
- 非空 header 的规范化名称与原值进入完整缓存身份；更换鉴权不能复用旧缓存或
  raw pins，失败保留原缓存/调用方计划。空 header 与未声明保持原缓存身份，
  头名称大小写变化保持稳定。请求头值不写进原生 JSON 或保真度/错误诊断。
- `rule_provider_download` 扩至 **15 个真实 curl 场景**，新增实际 UA/鉴权/自定义头
  检查、无头访问鉴权端点失败、非法头联网前拒绝、带头重定向目标零访问；两个
  稳定来源文件和独占暂存清理均断言。编译器回归另覆盖身份变化、失败留旧与快照回滚。
- 本地 **10/10 项项目测试**、**7 份公开样本编译与固定 1.14.2 check**、Android
  `:app:assembleDebug --offline --no-daemon` 通过。请求头样本采用只读种子、禁用网络，
  真实 HTTP 验证来自独立 loopback 回归。没有声称 Android 请求头实机下载或分流验收。

实际执行：

```bash
cmake --build build --target clash-flux test_singbox test_rule_provider_download
./run.sh --version
ctest --test-dir build --output-on-failure \
  -R '^(smoke|profile_links|page_transition|singbox|rule_provider_download|vpn|routing|compensation|sqlite_orm|persistence)$'
# 七份 docs/examples 样本各用 build/test_singbox 输出到独占临时目录，随后：
build/engines/sing-box check -c <临时配置.json>
# platform/android，环境与前批相同：
./gradlew :app:assembleDebug --offline --no-daemon
git diff --check
```

版本验证输出 v0.3.19。构建日志 `/tmp/clash-flux-l2-headers-build.log` 与
`/tmp/clash-flux-l2-headers-android-build.log`。HTTP 指定代理、多值/传输保留头、
MRS、定时热更新与多文件崩溃事务仍未完成。iOS 保持暂缓，明确拒绝非空 header。
本轮未修改数据库/schema/用户订阅文件，未提交或发布。

## 本批后续增量：精确 hosts 与内核运行回归（2026-10-03）

代码快照仍为 v0.3.19 / `7bbc149c81b728f9e1717440a29fff447ba13fb0` 加未提交增量。
L2 协议覆盖保持 **12/15**；本轮补 DNS 字段，不新增 L3 编辑界面。

- 接入精确 hosts 的 IPv4/IPv6 scalar 与完整列表，域名大小写/尾点规范化，规范化
  重名全部拒绝；异常条目按整条 unsupported 记账，保留独立合法映射。通配、别名、
  lan、CIDR、带点分 IPv4 的 IPv6、系统 hosts 开关仍未转换。
- 原生 hosts DNS 用于 A/AAAA 应答（优先于 policy、TTL=10）、节点与 DNS 端点
  domain_resolver、主连接前置 resolve，保留原 server/SNI。false/no/0 的 use-hosts
  或 dns.enable:false 只关应答映射，全局连接解析仍有效。次来源只保留解析依赖。
- 真实内核回归发现普通解析覆盖 hosts 的问题：显式路径绕过 DNS 缓存，普通
  resolve 排除已映射域名；命名空间化后同步 hosts DNS tag，避免后置规则引用旧名。
  多 IP 选择及缓存路径差异记 approx；不冒充全部 hosts 语义保真。
- 新增 `dns_hosts_runtime`：生产编译器（含主来源 ID）+ 固定官方 1.14.2 在独占临时
  目录和 loopback DNS/HTTP/SOCKS 服务运行，**12 场景**实际验证 A/AAAA/多地址、
  缺少对应地址族的空应答、TTL、大小写、非地址查询、子域不扩大匹配、直连、
  节点/DNS 端点解析，以及关闭 hosts DNS 应答、普通 DNS 缓存已存在时连接仍走映射。
- 永久编译回归覆盖非法值/列表原子性、重复域名、布尔关闭/异常、未匹配节点保持
  原解析器、TLS 身份、前置顺序、次来源隔离和命名空间引用。
- **11/11 项项目 CTest** 通过，其中下载回归仍为 15 场景；**8 份公开配置**编译与
  固定内核 check 通过。三份规范化私有订阅副本保留 **32/32、30/30、34/34** 节点，
  通过 check，源摘要不变，临时目录清理；只替换规则/去 provider，不验证原订阅所有
  分流规则或远端握手。Android Debug 构建通过，不代表 Android hosts 实机流量验收。

实际执行：

```bash
cmake --build build --target clash-flux test_singbox test_rule_provider_download
./run.sh --version
ctest --test-dir build --output-on-failure \
  -R '^(smoke|profile_links|page_transition|singbox|rule_provider_download|dns_hosts_runtime|vpn|routing|compensation|sqlite_orm|persistence)$'
build/test_singbox --compile-config docs/examples/dns-hosts-2026-10-03.yaml <临时配置.json>
build/engines/sing-box check -c <临时配置.json>
# 另七份公开样本及三份私有副本使用原有 test_singbox <输入> <GEO目录> <临时输出> 路径
# platform/android，JAVA_HOME / ANDROID_HOME / HUXERUI_HOME 与前批相同：
./gradlew :app:assembleDebug --offline --no-daemon
git diff --check
```

版本输出 v0.3.19；日志 `/tmp/clash-flux-l2-hosts-build.log` 与
`/tmp/clash-flux-l2-hosts-android-build.log`。受控 Linux DNS/连接运行回归通过，
没有新增公网/TUN/Android 实机验收；未改数据库/schema/真实订阅文件，未提交或发布。

## 本批后续增量：加密 DNS 证书参数（2026-10-03）

代码快照为 v0.3.19 / `7bbc149c81b728f9e1717440a29fff447ba13fb0` 加上述未提交增量。
L2 协议覆盖保持 **12/15**；整体仍部分完成，不增加 L3 设置界面。

- DoT/DoH/DoQ、HTTPS 强制 H3 支持显式 skip-cert-verify=true/false；exact 映射
  单端点 tls.insecure，缺省保持证书验证，保留 server/SNI。支持 h3/出站组合，
  不改变 bootstrap/其它服务器或订阅下载的证书设置。
- 非法布尔、重复参数、未知组合与非加密传输整台拒绝并记 unsupported，不保留
  有效参数的前缀。修复非 HTTPS 的 h3=false 被忽略；name-cert-verify 继续拒绝，
  不能用会修改 SNI 的 server_name 代替仅校验证书名称。
- 编译回归覆盖传输/布尔值、缺省、端口/路径/H3/detour 组合、非法值与命名空间，
  并检查 policy、节点 resolver 和显式 IP bootstrap 的端点级隔离。
- 新增 `dns_tls_runtime`：生产编译器 + 固定 1.14.2 + 独占临时证书/loopback 服务，
  **12 个实际 DoT/DoH 场景**。默认与 false 拒绝不受信任证书且没有 DNS payload；
  true 成功。临时信任根仅在测试配置注入，名称正确时 false 成功，IP 名称不匹配
  时 false 仍失败；SNI、HTTP Host 和自定义路径保持原值。拒绝场景同时断言服务器
  收到证书类 TLS alert，不把任意查询超时算为证书校验成功。
- **12/12 项项目 CTest** 通过；保留 hosts 12 场景、curl 下载 15 场景。
  **9 份公开配置**编译与固定内核 check 通过。三份只读私有订阅的规范化临时副本
  保留 **32/32、30/30、34/34** 节点并通过 check，源摘要不变；只替换规则/去除
  provider，不能据此声称原订阅全部规则与远端握手通过。Android Debug 构建通过。

实际执行：

```bash
cmake --build build --target clash-flux test_singbox test_rule_provider_download
./run.sh --version
ctest --test-dir build --output-on-failure \
  -R '^(smoke|profile_links|page_transition|singbox|rule_provider_download|dns_hosts_runtime|dns_tls_runtime|vpn|routing|compensation|sqlite_orm|persistence)$'
build/test_singbox --compile-config docs/examples/dns-tls-parameters-2026-10-03.yaml <临时配置.json>
build/engines/sing-box check -c <临时配置.json>
# 其它八份公开样本与三份私有规范化副本也各自编译/check；临时目录自动清理。
# platform/android，JAVA_HOME / ANDROID_HOME / HUXERUI_HOME 同前批：
./gradlew :app:assembleDebug --offline --no-daemon
git diff --check
```

版本输出 v0.3.19；日志 `/tmp/clash-flux-l2-dns-tls-build.log` 与
`/tmp/clash-flux-l2-dns-tls-android-build.log`。CMake 的 TLS 运行回归要求 Python、
固定打包内核与 OpenSSL CLI，缺少 CLI 会明确报告未注册；本机已满足前提并实际执行。
没有 DoQ/H3 实际握手、公网/TUN/Android 真机流量验收；未改数据库/schema/真实
订阅文件，未提交或发布。证书参数边界已同步到 AGENTS.md 和保真度基线。

## 本批后续增量：DNS 策略通配（2026-10-03）

代码快照仍为 v0.3.19 / `7bbc149c81b728f9e1717440a29fff447ba13fb0` 加未提交增量。
L2 协议覆盖保持 **12/15**，整体仍部分完成；本批补翻译语义，不新增 L3 编辑器。

- nameserver-policy 与节点专用 proxy-server-nameserver-policy 共用完整 ASCII 域名、
  整层 *、前缀 . 与 +.。* 每次只匹配一个非空标签，. 匹配子域并排除根域，+.
  展开为独立根/子域分支；支持中间/连续 * 及前缀组合。
- 从右向左按固定标签 > * > . 排序，保持 Clash trie 选择；后声明只覆盖同一
  规范化分支，Exact 明细记录覆盖。不能用字符串总长度或全局「精确优先」替代。
- 普通 DNS 发出锚定原生 regex，节点编译期选择直接用同一模式元数据，不用另一个
  regex 引擎重解释、不把元数据写入 JSON。合法域名下划线已纳入 DNS 依赖，非法
  节点名称不会被通配选中；未知 geosite:/rule-set:、部分标签通配和非法模式整条记账。
- 永久编译回归覆盖正反声明次序、普通/节点专用策略、规范化覆盖、各层/根域边界、
  不扩大匹配、非法值与模式注入、wildcard 所选 resolver 与 detour 的循环失败。
- 新增 `dns_policy_runtime`，生产编译器（含主来源 ID）+ 固定官方 1.14.2 + 独占
  loopback DNS/SOCKS 服务，实际验证 **63 场景**。按真正收到请求的 DNS 服务断言
  选择，覆盖固定标签优先、根域/单层/多层、大小写/下划线、中间/连续通配及前缀
  组合；两次内核分别开启/关闭节点专用策略，普通查询保持一致，实际 SOCKS 节点
  连接改用对应 resolver；普通 DNS 已缓存节点同名应答时仍查询专用 resolver。
  不是只看生成 JSON 的字符串。
- **13/13 项项目 CTest**、**10 份公开配置**编译/固定内核 check、Android Debug
  构建通过。三份私有订阅规范化临时副本保留 **32/32、30/30、34/34** 节点，通过
  check，源摘要不变；只替换规则/去除 provider，不验证原规则全部语义或远程握手。

实际执行：

```bash
cmake --build build --target clash-flux test_singbox test_rule_provider_download
./run.sh --version
ctest --test-dir build --output-on-failure \
  -R '^(smoke|profile_links|page_transition|singbox|rule_provider_download|dns_hosts_runtime|dns_policy_runtime|dns_tls_runtime|vpn|routing|compensation|sqlite_orm|persistence)$'
build/test_singbox --compile-config docs/examples/dns-wildcard-policy-2026-10-03.yaml <临时配置.json>
build/engines/sing-box check -c <临时配置.json>
# 另九份公开样本及三份私有规范化副本各自编译/check，独占临时目录自动清理。
# platform/android，JAVA_HOME / ANDROID_HOME / HUXERUI_HOME 同前批：
./gradlew :app:assembleDebug --offline --no-daemon
git diff --check
```

版本输出 v0.3.19；日志 `/tmp/clash-flux-l2-dns-wildcards-build.log` 与
`/tmp/clash-flux-l2-dns-wildcards-android-build.log`。未新增公网/TUN/Android 真机
DNS 流量验收；hosts 通配/别名、DNS 规则集策略、fallback-filter/FakeIP/direct
follow-policy 仍未完成。未改数据库/schema/真实订阅文件，未提交或发布；约束已同步 AGENTS.md。

## L3 目标目录首批交付（2026-10-03）

- 桌面宽弹窗与窄窗口独立编辑页复用 `RuleTargetPicker`。先选来源，再选默认出口、
  策略组或节点；搜索为原名的字面子串匹配，每次最多展示 200 项并保留当前有效选择。
- 目录只读订阅文件（上限 8 MiB、限制在订阅根），复用来源导出编译；未参与编排的
  Clash 来源也可读取。不请求 provider、不生成候选文件、不发布运行/预览映射。
  读取/编译在任务线程，写 State 回 UI；用请求序号、来源身份与持久层修订号丢弃过期结果。
- 来源/类型改变后清空对象；搜索变化不改引用。失效旧名称保留且不自动选择第一项，
  原样引用可继续编辑不可用策略或禁用；新引用必须来自当前目录，最终保存仍走领域候选检查。
  对象身份仍为 source ID + kind + 原始唯一名称，没有对象 UUID 或改名追踪。
- 手机、PPTP/OpenVPN 和原生 JSON 保留默认出口。JSON 次来源编排仍不支持；目录不是
  内核运行状态或远端握手验收。读取失败保留订阅文件和数据库记录。
- 实际验证：Linux `cmake --build build --target clash-flux test_singbox test_page_transition`
  （含 UI codegen）、`./run.sh --version`（0.3.19）通过；`ctest --test-dir build
  --output-on-failure --no-tests=error -L clashflux-required` **15/15** 通过。
  singbox 增加未参与来源、同名隔离、失败不发布部分目录与不准备 provider 的回归；
  page_transition 增加生产下拉/文字输入、搜索超出 200 项、来源切换、失效保留、默认出口
  与类型/来源校验。异步结果隔离为代码复核，未声称完成整页异步乱序实机验收。
- Android `:app:assembleDebug --offline --no-daemon` 通过；只构建，未安装/真机验证。
  Windows/macOS 本批未执行；无本批真实 GUI/TUN/远端线路验收；`git diff --check` 通过。
  未提交、未发布。构建/测试日志前缀 `/tmp/clash-flux-l3-target-picker-`。

## 历史首批验证记录与复现（v0.3.16 工作区）

| 验证 | 结果/范围 |
|---|---|
| Linux 主目标 + HuxerUI codegen | `cmake --build build --target clash-flux` 通过 |
| 应用版本 | `./run.sh --version`：v0.3.16，退出码 0 |
| 应用测试 | smoke/profile_links/page_transition/singbox/vpn/routing/compensation/sqlite_orm/persistence：9/9 通过 |
| 编译产物内核检查 | 配套 10 协议样本扩充原文凭据、WS header、全局/节点指纹；生产编译器生成 JSON，固定 1.14.2 `check` 退出码 0 |
| 定向语义检查 | 修复前复现 F01–F03；修复后永久回归通过（包括 OpenVPN LF/CRLF 凭据和来源指纹隔离）；前次多来源/不可用策略/循环及 JSON 次来源拒绝符合上述边界 |
| Android 本地 Debug | `:app:assembleDebug --offline --no-daemon` 成功；versionName 0.3.16、minSdk 24；AAR 身份和摘要匹配，APK 含 GEOIP/GEOSITE CN |
| APK 签名 | `apksigner verify --verbose --min-sdk-version 23`：v1/v2 均为 true；这是 Debug 签名，不是发布密钥验收 |
| Linux 唤醒/IPC | 本轮会话中冷启动预填窗口正常，编码 token 保持原样；20 个并发转发退出码全为 0，串行保护后无二进制 footer 泄漏；未完成表单确认下载的端到端验收 |
| 工作区检查 | `git diff --check` 通过；未提交、打标签、推送或发布 |

当前 CTest 共注册 32 项（包含上游 HuxerUI），其中 10 项缺少可用命令/可执行文件。
本轮执行的是上述 **9 项项目测试**，没有宣称整个 32 项套件通过。F01–F03 已加入
永久回归；其它前次编排语义探针仍是本地审查样本。

可重复的正向配置检查：

```bash
cmake --build build --target clash-flux test_profile_links test_singbox
./run.sh --version
ctest --test-dir build -R '^(smoke|profile_links|page_transition|singbox|vpn|routing|compensation|sqlite_orm|persistence)$' --output-on-failure

# 先按 Android 构建文档生成固定 GEO 资产，或使用同名的已校验缓存目录。
build/test_singbox docs/examples/layers-audit-2026-10-02.yaml \
  platform/android/app/build/generated/rule-set-assets/rules \
  /tmp/clash-flux-layers-audit.json
build/engines/sing-box check -c /tmp/clash-flux-layers-audit.json
git diff --check
```

本次没有新增远程线路握手、实际 DNS/分流行为、吞吐/功耗/后台长期运行验证；
没有在 Windows/macOS 编译或运行最新工作区，也没有安装本次 APK 进行 Android
浏览器 intent、返回手势、VPN 重建验收。历史真机性能记录不等于本轮改动已实测。

## 下一步交付与完成条件

| 优先级 | 任务 | 可以标成完成的条件 |
|---|---|---|
| P1 | 扩展订阅保护跨平台/崩溃验收 | R01–R03 已修复并通过本地失败/交错/重开回归；继续 Windows/macOS/Android 失败启动与生命周期验证，不把它当作完整 journal |
| P1 | 继续清点输入类型/默认语义 | R04 已修复；继续逐字段覆盖 DNS/协议/组输入类型、默认值及拒绝策略，补永久回归和固定内核检查 |
| P1 | 补桌面计划与手机生命周期验收 | 切主、启停规则、刷新/删除、失败回滚、进程重建有可重复场景；Windows/macOS/Android 各自留实际记录 |
| P1 | 外部 provider 与原生规则集 | 下载来源、原子提交、格式转换、顺序、失败留旧、刷新/删除均有定义与测试；再开放管理入口 |
| P1 | 完成桌面编排最终形态 | JSON 次来源、跨来源链、对象目录/身份迁移、rule ID 追溯、持久恢复 journal；完整验收，不扩展手机普通多订阅 |
| P2 | 原生能力产品化 | 优先原生 DNS/高级拨号和新的 endpoint；每个连接对象包含字段、状态、路由、启停与恢复，不以 JSON 可导入冒充编辑器完成 |

`urltest` 手动锁定、原生 fallback/load-balance、真实 providers 面板等固定内核没有的
能力归为“不做”，不放进待修 bug 或待完成百分比。iOS 是否恢复支持由后续项目决策
决定，本清单不产生 iOS 发布承诺。


## 2026-10-09：Windows 原生服务分离

代码快照：v0.3.29 发布候选，包含当前全部应用改动，排除 traces 调试文件。Windows 新增 SCM
`ClashFluxService` / LocalSystem 服务，通过受限命名管道控制 sing-box、PPTP/RAS
与 TUN 路由补偿；GUI/CLI 保留数据库、订阅和当前用户系统代理，TUN 不提权 GUI。
服务管理走 UAC 子命令、原始用户 SID 授权、受保护 Program Files 载荷与版本门禁；
正常更新失败尝试恢复旧载荷，不承诺完整多文件崩溃事务。详细契约见
[Windows 服务](windows-service.md)。

新增 `service_protocol`（所有桌面）与 `windows_service`（Windows 管理员环境）
回归，项目门禁现在为通用桌面 16 项、Windows 17 项；MSVC C4737 仅豁免直接 ORM，
Windows 仍必跑 16 项。原生服务测试使用隔离服务和假内核，不代替真实 VPN 流量验收。

本批实际验证：

- `cmake --build build --target clash-flux`（包含 HuxerUI codegen）与
  `./run.sh --version` 成功；发布候选版本为 0.3.29。
- 构建 `test_service_protocol`、`test_vpn`、`test_compensation`、`test_pptp_connect`
  及其余项目回归目标；`ctest --test-dir build --output-on-failure --no-tests=error
  -L clashflux-required`：16/16 通过。
- LLVM-MinGW 20261006：原生 `windows_service.cpp` 和 Windows SCM 测试程序以
  `-std=c++20 -Wall -Wextra -Werror` 编译、链接为 PE 成功；假内核 PE 编译成功。
  Windows 的 config/vpn/pptp/vpn_compensation/service/singbox/core 接口，以及
  `pptp.cpp` / `core.cpp` 实现交叉编译成功。
- 扩大到原有 `vpn_compensation_windows.cpp` 时，MinGW 的 `windns.h` 缺少
  `DNS_ADDR_ARRAY`、`DNS_QUERY_REQUEST`、`DnsQueryEx` 声明而失败；本批不改 DNS
  后端来规避 SDK 差异。因此不能宣称完整 Windows 构建已通过。
- `git diff --check` 通过。

完整发布候选的 [GitHub CI](https://github.com/FarnaHerry/Clash-Flux/actions/runs/37951900040)
已通过 Windows/MSVC Release 构建及全部项目回归（含原生 SCM 服务），Linux x64、
Linux ARM64、macOS ARM64 与 Android 也通过。标签发布再执行门禁及 WiX 安装器构建。
iOS 暂缓路径的现有 Xcode 工程解析失败仍为非阻塞，不生成 iOS 发布包。

未新增 Windows GUI/UAC/TUN 数据流/PPTP 实机验收；本机未安装真实服务，未更改
当前机器代理、数据库与订阅。


## 2026-10-26：v0.3.70 CI 修复与运行时弃用告警

代码快照：`v0.3.70` / `58df82517b128c1ae889e9100c24b089ae049e0a`。失败的首次标签 CI
`40292493304` 缺少 `<algorithm>`，导致 Linux/macOS 无法编译规则集缓存；Windows
个人规则导入回归用窄字符路径检查中文文件名，触发代码页转换异常。修复分别在
`d1d52cb` 与 `58df825`。修复后的主分支 CI `40300803363` 与标签发布 CI
`40313619869` 均成功。

标签 CI 通过 Linux x64/arm64、Windows x64、macOS arm64、Android APK 签名校验、
Windows Burn 安装器构建及发布汇总；Android、Windows 安装器、Linux DEB/RPM 与归档、
macOS 归档共 10 项资产已出现在 [v0.3.70 Release](https://github.com/FarnaHerry/Clash-Flux/releases/tag/v0.3.70)。
iOS Simulator 诊断 job 通过，但不生成发布包；没有执行 Windows 安装/升级实机验收。

本机实际检查：VS 2026 MSVC 下 `cmake --build build --target clash-flux`（CMake
4.4.2，Ninja 无待编译项）、终端 `./run.ps1 --version`（输出 v0.3.70）、
`git diff --check` 均通过。此前修复后定向的 10 项 CTest 为 10/10。

发布 CI 的 tag 快照仍有 Node 20 弃用告警：checkout v4、artifact v4。工作区已将
checkout/cache/setup-go/setup-java/upload/download-artifact 更新到 Node 24 兼容版本；
该工作区改动尚未推送，需后续 CI 实际运行确认新版 action 与发布流程兼容。


## 2026-10-12：服务变化后的 PPTP 能力恢复

代码快照：v0.3.29 工作区修复，未提交/发布。服务分离后，PPTP 适配器只在
构造时探测一次可用性；GUI 在服务安装或恢复前启动时，旧的不可用结果会永久
留在引擎选择与订阅卡片中。现有原生连接监视线程持续检测能力，使用
`VpnManager::setEngineAvailable` 更新选择，不替换适配器或断开其它连接。

能力与连接状态经同一个 `VpnSnapshot` 发布到应用级 `VpnModel`。未拨号的
订阅也读模型能力，页面不再直接构造/查询 VpnStore。显式连接先检查服务版本、
启动已安装的停止服务并重查能力，再修改主 TUN；失败保留主内核。具体失败
原因优先于通用的“PPTP 引擎不可用”。未改变 RAS 认证、凭据、路由与服务所有权。

实际验证：`cmake --build build --target clash-flux test_vpn`、
`./run.sh --version` 通过；完整目标构建后 `ctest --test-dir build
--output-on-failure --no-tests=error -L clashflux-required` 为 17/17。新增 VPN
回归实际验证不可用→可用→不可用→可用、拨号与路由执行，以及其它连接和已有
会话不被能力刷新重置。隔离临时数据目录的 GUI 连续运行 12 秒，未提前退出。
`huxerui build android --profile debug --source third_party/huxerui` 构建通过，
确认共用模型/Legacy 路径与 UI codegen 仍可编译；不把 APK 构建当作 VPN 实机验收。

Windows SCM 回归新增安装前后、长操作中与重启后的 PPTP 能力查询检查，本机
未执行该 Windows 专属用例；未新增 Windows 实机 RAS/PPTP 连接验收。

## 2026-10-26：规则页精简说明文字

代码快照：`58df825` 上的未提交工作区。删除订阅规则页的主订阅提示、列表视图名称，
以及规则集页的共享资源库说明；同步删除两条闲置文案的 default/en/zh 资源。
Windows/MSVC、CMake 4.4.2 下构建 `clash-flux test_page_transition`，确认规则页经过
HuxerUI codegen；`page_transition` 与 `ui_resources` 为 2/2，终端
`./run.ps1 --version` 输出 v0.3.70，`git diff --check` 通过。未执行 GUI 人工检查。

## 2026-10-26：首页卡片无描边与悬停试样

代码快照：`58df825` 上的未提交工作区。首页网格和桌面运行状态卡改用生产
`HomeCardSurface`，无描边；框架 Indication 在内容下方淡入 5% primary 悬停底色。
保留固定卡片尺寸与独立子控件命中，不给静态卡片添加点击动作。
Windows/MSVC、CMake 4.4.2 下构建 `clash-flux test_page_transition`（含首页 codegen）
通过，`page_transition` / `ui_resources` 2/2；`./run.ps1 --version` 输出 v0.3.70，
`git diff --check` 通过。用户接手 GUI 效果检查，本轮未启动或操作应用界面。

## 2026-10-26：规则页间距与首页卡片按压反馈

代码快照：`58df825` 上的未提交工作区。规则页移除无内容提示的空布局节点，
来源选择行到列表、页签到内容统一采用规则卡片的 4pt 间隙。首页卡片观察主指针，
按下缩至 98.5% 并下移 1pt，松开/取消后弹回；reduced motion 关闭变换。
Windows/MSVC、CMake 4.4.2 下 `cmake --build build --target clash-flux test_page_transition`
成功（含规则页与交互回归 codegen），`page_transition` / `ui_resources` 2/2。
新增生产卡片回归覆盖深浅主题、布局尺寸不变、取消/松开恢复、子按钮仅点击一次及
reduced motion；`./run.ps1 --version` 输出 v0.3.70，`git diff --check` 通过。
GUI 外观由用户检查，本轮未操作应用界面。

## 2026-10-26：猫咪卡片边框与远程订阅准备性能

代码快照：`58df825` 上的未提交工作区。手机/桌面 Compact 的猫咪运行标题卡去掉描边，
保持 raised 表面、nested 圆角和 48pt 标题槽；生产绘制回归继续验证展开、收缩与滚动位置。

用用户提供的私有订阅在本机临时目录复现：订阅本体 21,260 字节、32 个节点，
11 个 HTTP provider 加 2 个 ASN 快照共 642,598 字节，生成 JSON 1,957,725 字节。
首次订阅请求 6.119s，其中首字节等待 6.115s；网络等待与下述离线准备测量分开。
原文、响应头、生成配置和诊断日志未加入仓库。

编译器把本次从磁盘读取且已成功转换的原文快照交给准备层，避免重复读取/校验和相同字节重编译；
已固定的 options 原文不额外保留副本。到期刷新仍执行，字节变化重新编译，完整候选检查和失败留旧不变。
语法预检完整检查各条目但不累积废弃 JSON 树；classical 多个条目的 ASN 依赖同轮发现，整份集合仍须全部成功。

Windows/MSVC Release 同机交错 A/B：旧可执行文件保存在临时目录；网络获取替换为同一份本地资源字节。
热缓存预热一轮后各四次，中位数 **531.159ms → 207.039ms**；冷缓存各三次，
中位数 **738.368ms → 500.090ms**。新版本热缓存编译一次并复用 13 份已验证快照，
冷缓存编译三次。全部输出 JSON 的 SHA-256 相同，并通过固定 sing-box 1.14.3 的实际 `check`。
这些是桌面离线准备进程耗时，不包含下载、Android 后台启动或手机性能数据。

实际验证：构建 `clash-flux test_singbox test_rule_provider_download test_page_transition test_persistence`，
通过 codegen；`page_transition`、`ui_resources`、`persistence`、`project_test_gate` 通过，
`singbox`、`rule_provider_download` 最终 2/2（下载回归使用明确 OpenSSL 和 `--require-tls`）。
新增回归核对缓存复用的 JSON/账本一致性、到期同字节/新字节处理、ASN 批量发现及失败零提交。
Android Legacy 全量原生实现经现有 GUI 工具链 NDK 29.0.14206865、arm64-v8a/API24 语法检查通过；
未重新打包 APK，未连接手机、未执行真机或 GUI 人工验收。版本命令输出 v0.3.70，`git diff --check` 通过。

## 2026-10-26：规则资源有限并发、条件刷新与转换缓存

代码快照：`58df825` 上的未提交工作区，延续前一节性能修复。
生产 HTTP/ASN 准备最多 3 个工作线程，每轮先核对/去重完整身份，各资源独占暂存文件；
全部线程结束后按原顺序检查并提交。失败/异常不会提前清理仍被写入的暂存目录。
新增 ETag/Last-Modified 条件刷新，校验值与正文使用同一原子 v2 镜像，保留 v1 读取；
304 只接受对应已验证正文，候选拒绝时不更新正文、校验值或到期时间。
条件头不跨跳转传播，跳转时回到原始请求策略完整获取。

domain/ipcidr 的 YAML/text/MRS 完整转换使用有界进程内 LRU，逐字节核对内容、格式与 behavior；
不缓存上下文相关的 classical/ASN，不持久化跨版本编译产物，不改变来源账本、原订阅或生成规则。
最多 32 项，估算计费上限 32 MiB；退出后加速缓存自然丢弃，磁盘 raw/运行快照仍保留。
本轮没有实现 provider 到本地 SRS 的生成，也未使用预置 SRS 替换来源规则。

同一份私有离线样本，Windows/MSVC Release 新旧进程交错各 3 次，冷缓存完整进程中位数
**755.520ms → 345.981ms**；旧程序是前一节优化前保留的快照，不是紧邻本轮的工作区版本。
新程序冷准备仍编译 3 次，13 个资源/49 条保真度记录，输出 JSON 的 SHA-256 与前一节完全相同，
并再次通过固定 sing-box 1.14.3 的实际 `check`。
新程序同进程重复准备 5 次，每次重新从磁盘固定 raw 输入：转换缓存命中 10 个 MRS 集合，
准备函数耗时 34.232–39.314ms，中位数 **36.655ms**；该数字不含进程启动、网络或 Android 后台启动，
不能直接与前一节完整进程的 207.039ms 比较。未测真实并发公网下载收益或手机耗时。

构建 `clash-flux test_singbox test_rule_provider_download test_persistence` 成功；
`singbox`、`rule_provider_download`、`persistence` 3/3。
新增回归覆盖 3 个实际重叠工作线程、失败/异常等待与零提交、原快照保留、转换缓存内容/格式/来源隔离、
无效尾部拒绝、有界淘汰、并发编译、304 原文保留与候选失败留旧、v1/v2 兼容及校验值注入拒绝。
生产 curl 回归覆盖带 representation Content-Length 的 304、不接受非条件 304、条件头不随跳转传播，
继续明确使用 OpenSSL CLI / `--require-tls` 验证默认证书拒绝。

Android Legacy 全量原生代码经 NDK 29.0.14206865、arm64-v8a/API24 语法检查通过；
从生产 MainActivity 原样提取的 HTTP 方法经 JDK 17 / Android API36 `javac` 编译检查通过。
完整 `:app:compileDebugJavaWithJavac --offline` 在本机缺少固定版本 libbox AAR/元数据的
`stageSingboxAar` 门禁停止，未绕过门禁或伪造产物；未完成整应用 Java 构建、APK 或真机验收。
版本检查输出 v0.3.70，基础构建与 `git diff --check` 通过。未提交、打标签或推送。

## 2026-10-27：首页服务安装后继续原运行操作

代码快照：`82b8830` 上的未提交工作区。原先权限检查失败会先结束首页命令，再单独安装
服务，安装成功没有继续开启 TUN/启动内核，导致开关恢复旧意图。本轮将安装纳入应用级
运行操作，确认安装成功且服务存在后只重试一次原命令，再回读发布权威状态。安装期间
保留共享 pending 与原开关目标值；设置页安装/更新仍只管理服务，取消或失败不应用原命令。

Windows/MSVC Release 执行 `cmake --build build --target clash-flux test_page_transition`
成功，包含 HuxerUI codegen。`page_transition`、`ui_resources`、`service_protocol`、`vpn`、
`routing`、`compensation` 共 6/6 通过。新增无窗口回归使用生产命令续执行函数，覆盖安装
成功、取消、异常、后续命令失败、单次重试、冲突操作拒绝、发起视图卸载/缩放、旧轮询
拒绝及失败后重试。`./run.ps1 --version` 实际输出 v0.3.71，`git diff --check` 通过。
未执行真实 UAC 安装/取消、SCM 管理员回归、GUI 人工交互或真实 TUN 数据流验收，
未验证 Linux 运行；上述无窗口回归不等于平台端到端验收。

## 2026-10-27：首页流量方向专用配色

代码快照：`82b8830` 上的未提交工作区。`theme_colors.h` 新增 `TrafficColors` /
`ResolveTrafficColors()`，首页流量与流量统计统一使用上传橙、下载蓝，分别适配深浅主题，
不再复用强调色/警告色；下载面积填充仍为下载色的 18% 透明度。浅色上传/下载分别为
`#C47A00` / `#148BDC`，深色分别为 `#FFB84D` / `#3FB8FF`。

Windows/MSVC Release 构建 `clash-flux test_page_transition` 成功，包含 HuxerUI codegen；
`page_transition`、`ui_resources` 共 2/2 通过，版本检查实际输出 v0.3.71，
`git diff --check` 通过。已尝试启动构建产物进行 GUI 检查，但现有安装版单实例接管了
启动请求，未替换用户当前实例，因此新配色的实际 GUI 外观仍未验收；未验证手机平台。

## 2026-10-27：顶部搜索胶囊聚焦描边展开

代码快照：`82b8830` 上的未提交工作区。桌面非 Compact 的 `PillSearchField` 保留普通
描边，由 `WithSearchFocusOutline` 在上层绘制主题主色描边，以 250ms EaseOut 从左向右
展开矩形裁剪，上下边同步显现。MotionController 只使保留前景绘制失效，完成后不请求帧；
失焦立即清除，后台和 reduced motion 直接定位最终状态，手机/Compact 行为不变。

Windows/MSVC Release 构建 `clash-flux test_page_transition` 成功并通过 HuxerUI codegen；
首次链接因产物临时占用报 LNK1104，重试后成功。`page_transition`、`ui_resources` 通过；
新增生产绘制组件回归覆盖深浅色渐进/结束、重组次数与布局不变、失焦、快速重入、
缩放、后台和减少动态效果。`project_test_gate` 首次因普通 Shell 的 PATH 缺少 Ninja 失败，
在 MSVC 开发环境中重跑通过。`./run.ps1 --version` 实际输出 v0.3.71，`git diff --check` 通过。

使用构建目录内的独立 GUI 预览复用同一生产描边函数，检查深浅色聚焦、输入和清空正常；
不接数据库、代理或服务，不作为主应用全流程/手机验收。未做 CPU/GPU 定量性能测量。

## 2026-10-27：订阅切换幂等、取消范围与准备产物复用

代码快照：`82b8830` 上的未提交工作区。卡片/托盘统一使用应用级 `LaunchProfileSelection`：
相同待切 ID 的重复点击合并，只执行一份准备并保留最后一个待切目标；改选取消旧准备，
旧完成/异常按请求序号（含 ABA）拒绝发布，不能清除新 pending 或覆盖新选中状态。
取消发生在编译条目和准备阶段边界，单次 YAML/JSON 解析、JSON 序列化和下载不会被强制
中断；已经开始的内核交接继续完成或回滚。切回仍活动的来源不重新编译它。

此次取消令牌只属于显式切换：Android `AndroidRefreshProfilesDueOnce -> completeRemote`
与桌面 `refreshDue -> refresh` 不接收此令牌，也不进入切换队列。后台更新继续逐条完成
下载、候选检查和原子提交；更新与切换共享运行锁，可能相互等待，不能相互取消。
新增两个独立来源的准备回归确认切换令牌被取消不影响后台候选和自有缓存快照。

切换候选检查与运行交接复用同一份 PreparedStart，不再先 validateRoutingPlan 再从头
restartCore；成功提示直接取候选账本，代理组预览沿用候选快照及完整文件/数据修订指纹。
准备过程保留 JSON DOM，启动不再重新解析候选字节；Android 不读取/解析桌面驻留实例的
旧 config.json。代理组选项未实际改变 selector default 时保留编译字节，不再次 dump；
改写选中项后仍序列化并校验最终字节。大型来源目录与代理快照按所有权移动，避免额外复制。
HTTP/ASN 依赖发现、有效新快照和 GEO 输入/目录变化仍按需要重新编译；HTTP 检查阶段
已完成的 GEO 重编译结果传递给后续准备，不丢弃后再次编译。刷新/文件编辑的提交前检查
与提交后应用边界未被跳过，也未将独立来源只读报告替换成其它来源的运行报告。

实际命令与结果（Windows/MSVC Release）：

```powershell
cmake --build build --target clash-flux test_page_transition test_singbox test_persistence test_rule_provider_download
ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources|singbox|persistence|rule_provider_download|personal_rule_sets_runtime|service_protocol|vpn|routing|compensation|project_test_gate)$'
python tests/test_profile_selection_runtime.py build/health-evidence/clash-flux.exe
./run.ps1 --version
git diff --check
```

构建成功，包含 HuxerUI codegen；11/11 CTest 通过。新增 UI 回归覆盖重复点击只执行一次、
单任务上限、替换中间目标、返回活动项、ABA、异常后重试、视图卸载和缩放。新增编译回归
覆盖预取消、下载/检查后取消零提交、暂存退役、后台来源隔离、最终校验字节复用和 selector
幂等。之前版本的 Python 定向回归曾使用 CMakeCache 中的 Python 3.14 验证一次切换只记录
一份完整准备，以及失败时保留 profile/config 文件。扩展后台更新顺序用例后，当前脚本尚未
成功重跑：本机现有 Windows 单实例使初始化命令无法确认使用临时数据根；脚本在隔离数据库
核验处退出，没有执行 profile、文件或 config 修改。该 Python 回归未注册 CTest。
Windows 链接曾被开发版占用，用户授权后实例退出，随后构建成功。版本输出 v0.3.71，
`git diff --check` 通过，根 AGENTS 与各 SKILL.md 的行数/字符限制检查通过。

通过现有 AndroidLegacy.cmake 生成兼容源码，NDK 29.0.14206865 的
`clang++ --target=aarch64-linux-android24 -std=c++23 -fsyntax-only` 检查完整原生实现与
订阅卡片 UI 源码成功；未打包 APK、未执行 Android 真机后台更新/快速切换/数据流验收，
未执行 Windows 管理员服务/WFP 回归，未测手机耗时、CPU/GPU 或内存峰值改善比例。
## 2026-10-27：统一大文本编辑与预览

代码快照：应用 `82b8830` + 当前工作区；HuxerUI 固定 `3a6c18e4d5141b27f3671e33baa42b61550f83cd`，
Lib-CodeEditor 固定 `984d2886b04b77cffa162a2934766f5712929704`。

- 订阅原文的桌面弹窗/手机页面、规则集 JSON、编译 JSON、诊断报告统一使用
  `TextDocument` / `DocumentTextView`；单行字段和结构化实时日志仍使用各自控件。
- 文档 UTF-8 拆行在工作线程单飞准备，`initial_document` 避免 UI 声明复制全文；
  输入只记脏，保存/复制按需生成共享快照，文档内容跨卸载保留。规则集列表/表单共享资源引用，
  整库复制与保存组装在工作线程；订阅文件保存失败保留编辑内容并拒绝重复提交。
- 用户授权后通过 gh 凭据向 FarnaHerry/Lib-CodeEditor 的 farna/main 推送必要修复：
  固定子模块声明、预构建文档接口、Windows 公共头宏传播、关闭默认输入追踪、
  工具栏剪贴板初始化/只读保护/实际结果与统一文本变更通知、库图接口目标早退。
- Windows Release 主目标与 HuxerUI codegen 构建通过；`run.ps1 --version` / `git diff --check`
  收尾检查通过。CTest 的 page_transition、ui_resources、singbox、persistence、
  cps_dependencies、rule_provider_download、personal_rule_sets_runtime 共 7 项通过；
  扩展库独立验证 15 个用例、58 条断言通过（包含真实无窗口剪贴板及 IME 通知）。
- 生产组件回归覆盖 30,000 行文档、中文/emoji、输入不重组页面、快照复用、卸载重挂载、
  只读复制和剪切/粘贴后的保存快照、搜索关闭后一次点击重开。Windows 独立 GUI 验证
  20,001 条规则、中文/emoji 输入、深浅色切换、未保存内容重开，以及 1 MiB 单行显示/搜索栏布局。
- Android NDK 29 arm64 API 24 对六个受影响 UI 单元和扩展源码的语法检查通过。
  库图配置通过；重复下载挂起后复用了经 HEAD/干净状态/子模块版本核对的固定构建缓存，
  未改来源 URL/COMMIT，也未消费可变维护工作树。
- 未完成边界：Android APK/真机输入法、手机端耗时和峰值内存、其它桌面平台未实测；
  首次视口布局、字体测量和显式取全文仍在 UI 线程，不将上述检查等同于所有设备的大文件性能验收。

## 2026-10-27：v0.3.72 发布候选

代码快照：`v0.3.71` / `82b8830` 加本次工作区改动。候选版本 `0.3.72`，Android
`versionCode=150`，与 CMake `project(VERSION)`、Gradle `versionName` 一致。

- Windows / MSVC Release、Ninja、HuxerUI codegen：`cmake --build build --target clash-flux`
  及全量 `cmake --build build --parallel` 均成功；`clash-flux.exe --version` 输出
  `Clash-Flux v0.3.72`；`git diff --check` 通过。
- `ctest --test-dir build --output-on-failure --no-tests=error -L clashflux-required --timeout 300`
  25 项中 23 项通过。`windows_service` 因 service pipe 不可用失败；
  `windows_physical_dns_wfp` 因无法添加 WFP sublayer 失败。本地 Shell 为 medium integrity，
  未提升；这两项需要管理员测试环境。隔离的 `legacy_bundle_cleanup` 回归通过。
- Android APK、其余多平台构建与 Release 资产尚未由本机验证，需由候选提交的 CI 检查。
- 本机未执行 Windows Burn 的跨版本安装、失败回滚、静默卸载与维护页验收：Windows Sandbox
  不可用，且检测到本机应用进程；安装可能影响运行中的服务/代理。用户随后明确要求直接
  推送 tag 运行发布 CI、失败修复后重打，并要求本机暂不安装；隔离环境安装/升级/回滚/卸载
  验收仍未完成。

## 2026-10-27：v0.3.72 首轮 CI 失败与重验准备

代码快照：应用 `7ebfd30` + 本次修复；Lib-CodeEditor 固定更新到
`a332e7c3158818a891bb8181756d439a62df6268`。

- 首轮 tag CI `40461865066` 的 Windows、macOS 构建与 Windows 安装器成功；Android 因
  AndroidLegacy 未生成 `title_bar_search.cpp` 中的搜索焦点实现而链接失败。已将该源文件
  和头文件加入输入依赖，并将实现加入 Android UI 源码图。
- Linux x86_64/arm64 因 CodeEditor 合并的预编译 simdutf 对象要求 libstdc++ 新符号而失败。
  CodeEditor fork 现在固定 simdutf v7.7.0 源码提交，并仅在 Linux 按消费工具链构建，避免
  与应用 libc++ 混用。第一次重打 tag 的 Linux arm64 job 随后发现 upstream 头文件布局与
  SweetEditor include 路径不同；fork `2aa9955` 补上同版本兼容头目录。第二次重打 tag 的
  Linux 两架构 configure 随后发现该头目录进入了 simdutf 的安装导出接口；fork `a332e7c`
  将其限制为 build-only include。iOS Simulator 诊断
  同时暴露静态库已合入 simdutf 却继续传递该目标的链接边；fork 已从静态库链接闭包移除
  该冗余边。iOS 仍是非阻塞诊断平台，等待新 CI 确认。
- 固定到 CodeEditor `a332e7c` 后，Windows/MSVC Release 主目标和 HuxerUI codegen 构建成功；PTY 下
  `./run.ps1 --version` 输出 `Clash-Flux v0.3.72（sing-box 客户端）`；`git diff --check`
  通过。第二轮流水线因 Linux include 缺失取消；第三轮 `40467591353` 的 Linux x86_64 与
  arm64 configure 已失败，现有 Linux 修复待新 tag CI 复核 Android/Linux/iOS。
- 本机 Windows 安装验收仍未执行，按用户要求等待隔离测试环境；不得将构建或 CI 等同于
  Burn 升级、回滚、卸载验收。

## 2026-10-28：v0.3.73 发布候选验证

代码快照：应用 `d43769c` 加当前工作区；候选版本 `0.3.73`，Android `versionCode=151`；
HuxerUI 固定 `be67da4a898856382bb8d4876a6c8ce71d889f5e`。

- Linux Clang 21 Release 主目标与全量构建通过；`./run.sh --version` 输出
  `Clash-Flux v0.3.73（sing-box 客户端）`；`clashflux-required` 22/22 通过，
  覆盖订阅刷新、规则页分页、运行时健康检查、DNS/TLS 与持久化。
- Windows/MSVC Release 主目标和全量测试构建成功；PTY 下 `./run.ps1 --version`
  输出 `Clash-Flux v0.3.73（sing-box 客户端）`。`clashflux-required` 为 23/25；
  `windows_service` 和 `windows_physical_dns_wfp` 需要管理员上下文，本机未提升，交由正式 CI 验证。
- HuxerUI fork Windows 全量构建及 CTest 24/24 通过。macOS shared 原生 bundle 构建成功，
  但原生 lifecycle CTest 为 10/12；frame failure 关闭后窗口释放断言失败，windowless runtime
  retirement/reopen 超时。键盘派发关闭测试通过，诊断步骤随后超时；这两项仍未解决。
- Android 对受影响 UI 单元和 HuxerUI 扩展源码的 NDK 29 arm64/API 24 语法检查通过；
  本机未打 APK 或做手机实机验证。`git diff --check` 通过。
- 根据用户要求，本机不运行 Burn 安装。跨版本旧版升级、失败回滚、静默卸载和维护页仍未实机验收；
  本地构建、隔离清理回归及发布 CI 不替代该验收。

## 2026-10-28：HuxerUI 官方系统外观接口与猫咪动作 Demo

代码快照：应用 `7fcc273` 加当前工作区（保留原有未提交修改），应用版本 `0.3.75`；
HuxerUI 固定 `ce2a671832152bf2376d331cb81f20c59f0a6875`，本机独立 checkout 与 CI/Android pin 相同。

- HUI 应用开发技能及新公开头文件已核对；自动主题改用 `UseSystemThemeBrightness()`，
  当前主题深浅使用 `ThemeSpec::brightness`。显式模式、强调色与启动只读主题设置保留。
- Windows/MSVC Release 主目标及全量测试目标完整重建通过，避免旧对象继续消费
  `CreateApplicationWindow` 虚函数和旧 `ThemeSpec` 布局。初轮增量构建暴露旧对象链接/主题断言，
  完整重建后生产 `page_transition` 通过；版本命令实际输出 `Clash-Flux v0.3.75（sing-box 客户端）`。
- `ctest --test-dir build --output-on-failure --no-tests=error -L clashflux-required` 为 23/25；
  `windows_service` 报 `Service pipe unavailable (Win32 2)`，`windows_physical_dns_wfp` 无法添加 fixture sublayer。
  当前进程未提权，未禁用两项门禁。CPS 重定位/版本拒绝/缓存完整性、DNS/TLS、分页/主题及 persistence 均通过。
- HUI 独立维护仓库完整重建通过；24 项 CTest 中 20 项首轮通过，四项配置/构建夹具因未加载
  MSVC 环境失败，加载开发环境后 `--rerun-failed` 4/4 通过。Windows 原生测试通过；
  这些结果不等于 macOS/Linux/Android 原生或手机实机验证。
- Windows 安装器 UI 源码经 HuxerUI codegen 和 MSVC `/Zs` 检查通过，未执行 Burn 打包或安装。
- 隔离 Demo 新增 `--tricks` 动作实验室，八个动作使用保留帧时钟，支持速度、重播与静态逐帧检查；
  补齐躯干、后腿、前爪与尾巴连接，正式应用 Logo 和生产运行状态映射保持原样。
  Demo 使用同一固定 HUI 源码，Windows Debug 原生构建及 GUI 检查通过；补齐下半身后
  `box_cat_demo.exe --tricks --smoke-test` 验证八动作、终态停帧、快速重播、减少动态效果及逐帧检查通过，
  原 `box_cat_demo.exe --smoke-test` 的唤醒/停止/失败/取消、帧预算与恢复检查也通过。
- 未提交、打标签、推送或发布；未执行服务提权验收、手机真机、其它平台原生或安装/签名验收。

## 2026-10-28：动作实验室完整小猫重新设计

代码快照：应用 `7fcc273` 加当前工作区，HUI 固定 `ce2a6718`。根据反馈重新绘制头、耳、眼睛、鼻口、胡须、躯干和尾巴，移除旧 Logo 菱形下沿与胸前折线；仅用于隔离 Demo。
Windows Debug Demo codegen/原生构建、`box_cat_demo.exe --tricks --smoke-test` 八动作检查通过；GUI 实测待机及 44% 歪头姿势，检查头身连接和遮挡。
`page_transition`、`ui_resources` 2/2 通过；主目标构建、版本命令（v0.3.75）及 `git diff --check` 通过。其它平台原生及手机实机未覆盖。

2026-10-28 后续按角色一致性反馈调整：恢复原作耳朵、实心椭圆眼、睡眼/弯眼、尾巴素材及 #111318 深色线稿；移除新增瞳孔、鼻口、胡须，只将箱盖状尖角下沿改为圆弧下巴，保留完整身体。Windows Debug Demo 构建及实际窗口待机轮廓检查通过，未更改正式品牌资源。

2026-10-28 按身体比例反馈，将动作实验室小猫的躯干和腿纵向缩短约三成，保留头部尺寸与原作特征，同步上移尾根和地面投影并减小伸爪幅度。Windows Debug Demo codegen/原生构建及 GUI 待机比例检查通过。

## 2026-10-28：首页彩蛋轻缩放切换与单项内容池

代码快照：应用 `7fcc273` 加当前工作区，版本 `0.3.75`；保留已有未提交改动。

- 首页普通卡片双击改为实时内容的 200ms、0.96→1 轻缩放淡入，移除整卡 SharedBounds 快照、收窄到侧边及附加光照；外框、网格占位和正面子树状态保留。减少动态效果直接切面，快速反向从当前进度重定向，不逐帧写 State。
- 每次从正面显示彩蛋时从公共池抽取，返回不重抽。目前仅保留官方作者 `FinalScave` 的 GitHub 主页；已从 HuxerUI 官方提交 `eb32948` 的 GitHub 作者关联核对账号。旧连接/日志/订阅/设置快捷菜单不再作为彩蛋，猫咪运行控制保持排除。
- Windows/MSVC Release 主目标与 HuxerUI codegen、`test_page_transition` 构建通过；`ctest --test-dir build --output-on-failure --no-tests=error -R '^(page_transition|ui_resources)$' 为 2/2，通过鼠标/触控、深浅主题、减少动态效果、输入隔离、正面状态保留、快速反向、每次进入抽取、链接点击及结束后停帧检查。链接使用测试回调核对目标，未自动打开浏览器。
- 主程序 GUI 实际检查大卡片与 88pt 小卡片，作者入口布局正常，邻卡不移位。`./run.ps1 --version` 输出 v0.3.75，`git diff --check` 通过。
- 本轮未重跑完整桌面服务/VPN 门禁，未做其它原生平台或手机实机、帧耗时基准、安装/签名验证；未提交或推送。
## 2026-10-28：三条可关闭 Toast 与保留退场

代码快照：应用 `7fcc273` 加当前工作区，版本 `0.3.75`；保留其它已有未提交改动。

- 应用改用 `toast_stack.h` 的窗口级有界通知层；所有页面、内核操作及窗口订阅通知共用 `UseFluxToast()`。底部居中最多三条、8pt 间距；第四条淘汰最早一条，旧条目保留占位完成退场后补入最新消息，突发不积累过期提示。
- 每条支持鼠标、触摸和键盘激活关闭，各自计时；自动到期与手动关闭使用同一 200ms 下移/轻缩放淡出路径，结束后才卸载。关闭窗口清空条目及定时器，旧句柄不能向新窗口投递。通过公开 LayerController / ToastStyle / Transition 组合应用通知策略，未修改框架。
- Windows/MSVC Release 主目标与 `test_page_transition` 经 HuxerUI codegen 构建通过；定向 `page_transition` / `ui_resources` 2/2 通过，覆盖自动/手动退场、深浅主题、reduced motion、三条独立布局、第四条淘汰、连发保留最新三条、点击/触摸取消、计时独立与窗口清理。
- 使用同一生产组件的独立 Windows 原生窗口检查三条显示、第四条淡出替换、点击移除中间条目、主题切换保持消息，以及提示外按钮仍可操作；主程序实际重新启动，首页正常。`./run.ps1 --version` 为 v0.3.75，`git diff --check` 通过。
- 未重跑完整桌面服务/VPN 门禁，未验证其它原生平台、手机实机、安装/签名；未提交或推送。
