# Clash-Flux

Clash-Flux 是一个使用 C++23 和 HuxerUI 构建的全平台代理客户端，复刻 Clash Verge Rev
的核心体验：以 sing-box 为内核（桌面 spawn 官方二进制、Android 后台进程内运行 libbox），
桌面经 clash_api 的 REST API 与 WebSocket 推送交互，Android 经官方 libbox CommandClient
交互，UI 与内核接入层由 C++ 实现。配置支持 Clash YAML（由内置编译器转换为 sing-box 配置）和 sing-box 原生
JSON（导入后合并应用托管项，再交给 sing-box 运行）。

## 功能

- 订阅与代理配置管理：卡片式布局（右键菜单 / 双击切换 / 卡片内刷新）、URL 导入 /
  更新 / 启用 / 删除 / 规则编辑，本地落盘；支持 Clash YAML 和 sing-box 原生 JSON 文件导入
- 订阅下载：订阅级三态代理（内核 / 系统环境 / 直连）与「允许无效证书」开关；
  Windows 没有系统 CA 目录，https 订阅改用 Windows 证书存储校验，与系统浏览器
  的信任结论一致
- 代理页：分组横向标签栏（纯文字 + 选中主色下划线，标签过多可横向滚动；内容区
  支持左右滑动切换分组）、节点切换、整组测速（延迟着色），测速按钮固定在页面
  右下角
- 多重规则 / 连接 / 日志：按订阅查看各自规则，使用全局路由规则把域名/IP/CIDR
  分配给不同连接；连接快照（可逐条/全部关闭）、内核与应用日志分开保存并可恢复
- 原生 VPN 订阅：PPTP 保留系统拨号/特权服务路径（Windows 走系统 RAS，条目固定
  使用 MS-CHAPv2，MPPE-128 由订阅开关控制：勾选强制、不勾选按服务端要求协商）；
  OpenVPN `.ovpn` 由 sing-box 1.14 的 `openvpn-client` endpoint 承载，可同时
  启用多个连接；内网
  CIDR 由 sing-box 路由规则直接指向各 endpoint，PPTP 才由平台后端安装系统路由
- 内核控制：应用启动即拉起内核（没有选中订阅时用最小配置保持就绪），TUN 与系统
  代理只按实际设置恢复，不反过来决定内核是否启动；出站模式（规则/全局/直连）、
  混合端口、局域网连接、日志级别；GEOIP/GEOSITE 规则集缓存落盘前校验，损坏时
  自动清理并重新预取
- 订阅转换：Clash YAML → sing-box JSON 编译器（ss/vmess/vless/trojan/hysteria2/
  tuic 等协议、策略组、域名/IP/GEOIP/GEOSITE 规则，并将 Mihomo 常见的
  `RULE-SET,cn` / `RULE-SET,cn-ip` 映射到内置国内规则集；不支持的条目显式提示而非静默丢弃；
  原生 sing-box JSON 订阅直通）
- 系统代理（Windows、macOS、KDE / GNOME）与 TUN 模式开关（sing-box TUN，切换即重启内核生效）；Windows 后台代理操作直接使用系统 API，不弹命令行窗口
- 服务模式（可选）：统一 root systemd 服务托管 sing-box 和 Linux PPTP，
  TUN、PPTP 拨号和原生路由无需每次授权；OpenVPN 不依赖系统 CLI，随 sing-box
  内核生命周期运行；
  未安装时回落「接管外部实例 → 直接 spawn」
- 完整 CLI：同一二进制带子命令（core / mode / tun / proxy / profile / service），
  无参数启动进入 GUI
- 浅色/深色主题（跟随系统）、岛屿风界面、自定义窗口标题栏、系统托盘（Windows 托盘菜单跟随应用主题）、
  窄窗口响应式布局
- Android：sing-box libbox（与桌面内核同版本）运行在独立 `:background` 进程，
  由 VpnService 创建 TUN；主进程独占配置数据库，使用 Binder 控制运行时并用原子快照共享线路和连接。
  前台服务在系统回收后按持久化模式恢复核心或 TUN，网络切换/设备唤醒会更新默认接口并重拨旧连接；
  通知权限只控制通知栏显示，不阻挡服务启动；并可通过系统快捷设置磁贴开关 TUN

项目仍在开发中，界面和数据结构可能继续调整。

## 构建要求

- CMake 3.30 或更高版本
- 支持 C++23 modules / `import std` 的编译器（本机 GCC 16）
- Ninja（推荐）
- Linux 源码构建 HuxerUI 需要 GTK ≥4.14、libepoxy ≥1.5 与 libsoup ≥3.0 开发包；
  缺失时自动回落已安装/离线 0.3.0 SDK
- sing-box 内核由项目自带：configure 期自动下载官方 release（按本机
  平台/arch，哈希钉死，与 Android libbox 同为 1.14.0），无需手动放置；
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

OpenVPN 配置保存为原生 `.ovpn` 文本。为了让 sing-box 能在运行时直接接管连接，建议使用
`<ca>`、`<cert>`、`<key>`、`<tls-auth>` 和 `<auth-user-pass>` inline 块；当前转换器
把这些内容交给 sing-box，外部文件路径和脚本/系统路由指令不会被静默执行。

## 构建与运行

```bash
cmake -B build -G Ninja
cmake --build build -j
./run.sh
```

### 开发验证规范

每次修改完成后，无论改动大小、涉及何种文件（代码、UI、脚本还是配置），都必须先
在本地重新编译，再交付或继续验证功能：

```bash
cmake --build build --target clash-flux
./run.sh --version
```

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
Secrets；本地未提供发布密钥时，Gradle 会使用开发用 debug 签名。

## CLI

同一二进制带完整子命令；无参数启动进入 GUI，有参数走 CLI：

```bash
clash-flux version                    # 版本
clash-flux core start|stop|restart|status
clash-flux mode [rule|global|direct]  # 查看/切换出站模式
clash-flux tun on|off                 # TUN（需服务模式或 root）
clash-flux proxy on|off|status        # 系统代理
clash-flux profile list|import <url> [name]|use <id>|update <id>|remove <id>
clash-flux service install|uninstall|status|run
```

`service install` 需 root（GUI 设置页经 pkexec 提权调用）：安装 systemd 单元
`clash-flux.service`，此后 sing-box 和 Linux PPTP 都由同一个 root daemon 托管；
OpenVPN endpoint 由 sing-box 自己建立，不再要求安装 OpenVPN CLI。
已安装服务会在下一次启动内核时检查版本，并从当前客户端同步 Clash-Flux 与
sing-box；升级文件先写入同目录临时文件，再原子替换，因此即使旧服务或内核仍在运行，
也能由 systemd 平滑重启到新版本。首次安装或旧服务不支持自升级时，仍需执行一次
`sudo clash-flux service install`。出于 root 安全校验，客户端也必须位于 root 管理且
普通用户不可写的安装目录（默认软件包安装目录 `/opt/clash-flux`）。
GUI 通过受限 unix socket `/run/clash-flux/service.sock` 提交固定协议请求，
不再直接执行 `pppd` 或 `ip route`；安装时记录提权前用户 UID，socket 只允许该
用户和 root 访问。TUN/PPTP 开箱可用，OpenVPN 由 sing-box 用户态 endpoint
建立；未安装服务时 Linux 的 TUN/PPTP 不会静默尝试 root 操作，而是明确提示安装服务。

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
| build-windows-x86_64 | MSVC + choco OpenSSL + HuxerUI 安装器 | 正式 |
| build-windows-arm64 | windows-11-arm + vcpkg OpenSSL | 实验性 |
| build-macos-arm64 | macos-15 + brew LLVM | 实验性 |
| build-macos-x86_64 | macos-13 + brew LLVM | 实验性 |
| build-android | HuxerUI CLI 打 APK（GUI/native shell + sing-box libbox） | 实验性 |
| build-ios-simulator-arm64 | macOS + iOS Simulator SDK，编译 Clash-Flux app core | 诊断性，不阻塞发布 |

CI 使用固定 revision 的 HuxerUI iOS 平台源码，并从 Clash-Flux 自己的 CMake
项目编译 `clash-flux_huxerui_ios_core`（arm64 iOS Simulator）。此诊断 job 验证
Clash-Flux C++ 源码和 HuxerUI 静态平台库能够为模拟器编译；它不生成 `.app`/IPA，
也不上传 Release 资产或作为发布门禁。iOS Network Extension、sing-box 移动端
接入和完整应用壳仍列为后续 TODO；当前编译配置也不包含 curl TLS。

Linux RPM/DEB 自带桌面集成：`/usr/bin/clash-flux` 命令入口、应用菜单图标
（.desktop + hicolor 图标）；应用本体自包含安装于 `/opt/clash-flux`。

覆盖面原则：sing-box 内核发布什么桌面平台/arch，就构建什么目标（内核资产
SHA256 钉在 `cmake/singbox_bundle.cmake`，configure 期自动下载）。桌面 job
统一走 HuxerUI 源码通道（钉 commit clone 上游）。Linux、Windows、macOS、Android
和 iOS Simulator 源码构建统一固定 HuxerUI `0c5126235d43c2b703166bcc00781b850f2d1c39`，
并应用 `cmake/patches/` 中仍未被上游修复的拖动跟手、Linux 帧生命周期和 macOS/iOS
聚合初始化补丁。CMake 还会在构建前适配固定的 Lib-Camera revision，使它兼容 HuxerUI
`ApplicationContext` API。升级 HuxerUI revision 时先对照上游逐项审查补丁；上游已有修复的
补丁直接移除，其余补丁须在所有适用平台通过 `git apply --check --unidiff-zero`。

Windows 打包：`huxerui package windows` 产出自带安装向导的 setup.exe
（Burn 捆绑 MSI + HuxerUI 编写的安装器界面，含安装目录选择、桌面快捷方式、
修复/卸载；界面字符串含简中/繁中/英文）。GitHub Release 的 Windows 归档中同时
包含 `clash-flux-Setup-<版本>.exe` 安装包和便携版文件。
