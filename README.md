# Clash-Flux

<img src="resources/images/clash_flux_logo.png" width="112" alt="Clash-Flux Logo">

项目 Logo 采用出箱 demo 的完整造型；母版、旧版备份及图标再生成流程见 [Logo 资源说明](docs/branding/README.md)。

版本更新见 [v0.3.73 发布说明](docs/releases/v0.3.73.md)。推送 `v*` 标签后，CI 在发布门禁通过并收集安装包后创建 Release；优先使用 `docs/releases/<标签>.md` 作为说明。

简体中文 | [English](README.en.md)

Clash-Flux 是一个使用 C++23 和 HuxerUI 构建的全平台代理客户端，复刻 Clash Verge Rev
的核心体验：以 sing-box 为内核（桌面 spawn 官方二进制、Android 后台进程内运行 libbox），
桌面经 clash_api 的 REST API 与 WebSocket 推送交互，Android 经官方 libbox CommandClient
交互，UI 与内核接入层由 C++ 实现。配置支持 Clash YAML（由内置编译器转换为 sing-box 配置）和 sing-box 原生
JSON（导入后合并应用托管项，再交给 sing-box 运行）。

## 功能

- 订阅原文、规则集 JSON、编译 JSON 和诊断报告共用 CodeEditor 文本组件，工作线程预构建文档，输入时不逐键复制全文。

- 桌面外层使用分隔线，页内保留卡片；页名和顶部动作位于自定义标题栏，动作排列在窗口控件左侧。
  手机保留现有页面布局。
  界面约定见 [UI 开发约定](docs/ui-development.md)。
- 订阅与代理配置管理：条目右键菜单 / 双击切换 / 刷新、URL 导入 /
  更新 / 启用 / 删除 / 规则编辑，本地落盘；支持 Clash YAML、sing-box 原生 JSON 和 OpenVPN .ovpn 文件导入
  切换准备期间可改选，同一卡片重复点击合并，只保留最后一个待切目标；候选检查、启动与预览复用编译结果。
- 网页唤醒导入：兼容 sing-box 与 FlClash 订阅链接，打开预填名称与 URL 的导入表单；
  支持粘贴和扫码解析，格式与平台注册方式见[订阅唤醒链接](docs/profile-links.md)
- 订阅下载：订阅级三态代理（内核 / 系统环境 / 直连）与「允许无效证书」开关；
  Windows 没有系统 CA 目录，https 订阅改用 Windows 证书存储校验，与系统浏览器
  的信任结论一致
- 代理页：分组横向标签栏（纯文字 + 选中主色下划线，标签过多可横向滚动；点击、
  标签菜单与内容左右滑动按标签顺序切换，下划线与标签栏跟手移动、目标标签尽量居中；
  内容跟手翻页并在取消时回弹，切回保留滚动位置）、节点切换、整组测速
  （延迟着色），测速按钮固定在页面右下角
- 桌面编排目标：从 Clash 订阅的可搜索目录选择策略组或节点，包含尚未参与的来源；
  失效旧引用保留原名。手机、原生连接与 JSON 使用默认出口，详见[编排设计](docs/desktop-subscription-orchestration.md)
- 多重规则 / 连接 / 日志：按订阅查看、添加、禁用和替换规则，个人修改独立保存，更新后保留；
  [订阅个人规则](docs/subscription-personal-rules.md)直接编译成 sing-box JSON；可添加远程 JSON/SRS、导入本地规则集 JSON（桌面也支持 SRS），在个人或全局规则中引用；桌面可通过重启内核重新下载指定远程集合。`personal_rule_sets_runtime` 必跑回归覆盖导入、真实匹配、缓存与 TLS 拒绝。
  订阅规则选择器只显示支持编辑规则的来源；工具栏适配中等宽度，手机上的编译 JSON 使用独立页面。
  使用全局规则把域名/IP/CIDR
  分配给不同连接；连接快照（可逐条/全部关闭）、内核与应用日志统一显示、搜索与导出，历史可恢复
- PPTP 独立连接：保留系统拨号/特权服务路径（Windows 走系统 RAS，条目固定
  使用 MS-CHAPv2，MPPE-128 由订阅开关控制：勾选强制、不勾选按服务端要求协商；
  Windows 系统拨号同一时间仅支持一条 PPTP，连接第二条前需先断开，UI 会明确提示）；
  OpenVPN `.ovpn` 通过普通远程/本地来源导入，由 sing-box 1.14 的 `openvpn-client`
  endpoint 承载；桌面次来源按启用规则参与，手机保持单活动普通来源。
  不再提供旧 OpenVPN 独立连接或批量开关，PPTP 才由平台后端安装系统路由
- 内核控制：应用启动即拉起内核（没有选中订阅时用最小配置保持就绪），TUN 与系统
  代理只按实际设置恢复，不反过来决定内核是否启动；出站模式（规则/全局/直连）、
  混合端口、局域网连接、日志级别；桌面首页/托盘停止会撤销自有系统代理、确认
  内核退出并断开由应用持有的 PPTP，会把未完成的清理反馈为失败；桌面自有
  GEOIP/GEOSITE 缓存在替换前校验
  SRS/版本/zlib 文件头，启动时按周刷新，失败保留旧文件；原生规则集交给内核管理。
  Android 国内规则集保持固定构建资产，完整规则解析由内核负责
- 订阅转换：Clash YAML → sing-box JSON 编译器（SS/VMess/VLESS/Trojan/Hysteria v1/v2/
  SSH/TUIC/AnyTLS/Snell v4 等协议、SS 插件子集、策略组、域名/IP/GEOIP/GEOSITE 与源地址/端口/TCP/UDP
  规则，支持测速容差并保留长测速间隔；未声明的 `RULE-SET,cn` / `RULE-SET,cn-ip` 近似映射到内置国内规则集；不支持的条目显式提示而非静默丢弃；
  原生 sing-box JSON 订阅直通）
- 代理链：节点的 `dialer-proxy` 转为原生 `detour`，支持节点/组前向引用；编译期拒绝
  缺失目标和循环依赖。手动组支持 `default-selected`，未映射组字段与 `no-resolve`
  的当前语义差异进入保真度报告。
- DNS 转换：UDP/TCP/DoT/DoH/DoQ、HTTPS `#h3=true`、系统解析和 DNS 出站绑定；
  加密 DNS 支持显式 `#skip-cert-verify=true/false`，缺省保留证书校验；
  精确 `hosts` 域名到 IP/完整 IP 数组用于 DNS、节点和连接解析，异常条目整条记账；
  显式映射绕过普通 DNS 缓存，通配/别名/系统 hosts 开关仍未转换；
  `nameserver-policy` 和节点专用 `proxy-server-nameserver-policy` 支持完整域名、整层
  `*`、前缀 `.`/`+.`，共用从右向左的标签优先级；接入 IP bootstrap 和节点专用解析，
  检查 DNS 与代理链循环。直连解析时机、多服务器、fallback 与未转换字段进入
  保真度报告，不宣称支持 Clash 全部 DNS 策略。
- 高级映射：桌面接口绑定、Linux uint32 路由标记、TCP Fast Open/Multi Path；
  detour 忽略物理选项与 MPTCP 的 IPv6 差异明确记账。AND/OR/NOT 与任意名称 inline
  rule-providers 支持域名/正则、CIDR、端口、网络及平台进程/包名条件子集；路由还支持
  IP 版本和 Linux UID。保留显式 MATCH 默认出口与 REJECT-DROP；file 与 DIRECT HTTP
  provider 的 YAML/text 和 MRSv1 domain/ipcidr 已接入，指定代理与应用选择界面仍未接入。具体边界见保真度契约。
- JSON codec：Glaze 9.0.0 固定归档负责运行时快照、API 和内部路由策略；页面消费
  普通 C++ 数据，连接解析在任务线程。Clash YAML 与原生配置 DOM 保留现有适配器，
  迁移范围见 [开发记录](docs/glaze-migration.md)。
- 系统代理（Windows、macOS、KDE / GNOME）与 TUN 模式开关（sing-box TUN，切换即重启内核生效）；Windows 后台代理操作直接使用系统 API，不弹命令行窗口
- Linux 服务模式（可选）：统一 root systemd 服务托管 sing-box 和 Linux PPTP，
  TUN、PPTP 拨号和原生路由无需每次授权；OpenVPN 不依赖系统 CLI，随 sing-box
  内核生命周期运行；
  未安装时回落「接管外部实例 → 直接 spawn」
- Windows 原生服务：SCM LocalSystem 服务托管 sing-box、TUN 与 PPTP，应用保持普通
  用户权限；设置中安装/更新服务时授权一次。控制使用受限命名管道，服务不打开用户
  数据库；GUI 运行中安装或重启服务后会重新检测 PPTP 能力，详见 [Windows 服务](docs/windows-service.md)。
  新服务支持保留主 TUN 拨号、候选配置预备后交接；完整配置仍需重启一次，
  实现与验收范围见 [PPTP 连接交接](docs/native-pptp-handoff.md)。
- 完整 CLI：同一二进制带子命令（core / mode / tun / proxy / profile / service），
  无参数启动进入 GUI
- 浅色/深色/自动主题模式与可自定义主题色、岛屿风界面、自定义窗口标题栏、系统托盘（桌面启用托盘、打开“启动时隐藏到托盘”且系统托盘可用时启动直接驻留托盘，点击托盘打开主窗口；关闭托盘时该选项隐藏并保留历史值；托盘不可用时直接打开窗口；Windows 托盘菜单跟随应用主题；启用托盘后关闭窗口隐藏到托盘，停用后询问是否退出）、
  窄窗口响应式布局。最小化使用系统原生行为，Windows 保留任务栏入口，轻量托盘模式也不改变该行为
- 首页流量统计卡片用圆环图展示累计上传/下载占比（环心写占比最大一方的百分比），并同时显示各自流量总量与百分比
- 运行诊断分别检查原生控制、兼容接口、统计流和用户指定目标；统计故障按预算独立修复。
  PPTP 状态暂时无法确认时保留连接，详见 [运行健康复核](docs/runtime-health-review.md)。
  Windows/Linux 共用候选拒绝与断开保护：旧计划不引用新接口时保留主内核，仍引用时先撤出或确认停核；
  Windows 拨号补偿路由随会话持有、失败回滚，Linux 服务返回连接/断开/未知三态。
  v3 增加独立系统查询、本地有界现场记录及保存报告，使用和限制见 [系统证据与现场记录](docs/runtime-health-evidence.md)。
- Android：sing-box libbox（与桌面内核同版本）运行在独立 `:background` 进程，
  由 VpnService 创建 TUN；主进程独占配置数据库，使用 Binder 控制运行时并用原子快照共享线路和连接。
  前台服务在系统回收后按持久化模式恢复核心或 TUN，网络切换/设备唤醒会更新默认接口并重拨旧连接；
  通知权限只控制通知栏显示，不阻挡服务启动；并可通过系统快捷设置磁贴开关 TUN

项目仍在开发中，界面和数据结构可能继续调整。

## 构建要求

- CMake 4.4 或更高版本（本机与 CI 验证基线为 4.4.2）
- 通用第三方库通过 CPS 包消费；首次配置自动构建固定依赖，后续校验并复用工具链缓存。详见 [CPS 依赖管理](docs/cps-dependencies.md)
- 支持 C++23 modules / `import std` 的编译器（本机 GCC 16）
- Ninja（推荐）
- Linux 源码构建 HuxerUI 需要 GTK ≥4.14、libepoxy ≥1.5 与 libsoup ≥3.0 开发包；
  缺失时自动回落已安装/离线 0.3.0 SDK
- sing-box 内核由项目自带：configure 期自动下载官方 release（按本机
  平台/arch，哈希钉死，与 Android libbox 同为 1.14.2），无需手动放置；
  桌面缓存按版本及资产 SHA256 隔离，升级时不会复用旧版本内核；Android 构建
  核对正式 tag 与固定 revision，并校验 AAR 的版本、ABI 与 SHA256 元数据。
  `-DCLASHFLUX_BUNDLE_SINGBOX=OFF` 可关闭

Fedora：

```bash
sudo dnf install cmake ninja-build gcc-c++ gtk4-devel libepoxy-devel libsoup3-devel
```

Linux PPTP root 服务还需要系统拨号工具（服务只负责以 root 调用它们，不会把
工具打进应用）：

```bash
# Fedora
sudo dnf install ppp pptp iproute
# Debian/Ubuntu
sudo apt install ppp pptp iproute2
```

OpenVPN `.ovpn` 与 Clash YAML、sing-box JSON 共用远程订阅和本地文件入口，
自动按内容识别（扫码得到的文本同样按内容识别），新建界面不再提供独立 OpenVPN 类型或
配置文本粘贴入口，粘贴内容请保存为文件后从「本地文件」导入；本地文件入口是可拖入
文件的选区，点击仍打开系统文件选择器。源文件保留原文；可设为主订阅，
桌面也可通过启用的编排规则作为次来源，手机仍保持单活动普通订阅。
已有 `openvpn` 记录保留 ID、凭据、内网路由与辅助连接操作，归到本地来源分区，
不自动迁移或启用。兼容与限制见 [OpenVPN 导入](docs/openvpn-import.md)。
为了让 sing-box 能在运行时直接接管连接，建议使用
`<ca>`、`<cert>`、`<key>`、`<tls-auth>` 和 `<auth-user-pass>` inline 块；当前转换器
把这些内容交给 sing-box，外部文件路径和脚本/系统路由指令不会被静默执行。

## 构建与运行

首页启停按钮已接入猫咪出箱动画：桌面跟随真实内核状态，Android 跟随 VPN
后台快照；等待时持续轻抚，成功后起身，失败探头 wink 后回到睡眠。
[独立 HuxerUI Demo](demos/box-cat-demo/README.md) 仅用于隔离调试同一套视觉方案，
Linux 通过 `./demos/box-cat-demo/run-demo.sh`，Windows PowerShell 通过
`./demos/box-cat-demo/run-demo.ps1` 调试模拟启停（加 `-BuildOnly` 只编译），
不影响主程序代理；Windows 原生构建不依赖 WSL。

```bash
cmake -B build -G Ninja
cmake --build build -j
./run.sh
```

### 开发验证规范

项目级 AI 开发入口为 [AGENTS.md](AGENTS.md)，按任务读取受版本控制的 `project-skills/`：
[测试与构建](project-skills/clashflux-test/SKILL.md)、[发版](project-skills/clashflux-release/SKILL.md)、
[UI](project-skills/clashflux-ui/SKILL.md) 及其它领域技能。详细设计仍保留在各技能链接的 `docs/` 专题中，
只加载当前任务相关内容；修改开发流程时同步对应技能和文档。

每次修改完成后，无论改动大小、涉及何种文件（代码、UI、脚本还是配置），都必须先
在本地重新编译，再交付或继续验证功能：

```bash
cmake --build build --target clash-flux
./run.sh --version
```

二级标签内容使用框架 Pager；反向切换与隐藏虚拟页测量修复已包含在所用 fork 的固定提交中，所有 CI 平台共用同一版本。
详见 [UI 开发约定](docs/ui-development.md)。

命名模块的小函数需区分显式 inline 与优化器展开，检查时核对 Debug/Release 参数；
规则与汇编验证见 [C++ 模块开发](docs/cpp-modules-development.md)。

DNS 本地内核运行回归先构建 `test_singbox`，再执行
`ctest --test-dir build --output-on-failure -R '^(dns_hosts_runtime|dns_policy_runtime|dns_tls_runtime)$'`。
需要 Python 和固定打包内核；TLS 回归另需 OpenSSL CLI 生成独占临时证书，缺少时
CMake 明确报告未注册。验证范围与证据见[分层完成度记录](docs/l1-l2-l3-status.md)。

桌面 CI 配置 `-DCLASHFLUX_REQUIRE_PROJECT_TESTS=ON`，缺失或禁用必跑回归会让
配置失败；Python、固定打包内核和可执行 OpenSSL CLI 均为必要前提，下载测试
也必须执行默认证书拒绝场景。通用桌面必跑 21 项（含 CPS、运行健康、原生控制、界面资源、系统证据），Windows 另加服务、物理 DNS 和隔离 WFP 回归（24 项）；只有已记录的 MSVC C4737
缺陷允许跳过直接 ORM 测试（Windows 为 23 项），订阅持久化测试始终必跑。Windows 服务与 WFP 测试需要管理员环境。本地可使用同一门禁：

```bash
cmake -S . -B build -DCLASHFLUX_REQUIRE_PROJECT_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure --no-tests=error -L clashflux-required
```

普通开发配置该选项默认关闭；Android/iOS 不适用此桌面测试清单。

`run.sh` 默认只启动仓库 `build/clash-flux` 中已编译的程序，不负责隐式编译；
因此构建失败时不得把改动标记为已完成。若使用其他构建目录，需显式设置
`CLASHFLUX_BIN=/绝对路径/clash-flux ./run.sh --version` 验证对应产物可运行；需要
确认 GUI 启动时再执行 `./run.sh`。

也可以使用 HuxerUI CLI：

```bash
huxerui build linux
huxerui run linux
```

Android 构建（需要 Android SDK/NDK、Gradle，以及 HuxerUI SDK 的 `HUXERUI_HOME`）：

```bash
export HUXERUI_HOME=/path/to/huxerui-sdk
huxerui build android --profile release
```

Android 使用兼容编译路径，不要求 NDK 支持 C++ modules；数据面是源码构建的
sing-box libbox AAR（arm64-v8a，与桌面内核同版本），运行在独立后台进程，经 VpnService 提供 TUN。
当仓库内存在 `third_party/huxerui` 源码时，Android Java 模块和 C++ native 模块都会
从同一源码构建；没有源码时两者都使用 `HUXERUI_HOME` 中的已安装 SDK。详见
[Android 构建与运行时版本一致性](docs/android-build.md)。
Gradle 会按固定上游 revision 和 SHA256 生成并打包 `geoip-cn.srs` /
`geosite-cn.srs`；首次构建需要网络，运行时则无需联网下载国内分流规则。
Android 进程所有权、Binder 控制、服务恢复和网络切换策略见
[Android 后台服务生命周期](docs/android-background-lifecycle.md)。

GitHub Release 使用稳定的 Android 发布密钥签名。CI 需要配置
`CLASHFLUX_ANDROID_KEYSTORE_BASE64`、`CLASHFLUX_ANDROID_KEYSTORE_PASSWORD`、
`CLASHFLUX_ANDROID_KEY_ALIAS` 和 `CLASHFLUX_ANDROID_KEY_PASSWORD` 四个仓库
Secrets；APK 同时包含 v1（JAR）和 v2 签名，CI 会分别校验两种签名及正式证书指纹。
本地 Debug 构建使用开发用 debug 签名；Release 打包缺少长期发布密钥时必须失败，不会回退到 debug 签名。

## 架构与保真度契约

内核贴 sing-box、输入贴 Clash 生态、产品层用 Clash 的词汇只暴露内核真有的能力：Clash
YAML、sing-box 原生 JSON 与原生连接（PPTP、OpenVPN）统一由内置编译器翻译，每条映射按
exact / approx / unsupported 记入保真度账本，不静默降级，也不在界面里假装支持内核没有
的能力。分层职责、决策流程、协议/端点/规则覆盖基线与已知边界见
[内核分层与 Clash → sing-box 保真度契约](docs/singbox-layers-and-fidelity.md)。
当前接入程度、字段边界与下一批能力见
[sing-box 能力审查](docs/singbox-capability-audit.md)，稳定版版本与资产校验见
[内核升级记录](docs/singbox-stable-upgrade.md)，新增映射示例见
[Clash YAML 示例](docs/examples/kernel-capabilities.yaml)。
2026-10-03 工作区的逐层完成度、实际验证范围和剩余保真度问题见
[L1 / L2 / L3 开发复核](docs/l1-l2-l3-status.md)。该快照区分代码接入与实机验收，
不代表已发布安装包的全部能力。
最近增量的集中整理、新发现的订阅保护缺口和优先级见
[项目复审（2026-10-03）](docs/project-review-2026-10-03.md)。
后续已修复失败启动、异步清脏和删除文件引用边界：数据库无法打开时停止正常启动，
订阅未 hydrate 时拒绝写入，删除文件等待数据库提交并核对所有引用；回归范围见复审文末。
首批 L2 已关闭凭据原文、TLS/传输异常值和全局指纹漏记问题，包含永久回归；
后续工作区补 VMess/VLESS UDP 编码、VMess 填充参数、Hysteria2 Gecko 与跳端口映射，
保留协议默认并明确记录不适用参数，配置样本经固定内核检查。
SS 另支持 simple-obfs HTTP/TLS 与 v2ray-plugin WebSocket/TLS/mux 子集，
UDP-over-TCP 显式保留 Clash 的 v1 默认；边界见保真度契约，另附
[插件样本](docs/examples/ss-plugins-2026-10-02.yaml)。
AND/OR/NOT 路由另支持嵌套 GEO 与 inline RULE-SET 引用；失败时整条拒绝并撤回
未使用资源，示例见[逻辑规则样本](docs/examples/logical-rules-2026-10-02.yaml)。
工作区另接入 Hysteria v1 UDP 子集，保留认证优先级与 10 秒跳端口默认，默认窗口差异
明确记账；并修复 HY1/HY2 的 MBps/Mbps 单位区别，见[样本](docs/examples/hysteria1-2026-10-02.yaml)。
SSH 另支持密码、内联/加密私钥和主机公钥/算法列表；双认证顺序差异明确记账，
外部私钥路径仍拒绝，见[SSH 样本](docs/examples/ssh-2026-10-02.yaml)。
本地 `type: file` rule-provider 支持 YAML/text 的 domain/ipcidr/classical 子集，
`path` 相对应用数据目录（Linux 默认 `~/.local/share/clash-flux`），最多 8 MiB。
转换为 inline 快照，文件修改后须重新应用配置，缺少自动监听明确记 approx；
文件错误会阻止候选应用，保留旧订阅。MRSv1 domain/ipcidr 已接入，classical/未来版本和文件管理界面仍待接入，见
[样本与配套文件](docs/examples/file-providers-2026-10-03.yaml)及[保真度契约](docs/singbox-layers-and-fidelity.md)。
HTTP provider 也支持 YAML/text 直连子集：任务下载、完整转换与候选检查后才更新
自有缓存，失败沿用已验证旧缓存，运行计划保留独立内容快照。
资源准备支持有限并发、ETag/Last-Modified 条件刷新及有界进程内转换缓存，
详情见[缓存与保真度契约](docs/singbox-layers-and-fidelity.md)。`interval` 在应用
配置时检查到期，尚无定时热更新；`path` 只读不回写。支持 `header` 的单值 ASCII
数组子集（如 `User-Agent: [my-client/1]`），请求头参与缓存身份；带头下载不跟随
重定向并记 approx。多值、传输保留头、指定代理及 MRS classical/未来版本仍拒绝整份声明。
Android 使用系统 TLS 下载桥接，默认校验证书；8 MiB 或更小 `size-limit` 超限拒绝，
见[HTTP 离线样本](docs/examples/http-providers-2026-10-03.yaml)。
协议类型覆盖为 12/15；不表示每种协议的全部字段已支持，更多字段与 provider 适配继续推进。
代理组支持 `include-all-proxies: true` 的静态来源展开：本来源节点按名称排序追加到
显式成员之后，不引入其它组或其它订阅节点。非空筛选和代理集合组合尚未转换，
候选会明确拒绝；空组不会隐式直连。见[离线展开样本](docs/examples/group-expansion-2026-10-03.yaml)。
桌面已接入[唯一主订阅 + 规则驱动的多次来源编排](docs/desktop-subscription-orchestration.md)：
Clash YAML 次来源只通过启用规则参与，同名组/节点隔离；全局覆盖、来源分流、主规则、
主兜底使用固定层级，权重只在层内比较。目标不可用默认阻断，可明确选择主默认出口或直连。
主来源支持 Clash YAML / 原生 JSON，原生 JSON 次来源和跨来源代理链仍未开放；
手机保持单活动代理订阅，不增加多订阅编排。
交互参考与接入次序见[sing-box 官方 GUI 研究](docs/singbox-official-gui-review.md)。

## CLI

同一二进制带完整子命令；无参数启动进入 GUI，有参数走 CLI：

```bash
clash-flux version                    # 版本
clash-flux core start|stop|restart|status
clash-flux mode [rule|global|direct]  # 查看/切换出站模式
clash-flux tun on|off                 # TUN（需服务模式或 root）
clash-flux proxy on|off|status        # 系统代理
clash-flux profile list|import <url> [name]|use <id>|update <id>|remove <id>
clash-flux profile check [<id>]        # 检查订阅保真度（有无法映射的条目时退出码 1）
clash-flux service install|uninstall|status|run
```

`service install` 需 root（GUI 设置页经 pkexec 提权调用）：安装 systemd 单元
`clash-flux.service`，此后 sing-box 和 Linux PPTP 都由同一个 root daemon 托管；
OpenVPN endpoint 由 sing-box 自己建立，不再要求安装 OpenVPN CLI。
每次 GUI 启动后都会异步检查已安装服务，在内核自启前同步 Clash-Flux 与
sing-box；升级文件先写入同目录临时文件，再原子替换，因此即使旧服务或内核仍在运行，
也能由 systemd 平滑重启到新版本。首次安装或旧服务不支持自升级时，仍需执行一次
`sudo clash-flux service install`。出于 root 安全校验，客户端也必须位于 root 管理且
普通用户不可写的安装目录（默认软件包安装目录 `/opt/clash-flux`）。
GUI 通过受限 unix socket `/run/clash-flux/service.sock` 提交固定协议请求，
不再直接执行 `pppd` 或 `ip route`；安装时记录提权前用户 UID，socket 只允许该
用户和 root 访问。TUN/PPTP 开箱可用，OpenVPN 由 sing-box 用户态 endpoint
建立；未安装服务时 Linux 的 TUN/PPTP 不会静默尝试 root 操作，而是明确提示安装服务。

Windows 使用原生 SCM 服务 `ClashFluxService`，首次在设置的内核服务安装，软件升级后
启动应用会异步检查版本，由服务自己的 LocalSystem 助手同步受保护 Program Files
安装目录中的载荷并重启，不请求 UAC。同版本不更新，未安装不主动安装；更新失败
保留手动入口并暂停本次内核自启。旧服务需要先手动更新一次启用此能力；用户可写
目录中的便携版仍通过「更新服务」授权更新。UAC 只提升服务管理子命令，GUI 不需要管理员运行。
管理员终端也可执行 `clash-flux.exe service install|uninstall`。Windows 内核不再由
GUI 直接启动，详细授权、退出行为和验证范围见 [Windows 服务](docs/windows-service.md)。

## 多平台 CI

`.github/workflows/build.yml` 在推送到 main、PR 以及 `v*` Tag 时都跑完整矩阵
（仓库是公开仓库，标准 GitHub-hosted runner 的 Actions 用量免费，Windows/macOS
的倍率只影响私有仓库额度；真正的限制是 Free 计划的并发与 fair-use）。Tag 推送会
额外跑发布流程：`publish-release` 汇总各平台产物创建对应的 GitHub Release
（各平台归档 + Windows 安装包）。

矩阵（命名 `build-<os>-<arch>`）：

| Job | Runner | 状态 |
|-----|--------|------|
| build-linux-x86_64 | ubuntu 容器 + clang-21/libc++ | 正式 |
| build-linux-arm64 | ubuntu-24.04-arm 原生 | 实验性 |
| build-windows-x86_64 | MSVC + runner 预装 OpenSSL + HuxerUI 安装器 | 正式 |
| build-macos-arm64 | macos-15 + brew LLVM | 正式发布门禁 |
| build-android | HuxerUI CLI 打 arm64 APK（GUI/native shell + sing-box libbox） | 正式发布门禁 |
| build-ios-simulator-arm64 | macOS + iOS Simulator SDK 编译未签名 app/Packet Tunnel，并构建固定 sing-box 的 iOS device/Simulator Libbox.xcframework | TODO / 暂缓；实验性诊断，不阻塞发布 |

Windows 安装器另有 `build-windows-installer` 门禁。当前矩阵没有 Windows arm64 或
macOS x86_64 job；Linux arm64 为非阻塞实验项。表中状态描述工作流门禁，
不表示本次已经查询线上 CI 结果或完成平台实机验收。

CI 使用固定 revision 的 HuxerUI iOS 平台源码，从 Clash-Flux 自己的 CMake
项目编译 `clash-flux_huxerui_ios_core`（arm64 iOS Simulator），并从与 Android
相同的 sing-box revision 构建 `Libbox.xcframework`（iOS device + Simulator）。此诊断
job 验证 app、Packet Tunnel 与引擎可为 Simulator 编译；它不生成 IPA、不上传 Release 资产，也不作为
发布门禁。iOS 订阅下载由原生 URLSession 提供 TLS：默认验证系统证书，订阅可单独选择允许无效证书。
**iOS 当前按 TODO 暂缓，不属于受支持平台，也没有 TestFlight/App Store 发布承诺。**现有工作只验证 Simulator 编译；可分发的签名设备包和真机 VPN 生命周期尚未完成，是否恢复支持由后续项目决策确定。
构建和 Apple 签名说明见 [iOS 构建文档](docs/ios-build.md)。

Linux RPM/DEB 自带桌面集成：`/usr/bin/clash-flux` 命令入口、应用菜单图标
（.desktop + hicolor 图标）；应用本体自包含安装于 `/opt/clash-flux`。

官方桌面内核资产表保留 6 项版本/摘要映射，客户端构建范围以实际 CI 矩阵为准
（内核资产 SHA256 钉在 `cmake/singbox_bundle.cmake`，configure 期自动下载）。桌面 job
统一走 HuxerUI 源码通道，从 [FarnaHerry/HuxerUI](https://github.com/FarnaHerry/HuxerUI) 的
`farna/main` 检出经验证的固定 SHA；Linux、Windows、macOS、Android 和 iOS Simulator
共用 `.github/workflows/build.yml` 的 `HUXERUI_COMMIT`。框架补丁集中维护在 fork，
`main` 保留官方基线；Lib-Camera、Lib-Charts、Lib-SQLite 也从各自 `FarnaHerry` fork 的
`farna/main` 按固定 SHA 获取，原生构建与 Android 库图共用同一依赖身份。
本地检出、补丁归属与升级流程见 [HuxerUI fork](docs/huxerui-fork.md)。

Windows 打包：`huxerui package windows` 产出自带安装向导的 setup.exe
（Burn 捆绑 MSI + HuxerUI 编写的安装器界面，含安装目录选择、桌面快捷方式、
修复/卸载；界面字符串含简中/繁中/英文）。GitHub Release 的 Windows 归档中同时
包含 `clash-flux-Setup-<版本>.exe` 安装包和便携版文件。
新安装器使用独立的 Burn 升级标识，并在 MSI 安装提交后迁移清理 0.3.0–0.3.68 的旧 Bundle 注册和专属缓存；MSI 升级标识保持不变，用户数据与共享 MSI 缓存保留。边界见 [HuxerUI fork](docs/huxerui-fork.md#windows-legacy-bundle-migration)。

启动时在首帧构建前同步、只读地读取保存的主题，避免默认主题切换闪烁；其余设置与订阅仍异步加载。Lib-SQLite 启动只读查询已合入所用 fork 的固定提交，详见 [UI 开发约定](docs/ui-development.md)。

ASN 规则以完整 ipverse IPv4/IPv6 公告网段快照转换，标为近似，数据缺失时拒绝候选并保留旧订阅。
MRS 使用 vendored Zstandard 1.5.7 静态解码，无需额外安装系统库；固定归档 SHA256 由 CMake 校验。
详细限制见 [保真度契约](docs/singbox-layers-and-fidelity.md)。
