---
name: clashflux-data
description: 修改 Clash-Flux 持久化、数据库迁移、订阅导入刷新删除、文件提交或 CLI 单实例数据路径时使用。
---

# 持久化与订阅保护

修改前检查 `src/persistence*`、`src/sqlite_schema.*` 与实际调用路径；本文保存数据安全契约，平台数据所有权另见 [平台技能](../clashflux-platform/SKILL.md)。

## 数据与迁移

- settings/profiles 使用 `clashflux.persistence` 的内存缓存 + 异步 SQLite ORM 落库；`clashflux.db` 只是同步接口转发门面，新代码直接依赖 persistence。
- 数据库记录和 profile 文件共同构成订阅。升级、启动、导入、刷新、候选失败都不得清空、重建或“修复性”删除数据；CI 失败不能通过删除数据库或订阅目录恢复。
- 老库 user_version=0 通过 0→1 事务重建表并复制全部数据；schema 再改须升版本并追加 Migration 链。失败明确终止启动并保留原库，不退回空内存缓存。
- hydrate 完成前不将空缓存当“没有订阅”写回。内容修订号必须涵盖 hydrate，显式 RequestSync 仍强制拉取；运行期设置从 SettingsModel 读取。

## 原子文件与删除

- 刷新/编辑先写独占临时文件、完成解析与候选检查，成功后原子替换；失败保留旧文件、数据库引用与活动配置。通用下载先关闭并检查写入，再执行 DownloadOptions.validate；临时目录用 RAII，Windows 覆盖不得先删旧文件。
- 用户删除由 persistence 在删除事务提交后，核对持久化与当前缓存的全部引用，再在任务线程清理；只响应明确删除的那个 ID，不启动扫描或按文件异常批量删元数据。
- 仅使用 open 显式传入的文件根；未配置根、越界、无法确认安全或仍被任何订阅引用的文件保留，不扫描补删。
- 本地首次导入可先存为未参与来源，启用时检查；不能借此绕过刷新/编辑的候选检查。Android 完整配置检查由后台 libbox 执行，不能宣称有桌面 CLI 预检查。

## 异步一致性与进程

- 同类 flush 串行，按 key/ID 的写入版本确认；await 后不得无条件清脏（包括 ABA），失败保留待写版本。
- 日志不进库：应用和内核推送共用 core/logs.log，不迁移旧分离日志；内核 stdout/stderr 诊断文件继续保留。
- 需要数据库的 CLI 必须在应用 Runtime 内通过 setPendingCommand → cli::run → flush → 退出执行，不能在 Runtime 外直读写持久化。
- GUI/CLI 先 acquireOrActivate；非 owner CLI 通过 tryForwardCommand 转发，owner 用 servePendingCommands 执行并回传输出/退出码，不能出现第二份 Runtime、托盘或缓存。
- version/help/service 是 isDirectCommand 快路径；无显示服务的 root service run 不初始化 GUI。资源归本进程用 RAII；服务/其它进程持有的资源按 pidfile、存活检查和 socket 协议管理，不恢复旧 OpenVPN 独立进程架构。
- 首帧只读配置的特殊边界见 [启动快照](../../docs/ui-development.md#启动设置快照)；不扩大到普通设置和订阅。

## 必须证明的回归

使用 [测试技能](../clashflux-test/SKILL.md) 执行 persistence 与基础检查。每次触及上述边界，至少覆盖：

- 多个 profile、不同来源、稳定 ID、全部被引用文件与迁移后成功重开，不能只测一条示例。
- 刷新/编辑/候选失败后旧数据、引用和活动配置仍在；删除只影响指定 ID，仍被其它行引用的文件保留。
- 让生产 flush 在真实 SQLite 写锁后挂起，再插入新动作，核对提交和重开结果；覆盖 ABA、失败重试与同类串行，不能用顺序写入替代竞争回归。
