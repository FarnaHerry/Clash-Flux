# Windows 原生网络服务

Windows 数据面由原生 SCM 服务 `ClashFluxService` 托管，服务类型为
`SERVICE_WIN32_OWN_PROCESS`，账户为 `LocalSystem`。GUI/CLI 保持普通用户运行；
服务直接启动同一软件版本的 `sing-box.exe`，并拥有 PPTP RAS 会话、服务器绕行路由
与主 TUN 的路由补偿。服务不创建 HuxerUI Runtime，不打开配置数据库，也不访问
订阅缓存。系统代理仍由 GUI 在当前用户会话内通过 WinINet 设置。

PPTP 连接保留主 TUN 拨号，候选配置完成准备后再交接一次主内核；服务拥有
物理 DNS 原生出口、服务器绕行及拨号期间的路由捕获。旧服务必须支持
`PPTP_LIVE_DIAL` 才允许活动 TUN 下拨号，细节和验证边界见
[PPTP 连接交接](native-pptp-handoff.md)。

## 安装、更新与卸载

设置 → 内核服务提供安装/更新和卸载按钮。只为 `service install` / `service uninstall`
子命令请求 UAC，不提升整个 GUI。CLI 也可在管理员终端执行：

```powershell
& '.\clash-flux.exe' service install
& '.\clash-flux.exe' service status
& '.\clash-flux.exe' service uninstall
```

安装源旁必须有 `engines/sing-box.exe` 和应用依赖 DLL，发行包已包含这些文件。
安装程序将自身、相邻 DLL 与内核复制到
`%ProgramFiles%\Clash-Flux-Service\bin`，所有载荷和目录明确限制为 SYSTEM/管理员
可写。SCM 的可执行路径加引号，使用 `clash-flux.exe service run` 入口。
服务配置自动启动和失败重启，但开机只启动控制服务，收到 GUI/CLI 的 START 才启动
数据面，不读取数据库猜测用户配置。

安装时记录原始用户 SID；GUI 在 UAC 前读取 SID，并经内部
`service install --owner-sid <SID>` 参数传递，支持由另一个管理员账户批准安装。
该用户可以查询/启动 SCM 服务并使用控制管道；更改服务配置、卸载与停止服务本身
仍需管理员权限。每台机器只有一份服务，目前只授权一个安装用户。

Windows 服务版本必须与客户端完全相同。每次 GUI 启动、设置 hydrate 完成后，
应用在任务线程检查已安装服务，并在自动运行内核之前完成版本同步；同版本不重装，
未安装不主动安装。检查/更新失败只反馈一次并记日志，暂停本次自动运行内核，
设置页仍可手动安装/更新。服务检查期间禁用设置页的服务管理按钮，更新与内核启停
复用 CoreStore 生命周期锁；CLI 查询不发起 GUI 启动维护或 UAC。

版本不符时，服务收到无路径参数的 `UPGRADE`，通过命名管道实际客户端 PID 取得
其可执行文件路径。自更新仅接受位于 Program Files、逐级目录/文件均由
SYSTEM/管理员/TrustedInstaller 持有且不允许普通用户修改的载荷；拒绝用户可写
目录、可写 DLL、reparse point 与异常 ACL。应用、相邻 DLL 与固定内核先复制到受保护
的 `upgrade-source`，然后从旧服务载荷制作独立的 `updater` 副本，用 LocalSystem
静默启动 `service apply-upgrade <继承的事件句柄>`。助手只有收到服务在回复后触发的
事件才切换载荷；不运行 UI、不接受客户端提供的源路径或安装用户 SID，保留原授权用户。
助手不在内核 Job Object 中，能够在 SCM 停止旧服务后替换文件并重新启动 SCM。
客户端重试 VERSION 握手，确认新服务匹配当前版本才继续启动内核。

**Program Files 安装版的上述服务同步不请求 UAC**；可写目录中的便携版仍通过设置
按钮授权更新。没有 UPGRADE 指令的旧版服务需手动更新一次，此后使用自更新。
不自动从普通用户可写目录替换 LocalSystem 的代码，也不在自动检查失败后请求 UAC。
更新先暂存全部载荷，再停止服务、切换载荷并重启；失败尝试恢复旧载荷和授权 SID。
卸载只移除专用服务目录和 SCM 注册，不删除用户数据库、订阅或 GUI 安装目录。
应用 MSI 卸载（含"应用和功能"）会同步停止并删除该服务注册，不再要求先手动卸载服务；
MajorUpgrade 升级路径不执行该清理（服务存活，代理不中断）。服务载荷目录因受保护
ACL 不随 MSI 删除，彻底清理需以管理员删除或经服务自身卸载流程。

## IPC 与生命周期

控制管道为 `\\.\pipe\ClashFlux.Service.v1`，只读查询使用同名管道的 `.Query`
端点与独立工作线程，PPTP 拨号不阻塞 VERSION/STATUS。两条管道均拒绝远程客户端，DACL 只授权
SYSTEM、管理员与安装用户，安装用户没有创建同名管道实例的权限。客户端核对管道
服务端 PID 与 SCM 注册进程；服务端读取命令后模拟客户端并核对实际令牌。
命令有长度上限、超时和严格字段编码，路径、密码、接口名使用 UTF-8 十六进制字段，
避免空格/换行混入协议。连接、读写使用 overlapped I/O；SCM STOP 会取消等待，
未完成的 I/O 在缓冲区和句柄释放前完成回收。

START 只接受本机绝对 JSON 路径。服务以请求用户身份检查读取权限并取得配置快照，
随后在受保护目录保存运行快照；内核固定为服务载荷中的 `engines/sing-box.exe`。
`-D` 保留客户端配置父目录，以保持现有相对资源/缓存语义；stdout/stderr 写入受保护的
`sing-box.service.log`。服务不提供任意程序、shell 或系统命令执行接口。

内核放入设置了 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的 Job Object；服务异常退出
也会清理内核。服务检测内核退出时释放 TUN 补偿路由；STOP 先释放补偿，再终止内核。
SCM STOP/SHUTDOWN 还会断开服务持有的 PPTP 会话。Windows 停止内核仍使用强制终止，
不宣称与 POSIX SIGTERM 等价的优雅退出。

关闭窗口隐藏到托盘时继续运行；正常退出 GUI 仍先停止 VPN/内核，再退出。
独立 CLI `core start` 可让服务内核在 CLI 退出后驻留，`core stop` 负责停止。
Windows 不再回退到由 GUI 直接 spawn 数据面；尚未安装服务时通过设置安装后再启动。

PPTP 能力由原生连接监视线程持续检测，并与连接状态一起发布到应用级模型。
在 GUI 已运行时安装/更新或重启服务，无需重启 GUI 即可恢复 PPTP 引擎可用状态；
尚未拨过号的订阅卡片也使用同一能力快照。连接前再次检查服务版本并启动已安装的
停止服务，再刷新引擎选择；不可用时保留主内核与已有连接。能力刷新不重建适配器，
不会因此断开其它连接或释放路由。具体连接错误优先于通用的“PPTP 引擎不可用”。

Windows RAS 客户端同一时间只支持一条 PPTP 会话（`vpn::PptpSingleSession()`）。
存储层在停止主内核之前拒绝第二条 PPTP 拨号，错误信息指明占用中的连接；
订阅页在第二条 PPTP 卡片点击“打开”时给出同样的原因；PPTP 卡片展示该平台边界。
该限制不约束 OpenVPN——sing-box endpoint 仍可并发多条。

订阅页的连接/断开动作在任务线程逐条执行，每条结束后携带 `VpnStore` 权威快照
返回 UI 线程，并通过 `VpnModel::Publish` 立即更新卡片；失败也走相同的发布路径，
不等待一秒轮询或整批拨号结束。启动初始化完成同样直接发布。发布时使在途旧轮询
失效，数据泵通过 `PublishPolled` 核对请求序号，避免把卡片回写成旧的连接中/已连接
状态；没有用户动作时仍由唯一数据泵同步监视线程发现的能力和断线变化。

## 验证

`service_protocol` 验证 Unicode/空字段、非法编码、NUL、长度上限和歧义字段。
Windows 专属 `windows_service` 回归使用独占 SCM 服务、受保护的测试目录和假内核，
需要管理员运行，不配置真实 VPN、TUN、系统代理或订阅。覆盖安装、版本握手、普通
安装用户控制、异常回复、长操作期间查询、Job Object 清理、服务崩溃重启、卡住的管道客户端、卸载
及用户配置保留。Windows CI 的项目门禁要求注册且启用该测试，不允许静默跳过。
自更新回归用两份不同版本的假服务验证普通安装用户发起 UPGRADE、SYSTEM 助手跨
进程停止/替换/重启、版本重新握手与原用户授权保留；同时检查不可信客户端目录、
普通用户可写 DLL 被拒绝且旧服务保持可用，以及普通管理员不能直接调用内部助手。
通用 `service_protocol` 还覆盖启动策略的同版本跳过、未安装跳过、更新失败、不可达
与停止服务恢复，确认失败结果不被当作更新成功。
PPTP 能力查询还覆盖安装前、安装后、长操作中和 SCM 重启后；通用 `vpn` 回归
覆盖能力从不可用恢复、再次失效与恢复，检查实际拨号/路由和其它连接不被重置。

本地 Linux 构建/回归和 Windows 交叉编译仅验证对应范围；真实 Windows GUI、UAC、
TUN 数据流与远端 PPTP 连接仍需 Windows 机器验收，不能以假内核测试代替。

实现使用微软的 [SCM 入口](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-startservicectrldispatcherw)、
[命名管道访问控制](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights)
和 [客户端模拟](https://learn.microsoft.com/en-us/windows/win32/ipc/impersonating-a-named-pipe-client) API。
自更新来源使用 [管道客户端 PID](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getnamedpipeclientprocessid)
与 [文件安全描述符](https://learn.microsoft.com/en-us/windows/win32/api/aclapi/nf-aclapi-getnamedsecurityinfow) 校验。
