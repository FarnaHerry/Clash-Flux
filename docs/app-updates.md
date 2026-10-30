# 应用内下载与升级

设置 → 关于 → 检查更新读取项目 GitHub latest stable Release，按数字比较稳定版本。
发现新版本后可在应用内下载；下载完成并通过校验后显示“安装已下载的更新”。
桌面用户再次确认“关闭并安装”后，应用保存配置、断开代理。Windows 退出后由独立助手
启动安装向导；Linux 保留应用进程直到 Polkit 授权及包管理器安装成功，成功后才退出。
Linux 授权取消或安装失败恢复原窗口与同一持久化会话并反馈错误。下载完成不会自行安装。

## 平台与产物

| 平台 | 应用内安装产物与入口 |
| --- | --- |
| Windows x64 | `clash-flux-<version>-windows-x86_64.exe`，沿用现有 Burn/MSI 安装向导及其 UAC 授权 |
| Linux x86_64 / aarch64，DEB 系 | `clash-flux-<version>-linux-<arch>.deb`，`pkexec /usr/bin/dpkg --install <固定快照>` |
| Linux x86_64 / aarch64，RPM 系 | 同名 `.rpm`，`pkexec /usr/bin/rpm --upgrade <固定快照>` |
| Android arm64-v8a | `clash-flux-<version>-android-arm64-v8a.apk`，系统安装界面确认覆盖升级 |
| 其它平台 / 架构 / 包格式 | 保留 Release 页面手动下载入口 |

对外 Release 文件名统一为 `clash-flux-<version>-<platform>-<arch>.<format>`，版本不带 `v`，
格式可为 `exe`、`zip`、`deb`、`rpm`、`tar.gz` 或 `apk`。Windows 安装器与便携归档
分别使用 `clash-flux-<version>-windows-x86_64.exe` / `.zip`。
Windows 更新优先匹配新名称；仅当新名称完全不存在时，兼容历史
`clash-flux-Setup-<version>.exe`，旧名称仍须满足相同来源、摘要和唯一性校验。
新名称元数据非法或重复时不降级匹配旧名称。HUI/WiX 的内部 Setup 文件名及旧 Bundle
清理规则保持原约定，由 CI 暂存为统一对外名称，不重命名已发布资产。

Linux 根据系统包数据库选择 DEB/RPM，要求 `/usr/bin/pkexec` 和对应包管理器可用；
桌面会话须有可用的 Polkit 授权代理，密码在系统授权窗口输入，应用不接触密码。
安装使用发行包的系统安装位置，不承诺覆盖任意 tar/ZIP 解压目录。
Windows 由安装向导选择/沿用安装位置。安装后从向导或系统应用入口重新打开；
Linux 安装失败/取消保留原应用并显示 Polkit/包管理器错误；无桌面授权代理时明确失败，
不回落到不可见的终端密码提示。

## 下载、验证与生命周期

- 版本检查和包下载统一使用 HuxerUI 自己的平台 `HttpClient`：`AppHttpClient`
  转发 `SendAsync` / `SendStreamAsync`，包体通过 `Body().ReadAsync` 分块读取；
  文件写入与 SHA-256 校验由应用工作线程完成，不在 UI 线程做阻塞写入或散列。
- `AppUpdateModel` 是 Runtime 级共享模型；检查与下载单飞，应用任务域持有工作，
  切页、缩窗及托盘销毁/重建窗口保留进度。取消下载使请求号失效并取消传输；
  已执行的文件写入由 worker 保留暂存所有权直到结束，不让旧结果覆盖重试。
- API 检查 10 秒超时、2 MiB 响应上限；包下载流式读取，每块 64 KiB，
  30 分钟总超时，声明大小在 1 字节至 512 MiB 之间。进度按实际下载字节计算。
- 只选择名字、版本、平台完全匹配的唯一 uploaded 资产；下载 URL 必须是固定项目
  该 tag 的标准地址，最终响应限同一地址或 GitHub 官方 release-assets HTTPS CDN。
  缺失/非法 SHA-256 digest、上传未完成、重复资产或错误包格式只保留手动入口。
  沿用系统 TLS 证书校验，不提供跳过证书选项。
- 缓存使用应用 cache 下 `app-updates/pending-<随机值>` 的独占目录；下载先写
  `package.part`（Android 为 `package.part.apk`），关闭并确认写入成功、校验完整大小和 SHA-256 后才重命名成安装候选。
  安装交接前再次校验。失败/取消只清理本次已知文件，不扫描或清理订阅/数据库。

## 退出和权限边界

升级准备先确认配置可保存，再停止 VPN/内核；冻结新的持久化写入后排空已有待写版本。
保存失败、停核失败或助手未准备成功时不退出、不安装，恢复同一持久化会话供重试。
Linux 授权和安装在任务线程等待，应用保持冻结且不可交互；失败恢复原窗口与待写会话，
成功才请求应用退出，避免桌面会话在主进程退出后收走升级助手。
冻结不会清库、重开库或丢弃待写版本。冻结后的晚到修改明确失败，不能假装保存成功。

桌面的 `clash-flux-updater` 不创建 Runtime、不打开数据库、不更新服务载荷。应用版本升级
沿用原有安装包；重开后的服务版本同步沿用现有受保护服务升级流程。

Windows 助手使用静态 CRT，不依赖应用 DLL；先复制至本次缓存目录，再持主进程句柄和
禁止写入/删除的包句柄，校验后确认就绪。主进程退出后才用原生 API 显示安装器，
安装器失败/取消会提示；不通过 shell，不提权 GUI，不尝试原地覆盖正在运行的应用。

Linux 助手是普通用户进程；先复制下载内容到 sealed memfd 并校验，再向父进程确认就绪。
主进程仍存活时，Polkit 只提升 dpkg/rpm（禁用内置终端授权代理）；包管理器从助手持有的
不可修改快照读取安装包。助手等待包管理器退出并返回退出码及有界诊断；
用户缓存中的文件替换不会影响授权后的安装内容。DEB/RPM 的事务与依赖错误由包管理器
处理，本功能不宣称跨包管理器回滚，也不绕过已有包锁或自动卸载其它软件。

Windows 助手等待主进程退出最多 120 秒，超时不安装。`ready.txt` 与 `result.txt` 只记录
本次交接；Windows 失败另写同缓存根的 `last-result.txt`，下次应用启动读取有界消息并消费；
Linux 直接将退出码和 `installer.log` 的有界诊断返回仍存活的应用。
安装成功删除安装包；Windows 仍可能留存很小的助手自身和诊断文件，归应用缓存管理。
进程被系统强杀也可能留下暂存目录，不做启动时批量删除。

## Android 系统安装

Android 使用相同的 HUI 平台 HTTP 下载与共享进度模型，不跳浏览器找 APK，也不依赖应用商店。
应用作用域的 HUI Java PlatformModule 在工作线程用 JCA 校验大小和 SHA-256，再用
PackageManager 检查包名、所选 Release 的 versionName、严格递增的 versionCode、设备最低 SDK
及当前应用和候选 APK 的正式发布签名。证书指纹沿用 [Android 构建](android-build.md) 的唯一基线；
debug 签名安装不能覆盖升级为正式签名，不自动卸载或清除数据来绕过签名不一致。

点击“继续安装”后，API 26+ 如未授权先打开此应用的“允许安装未知应用”设置，返回后读取真实授权。
用户拒绝时保留代理运行和已下载 APK；授权后保存配置、停止 VPN/核心、冻结并排空持久化写入，
再次校验 APK，才通过系统安装 Intent 请求用户确认。API 24–25 由系统安装器处理对应权限。
安装期间禁止应用内交互；系统负责替换应用与进程生命周期，不主动杀死进程。
取消、拒绝或失败恢复同一持久化会话并允许重试，已停止的代理由用户重新开启。

私有只读 ContentProvider 仅授予本次 `cache/app-updates/pending-<数字>-<数字>/package.apk`
的临时读权限，不暴露数据库、订阅或任意路径，也不在 Provider 初始化 Runtime。
进入系统授权/安装界面后保留 APK，防止系统回收 UI 时安装器失去读取内容；系统回收后的新会话
不会自动续装，也未恢复旧进度，用户可重新检查/下载。安装成功后从系统入口重新打开应用。

## 验证

`app_update` 覆盖平台选择、资产来源/摘要/大小/歧义、真实文件校验和独占暂存清理；
`app_update_async` 覆盖单飞、取消、过期任务、页面卸载后的失败收尾与重试；
`app_update_handoff` 在 Windows 运行无提权假安装器验证父进程退出与文件锁，
Linux 在测试专用构建中替换 pkexec 启动入口，走生产交接与安装流程，覆盖父进程在授权期间存活、
sealed 快照、DEB/RPM 参数、成功、取消、缺少授权代理和包管理失败；生产构建固定使用 /usr/bin/pkexec。`persistence` 覆盖真实 SQLite 写锁下冻结、
晚到写入拒绝、失败恢复、多来源/ID/引用文件和重开保留。

Android 每次 Gradle preBuild 运行 `:app:testAppUpdatePolicy`，使用真实文件和 JCA 覆盖摘要、
大小、私有路径、包名、签名和版本拒绝；无需设备。还需编译同源 HUI Java、应用 Java/Manifest
和 NDK 主目标。系统授权、安装取消、跨版本覆盖、数据保留及安装后重启必须在正式签名真机单独验证。

这些回归不安装真实 Release，不请求 UAC/Polkit，不代替跨版本安装/卸载、VPN 或实机验收。
实际检查与未完成边界记录在 [验收记录](l1-l2-l3-status.md)。
