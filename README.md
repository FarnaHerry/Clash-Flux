# Clash-Flux

Clash-Flux 是一个使用 C++23 和 HuxerUI 构建的桌面代理客户端，复刻 Clash Verge Rev
的核心体验：以 mihomo（Clash.Meta）为内核，全程经其 external-controller 的
REST API 与 WebSocket 推送流交互，UI 与内核接入层全部由 C++ 实现。

## 功能

- 订阅管理：卡片式布局（右键菜单 / 双击切换 / 卡片内刷新）、URL 导入 / 更新 /
  启用 / 删除 / 规则编辑，本地落盘
- 代理页：策略组卡片、节点切换、整组测速（延迟着色）
- 多重规则 / 连接 / 日志：按订阅查看各自规则，使用全局路由规则把域名/IP/CIDR
  分配给不同连接；连接快照（可逐条/全部关闭）、实时日志流
- 原生 VPN 订阅：Linux 支持 PPTP 和 OpenVPN CLI（粘贴 `.ovpn` 文本），可多选
  同时连接；内网 CIDR 由统一 root 服务安装到对应隧道
- 内核控制：自动启停、出站模式（规则/全局/直连）、混合端口、局域网连接、日志级别
- 系统代理（KDE / GNOME）与 TUN 模式开关
- 服务模式（可选）：统一 root systemd 服务托管 mihomo、Linux PPTP 和 OpenVPN，
  TUN、拨号和原生路由无需每次授权；
  未安装时回落「接管外部实例 → 直接 spawn」
- 完整 CLI：同一二进制带子命令（core / mode / tun / proxy / profile / service），
  无参数启动进入 GUI
- 浅色/深色主题（跟随系统）、岛屿风界面、自定义窗口标题栏、系统托盘、
  窄窗口响应式布局
- Android：按设备 ABI 内置 mihomo Android 内核，使用应用私有目录保存数据、
  支持 Android Activity 生命周期与外部链接；VPN/TUN 接入尚未提供

项目仍在开发中，界面和数据结构可能继续调整。

## 构建要求

- CMake 3.30 或更高版本
- 支持 C++23 modules / `import std` 的编译器（本机 GCC 16）
- Ninja（推荐）
- Linux 源码构建 HuxerUI 需要 GTK ≥4.14、libepoxy ≥1.5 与 libsoup ≥3.0 开发包；
  缺失时自动回落已安装/离线 0.3.0 SDK
- mihomo 内核由项目自带：configure 期自动下载官方 release（linux x86_64，
  哈希钉死），无需手动放置；`-DCLASHFLUX_BUNDLE_MIHOMO=OFF` 可关闭

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

Linux OpenVPN 还需要系统 CLI：

```bash
# Fedora
sudo dnf install openvpn iproute
# Debian/Ubuntu
sudo apt install openvpn iproute2
```

OpenVPN 配置保存为原生 `.ovpn` 文本。为了让 root 服务能够安全托管连接，建议使用
`<ca>`、`<cert>`、`<key>`、`<tls-auth>` 和 `<auth-user-pass>` inline 块；配置中引用的
相对路径文件不会随订阅卡片自动复制，若必须使用外部文件请填写绝对路径并确保 root 服务可读。

## 构建与运行

```bash
cmake -B build -G Ninja
cmake --build build -j
./run.sh
```

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

Android 使用兼容编译路径，不要求 NDK 支持 C++ modules；APK 同时包含
arm64-v8a 与 x86_64 的 mihomo 内核，首次启动时按设备 ABI 解包到应用私有目录。
当前版本可启动 mihomo 的本地代理，但 Android VPN/TUN 接入尚未提供。

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
`clash-flux.service`，此后 mihomo、Linux PPTP 和 OpenVPN 都由同一个 root daemon 托管。
更新了二进制或新增了服务协议后，需要重新执行一次 `sudo clash-flux service install`
让 systemd 使用新版本 daemon；仅替换 GUI 二进制不会更新已运行的 root 服务。
GUI 通过受限 unix socket `/run/clash-flux/service.sock` 提交固定协议请求，
不再直接执行 `pppd` 或 `ip route`；安装时记录提权前用户 UID，socket 只允许该
用户和 root 访问。TUN/PPTP/OpenVPN 开箱可用，未安装服务时 Linux 原生 VPN 不会静默尝试
用户态 root 操作，而是明确提示安装服务。

## 多平台 CI

`.github/workflows/build.yml` 矩阵（命名 `build-<os>-<arch>`）：

| Job | Runner | 状态 |
|-----|--------|------|
| build-linux-x86_64 | ubuntu 容器 + clang-21/libc++ | 正式 |
| build-linux-arm64 | ubuntu-24.04-arm 原生 | 实验性 |
| build-windows-x86_64 | MSVC + choco OpenSSL + HuxerUI 安装器 | 正式 |
| build-windows-arm64 | windows-11-arm + vcpkg OpenSSL | 实验性 |
| build-macos-arm64 | macos-15 + brew LLVM | 实验性 |
| build-macos-x86_64 | macos-13 + brew LLVM | 实验性 |
| build-android | HuxerUI CLI 打 APK（GUI/native shell + mihomo 内核） | 实验性 |

覆盖面原则：mihomo 内核发布什么桌面平台/arch，就构建什么目标（内核资产
SHA256 钉在 `cmake/mihomo_bundle.cmake`，configure 期自动下载）。桌面 job
统一走 HuxerUI 源码通道（钉 commit clone 上游）。

Windows 打包：`huxerui package windows` 产出自带安装向导的 setup.exe
（Burn 捆绑 MSI + HuxerUI 编写的安装器界面，含安装目录选择、桌面快捷方式、
修复/卸载；界面字符串含简中/繁中/英文）。GitHub Release 的 Windows 归档中同时
包含 `clash-flux-Setup-<版本>.exe` 安装包和便携版文件。
