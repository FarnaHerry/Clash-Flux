# 跨平台多客户端 TUN 冲突规避与实施记录

日期：2026-10-25。基线：`159e674` 加当前工作区。
状态：**已接入只读冲突检查与 Windows 启动诊断；自动地址避让仍待实施**。
保留原文件路径供既有链接使用，范围已扩展到 Windows、Linux、macOS 和 Android。

## 已确认现场

独立 sing-box 客户端的活动 `tun0` 已占用 `172.19.0.1/30`，与 Clash-Flux
托管 TUN 的默认地址相同。服务日志明确报
`configure tun interface: set ipv4 address: The object already exists.`，
GUI 仅显示服务托管分支的占位退出码 `-1`。
仅切换到 Clash-Flux 系统代理时，其监听正常且接收请求，但 DNS/出口连接超时，
目标路由仍经过另一客户端的 `tun0`；用户关闭该 sing-box 客户端后确认恢复。
上述证据只对应本次现场，不把所有超时归因于其他客户端。

## 待办

- [x] **真实启动错误回传。** Windows 服务保留实际退出码，按启动前日志偏移读取最多
  4096 字节，经已鉴权只读 `CORE_ERROR` 返回固定分类与退出码。原始日志不出受保护目录，
  未知错误只指向本地日志，避免订阅 URL、凭据与路径泄漏。立即/延迟退出均保留诊断，
  下次成功启动清除旧错误；旧服务不支持查询时明确提示更新。
- [x] **桌面启动前冲突检测。** `network_conflict.*` 只读查询活动接口及 IPv4/IPv6 地址，
  拒绝候选 TUN 地址网段重叠或显式接口名占用；离线残留接口不阻断。
  查询 IPv4/IPv6 两半地址空间的实际选路，只在两半均走同一内核标识的隧道、
  且命中默认或半默认前缀（/0、/1）时认定全局接管，代理模式也检查此类出口冲突。
  分流企业 VPN 不仅因存在就阻断；显式连接的原生会话可保留其路由，但不豁免地址冲突。
  自身豁免要求原生鉴权实例身份匹配上一成功计划，再按接口名与地址唯一定位；
  GUI 重开接管驻留实例还需服务/进程存活证据，才读取旧配置作接口匹配。
  不单独信任进程名、旧配置或 pidfile。下载准备前、编译候选时及配置替换前复查；
  失败反馈具体接口/地址，不增加常驻横幅，不修改其他客户端。
- [x] **Android VPN 所有权检查。** 首页、磁贴、授权 Activity、授权返回后的服务启动，
  以及后台实际建隧道前统一检查 `ConnectivityManager` 的活动 VPN。
  使用系统 owner UID，或旧版/隐私受限环境下当前 `VpnService.prepare` 授权区分自身，
  不把持久化 `vpn-active` 当所有权证据。其他活动 VPN 需要先断开；
  未确认状态仍交给正常系统授权流程，不猜测包名、不自动替换其他 VPN。
- [ ] **托管默认地址避让。** 对应用生成的默认 TUN 地址检查已有接口和路由网段重叠，
  从有界候选集合选择可用网段；配置准备与服务应用之间处理竞争，候选耗尽明确失败。
  不用另一个固定地址冒充永不冲突，不静默重写用户原生配置的显式地址/DNS 策略；
  应用地址变化时同步核对依赖的 DNS、排除地址与路由补偿。

## 边界与验收

Windows 使用 IP Helper；Linux 使用 getifaddrs、内核 SIOCGIFHWADDR/sysfs 接口类型与有界 rtnetlink
FIB 查询；macOS 使用 getifaddrs 与有界 routing-socket RTM_GET。
查询不发数据包、不解析域名、不运行 shell。查询缺失保留 Unknown 并记录日志，
不把路由采样概括为全部策略路由健康，也不声称已检查全部 DNS/PAC/WPAD 设置。
配置准备与内核应用无法对其他客户端加事务锁；最终竞争仍以真实内核错误为准。
自动选址、完整路由网段避让和独立 DNS 接管冲突判定尚未实现。
**iOS 继续 TODO 暂缓**，不扩大发布承诺，不冒充支持桌面路由检查。

地址避让只解决地址占用，不承诺两个独立的全局 TUN 能同时接管系统网络。
默认建议由一款客户端拥有全局 TUN/系统代理；有意串联或按网段分流需另行设计，
不能通过强制绑定物理网卡悄悄绕过用户既有 VPN/PPTP 意图。

完整后续验收还需覆盖双客户端真实默认路由/DNS、无关企业 VPN、
候选选择竞争与耗尽、服务启动即退/延迟退出、日志隔离与凭据脱敏、旧服务兼容，
以及失败后旧运行计划、订阅文件和数据库均保持不变。
Windows 服务与网络场景使用隔离管理员 fixture；真实双客户端验收单列记录。

实施时执行主目标构建、`./run.ps1 --version`、相关回归及 `git diff --check`。

回归扩展既有 `runtime_evidence`、`service_protocol`、`windows_service`，不改门禁数量。
覆盖双地址族重叠、相邻网段、离线接口、代理模式、原生会话、分流 VPN、自身/歧义身份，
以及真实只读 OS 快照。Windows 隔离服务另覆盖立即/延迟退出、实际退出码、日志分界、
脱敏和成功重启清错。Android `tests/android/VpnConflictTest.java` 检查所有权判定；
编译需 Android SDK android.jar 和生成的 R，纯判定运行不调用 Android stub 方法。
macOS 和 Android 真机双客户端仍需单列验收，不能拿宿主用例代替。

本地已执行：Windows 主目标构建、`runtime_evidence`/`service_protocol`、管理员隔离
`windows_service`（含服务自更新与新诊断场景）；WSL Fedora 的独立 C++23 编译及
生产检查器只读 OS/判定用例；Android SDK 36 + Java 8 兼容编译和所有权判定用例。
没有改动当前用户网络设置，也没有执行真实双客户端的启停验收。

Linux 另以独占 user/network namespace 建立真实 TUN，验证 IPv4/IPv6 的 /0 与成对 /1
路由、代理模式冲突、自身豁免、地址占用和离线残留；覆盖 sysfs 仍指向宿主网络空间、
getifaddrs 的 TUN 链路地址为空时的内核类型查询。复现命令为
`python3 tests/test_network_conflict_namespace.py <test_runtime_evidence>`，脚本自行隔离并拒绝在原网络空间改路由。
