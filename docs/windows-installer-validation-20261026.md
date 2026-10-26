# Windows 0.3.68 → 0.3.69 安装验证（2026-10-26）

代码快照：发布标签 `v0.3.69`，提交 `68fbb25a8b7085733cb8c1e29e3262038b7fa2cc`；HuxerUI 固定 `abd07a3f19d2680ade6093075608f73b42a9b3a3`。
以下为本机实际验证，时间为 Asia/Shanghai。结论：应用升级、旧 Burn 注册迁移和数据保留通过；升级保留服务失败，已手动恢复服务，不将整条升级路径记为无缺陷通过。

## 安装包与安装前状态

- 使用下载目录内 `clash-flux-Setup-0.3.69.exe`，大小 35,910,543 字节。
- SHA-256：`7bd83bd2011dbd6b27bbddb401b08e0a7f00d2de2dce61cb0ef0436a7319bee2`，与 GitHub Release 资产摘要一致。
- 主程序、ClashFluxService 均为 0.3.68；GUI 已关闭，服务运行，内核未运行。
- 卸载项包括 0.3.60、0.3.61、0.3.62、0.3.66、0.3.68 五个旧 Burn Bundle，以及当前 0.3.68 MSI。
- Package Cache 中 Clash-Flux 文件 21 个，共 440.46 MiB。
- 数据库 `integrity_check=ok`、`user_version=2`，含 profiles 5 行、settings 15 行。
- 使用 SQLite backup API 保存独立数据库备份，同时复制 profiles 文件；仅在本地保存，不提交订阅、凭据或备份。

## 实际执行与结果

1. 09:59 使用 `/quiet /norestart /log <证据目录>/upgrade.log` 首次安装返回 1622（`0x80070656`）。MSI 日志停在建立服务端执行/日志阶段，未进入产品变更。复核程序文件、数据库逻辑内容和旧注册均保持原样；这不代表安装中途回滚已覆盖。
2. 改用 `/quiet /norestart` 与安装器默认日志位置重试，10:00:35 返回 0，Burn `Apply complete=0x0`，未要求重启。
3. MSI 提交日志逐条记录五个旧 Bundle 的 `removed legacy bundle registration and private cache`，没有再次调用旧版缓存 bootstrapper，也没有空 HWND 或 non-vital 卸载失败。
4. 安装后卸载项只剩 0.3.69 Bundle 与隐藏的 0.3.69 MSI。新 Bundle UpgradeCode 为 `{8A8B3461-3A85-5F69-B9CB-DE0C2F641033}`。
5. 同版本重新打开安装器正确显示“此版本已经安装”维护页，提供卸载、修复和重新安装入口；仅检查页面，随后关闭，没有执行这些动作。

## 服务异常与恢复

升级后 `service status` 显示服务未安装，`sc.exe query ClashFluxService` 返回 1060。
MSI 日志证明旧 0.3.68 产品在 `UPGRADINGPRODUCTCODE` 已设置时仍将 `ServiceCleanup` 组件置为 Absent，随后执行 `ServiceControl(Name=ClashFluxService,Action=2)` 和 `Action=8`，停止并删除了原服务。
因此文档原有“MajorUpgrade 保留服务”的承诺在此升级路径不成立；后续修复必须考虑已发布旧 MSI 自身的卸载行为。

为恢复本机可用状态，从受保护的新版 Program Files 目录提权执行：

```powershell
clash-flux.exe service install --owner-sid <原交互用户 SID>
```

恢复安装退出 0；再次查询确认服务 0.3.69 正在运行、PPTP 系统服务后端可用，内核仍未运行。没有请求代理连接或修改系统代理。

## 数据与运行验证

- 安装后及新版 GUI 启动后，profiles/settings 两表的行数和全部行内容摘要均与安装前一致，`integrity_check=ok`。
- 全部 2 个 profiles 文件的相对路径、长度和 SHA-256 保持一致；GUI 可见 2 条远程订阅及 3 条 PPTP 订阅。
- `clash-flux.exe --version` 输出 0.3.69、退出 0；开始菜单目标仍指向当前 Program Files 主程序。
- 新版 GUI 首页正常显示内核停止；未执行真实 VPN、TUN 或 PPTP 连接。
- 运行后锁文件、WAL/SHM、运行日志与健康历史的变化属于运行期状态，数据库逻辑内容及订阅文件单独核对。
- 验证结束时 GUI 与维护页已关闭，恢复到安装前的 GUI 关闭状态；后台服务运行，内核停止。

## 记录修改后的基础检查

已加载 MSVC x64 开发环境，执行 `cmake --build build --target clash-flux`（无待构建项）、`CLASHFLUX_BIN=build/health-evidence/clash-flux.exe` 对应的 `./run.ps1 --version`（0.3.69、退出 0）和 `git diff --check`，均通过。仅新增/更新验证文档，没有修改安装器代码，没有提交或推送本次记录。

## 保留的缓存与未覆盖边界

安装后 Clash-Flux 缓存文件 18 个、455.47 MiB，其中旧版文件 16 个、421.07 MiB。增加来自新 MSI 缓存；迁移只删除五个旧 Bundle 的私有缓存，不清理共享 MSI 载荷。
0.1.3、0.2.1 的早期 Bundle 缓存及历史 MSI 缓存/空依赖 Provider 仍保留，不能宣称所有历史痕迹清零；本次没有额外手动删除。

本机没有执行真实卸载、修复重装、安装中途故障回滚或 VPN 数据流验收。首次自定义日志目录失败的根因尚未确定，默认日志路径重试成功。
本地证据位于 `%LOCALAPPDATA%\ClashFlux-InstallValidation\v0.3.69-20261026`，包含前后系统清单、数据库内容摘要、数据备份、两次安装日志和服务恢复结果。

## 后续经用户授权清理历史缓存（10:16）

用户明确要求删除无用历史缓存后，以管理员身份通过 Windows Installer API 核对全部用户的 226 个 MSI 产品注册，并逐个检查目标产品身份、旧版本、卸载注册和依赖引用。按精确白名单删除 16 个未使用的历史目录、18 个文件，共 441,530,530 字节（421.08 MiB），没有递归删除 Package Cache 根目录，没有修改注册表或 Windows Installer 系统缓存。

清理后只保留 0.3.69 Bundle 与 MSI 的两份缓存，共 34.40 MiB；两份缓存摘要与清理前一致。程序文件、5 条订阅、15 项设置、全部订阅文件及当前卸载注册均保持一致；服务仍为同一进程运行的 0.3.69，内核停止。精确删除清单、摘要及复核结果保存在本地证据目录的 `cleanup-plan.json`、`cleanup-result.json` 和 `cleanup-verification.json`。下载目录、数据备份及安装验证日志未清理。
