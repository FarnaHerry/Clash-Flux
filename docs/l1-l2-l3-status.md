# L1 / L2 / L3 开发完成度复核

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
`2aa99554a64339a07bbd2c553cf253f40f375b16`。

- 首轮 tag CI `40461865066` 的 Windows、macOS 构建与 Windows 安装器成功；Android 因
  AndroidLegacy 未生成 `title_bar_search.cpp` 中的搜索焦点实现而链接失败。已将该源文件
  和头文件加入输入依赖，并将实现加入 Android UI 源码图。
- Linux x86_64/arm64 因 CodeEditor 合并的预编译 simdutf 对象要求 libstdc++ 新符号而失败。
  CodeEditor fork 现在固定 simdutf v7.7.0 源码提交，并仅在 Linux 按消费工具链构建，避免
  与应用 libc++ 混用。第一次重打 tag 的 Linux arm64 job 随后发现 upstream 头文件布局与
  SweetEditor include 路径不同；fork `2aa9955` 补上同版本兼容头目录。iOS Simulator 诊断
  同时暴露静态库已合入 simdutf 却继续传递该目标的链接边；fork 已从静态库链接闭包移除
  该冗余边。iOS 仍是非阻塞诊断平台，等待新 CI 确认。
- 固定到 CodeEditor `2aa9955` 后，Windows/MSVC Release 主目标和 HuxerUI codegen 构建成功；PTY 下
  `./run.ps1 --version` 输出 `Clash-Flux v0.3.72（sing-box 客户端）`；`git diff --check`
  通过。上一轮使用 CodeEditor `e3695be` 的流水线已因 Linux include 缺失取消；`2aa9955`
  仍待新 CI 验证 Android/Linux/iOS 修复。
- 本机 Windows 安装验收仍未执行，按用户要求等待隔离测试环境；不得将构建或 CI 等同于
  Burn 升级、回滚、卸载验收。
