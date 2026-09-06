# Clash-Flux

Clash-Flux 是一个使用 C++23 和 HuxerUI 构建的桌面代理客户端，复刻 Clash Verge Rev
的核心体验：以 mihomo（Clash.Meta）为内核，全程经其 external-controller 的
REST API 与 WebSocket 推送流交互，UI 与内核接入层全部由 C++ 实现。

## 功能

- 订阅管理：卡片式布局（右键菜单 / 双击切换 / 卡片内刷新）、URL 导入 / 更新 /
  启用 / 删除 / 规则编辑，本地落盘
- 代理页：策略组卡片、节点切换、整组测速（延迟着色）
- 规则 / 连接 / 日志：规则列表、连接快照（可逐条/全部关闭）、实时日志流
- 内核控制：自动启停、出站模式（规则/全局/直连）、混合端口、局域网连接、日志级别
- 系统代理（KDE / GNOME）与 TUN 模式开关
- 服务模式（可选）：root systemd 服务托管内核，TUN 等特权操作无需每次授权；
  未安装时回落「接管外部实例 → 直接 spawn」
- 完整 CLI：同一二进制带子命令（core / mode / tun / proxy / profile / service），
  无参数启动进入 GUI
- 浅色/深色主题（跟随系统）、岛屿风界面、自定义窗口标题栏、系统托盘、
  窄窗口响应式布局

项目仍在开发中，界面和数据结构可能继续调整。

## 构建要求

- CMake 3.30 或更高版本
- 支持 C++23 modules / `import std` 的编译器（本机 GCC 16）
- Ninja（推荐）
- Linux 源码构建 HuxerUI 需要 GTK ≥4.14、libepoxy ≥1.5 与 libsoup ≥3.0 开发包；
  缺失时自动回落已安装/离线 0.2.0 SDK
- mihomo 内核由项目自带：configure 期自动下载官方 release（linux x86_64，
  哈希钉死），无需手动放置；`-DCLASHFLUX_BUNDLE_MIHOMO=OFF` 可关闭

Fedora：

```bash
sudo dnf install cmake ninja-build gcc-c++ gtk4-devel libepoxy-devel libsoup3-devel
```

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
`clash-flux.service`，此后内核由 root 服务托管（unix socket
`/run/clash-flux/service.sock`），TUN 开箱可用。

## 多平台 CI

`.github/workflows/build.yml`：Linux（ubuntu 容器 + clang-21/libc++）为正式 job；
Windows（MSVC）/ macOS（brew LLVM）/ Android（HuxerUI CLI 打 APK，仅 GUI 壳、
不含 mihomo 内核）为实验性 continue-on-error。桌面 job 统一走 HuxerUI 源码
通道（钉 commit clone 上游），mihomo 在 configure 期自动下载。

Windows 打包：`huxerui package windows` 产出自带安装向导的 setup.exe
（Burn 捆绑 MSI + HuxerUI 编写的安装器界面，含安装目录选择、桌面快捷方式、
修复/卸载；界面字符串含简中/繁中/英文）。
