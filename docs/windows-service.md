# Windows 原生网络服务

Windows 数据面由原生 SCM 服务 `ClashFluxService` 托管，服务类型为
`SERVICE_WIN32_OWN_PROCESS`，账户为 `LocalSystem`。GUI/CLI 保持普通用户运行；
服务直接启动同一软件版本的 `sing-box.exe`，并拥有 PPTP RAS 会话、服务器绕行路由
与主 TUN 的路由补偿。服务不创建 HuxerUI Runtime，不打开配置数据库，也不访问
订阅缓存。系统代理仍由 GUI 在当前用户会话内通过 WinINet 设置。

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

Windows 服务版本必须与客户端完全相同。软件更新后，在设置中更新服务；版本不符
会拒绝启动数据面，不自动从普通用户可写目录替换 LocalSystem 的代码。
更新先暂存全部载荷，再停止服务、切换载荷并重启；失败尝试恢复旧载荷和授权 SID。
卸载只移除专用服务目录和 SCM 注册，不删除用户数据库、订阅或 GUI 安装目录。
卸载应用安装包前应先卸载其独立网络服务。

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

## 验证

`service_protocol` 验证 Unicode/空字段、非法编码、NUL、长度上限和歧义字段。
Windows 专属 `windows_service` 回归使用独占 SCM 服务、受保护的测试目录和假内核，
需要管理员运行，不配置真实 VPN、TUN、系统代理或订阅。覆盖安装、版本握手、普通
安装用户控制、异常回复、长操作期间查询、Job Object 清理、服务崩溃重启、卡住的管道客户端、卸载
及用户配置保留。Windows CI 的项目门禁要求注册且启用该测试，不允许静默跳过。

本地 Linux 构建/回归和 Windows 交叉编译仅验证对应范围；真实 Windows GUI、UAC、
TUN 数据流与远端 PPTP 连接仍需 Windows 机器验收，不能以假内核测试代替。

实现使用微软的 [SCM 入口](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-startservicectrldispatcherw)、
[命名管道访问控制](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights)
和 [客户端模拟](https://learn.microsoft.com/en-us/windows/win32/ipc/impersonating-a-named-pipe-client) API。
