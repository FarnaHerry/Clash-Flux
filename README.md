# Clash-Flux

Clash-Flux 是一个使用 C++23 和 HuxerUI 构建的桌面代理客户端，复刻 Clash Verge Rev
的核心体验：以 mihomo（Clash.Meta）为内核，全程经其 external-controller 的
REST API 与 WebSocket 推送流交互，UI 与内核接入层全部由 C++ 实现。

## 功能

- 订阅管理：URL 导入 / 更新 / 启用 / 删除，本地落盘
- 代理页：策略组卡片、节点切换、整组测速（延迟着色）
- 规则 / 连接 / 日志：规则列表、连接快照（可逐条/全部关闭）、实时日志流
- 内核控制：自动启停、出站模式（规则/全局/直连）、混合端口、局域网连接、日志级别
- 浅色/深色主题（跟随系统）、自定义窗口标题栏、系统托盘

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
