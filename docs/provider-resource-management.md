# Provider 请求协调与计时

2026-11-02 第一批：统一下载入口、在途合并和分段诊断。跨验证/应用阶段
准备结果复用仍未实现；2026-11-03 已接入下文独立到期调度，2026-11-04 补齐桌面 Provider
常驻下载器与有界空闲 easy/连接复用；原生 remote rule_set 继续由 sing-box 管理。

## 所有权与提交

`CoreStore` 持有一份 `provider_resources::Manager`，规则/ASN 与代理集合通过
`downloadProvider` 共用入口。编译器仍只读快照、输出需求，不联网。Android 管理器在 UI
进程的任务线程内使用现有 Java TLS 桥接，不将 SQLite/缓存提交搬进 libbox 后台进程。

管理器默认最多三个活动下载、64 个不同在途请求（包括等待项）；满额拒绝新请求，
同一请求的等待者仍可加入。身份比较使用完整来源/资源身份和完整请求策略，包括 URL、
请求头、条件请求、大小、超时、重试、TLS 与代理选项，不按缓存文件名摘要判断。
凭据只用于内存中的身份比较，不输出到新诊断日志。

仅共享有界原始响应，不共享候选批准、暂存路径或提交结果。每个候选在自己的独占目录
读取/转换响应，完成完整检查后才提交自己的缓存和 options；失败保留原文件与快照。
桌面下载所有者直接使用自己的已关闭下载文件，其他消费者写各自暂存，避免所有者多写一次。
304 仍由缓存准备层核对对应已验证正文；不同条件头不合并。

等待者可独立取消；排队的下载所有者取消后，存活消费者可用自己的 loader/暂存目录接手。
已经开始的同步 IO 由原任务线程执行到底，取消的候选不提交，但响应仍可供其他消费者使用。
没有分离线程；准备器继续等待全部工作结束后才移除暂存目录。完成/失败/异常后移除在途项，
只由当前消费者持有响应，后续手动更新会发起新请求，不形成第二份长驻正文缓存。

现有生命周期锁与版本核对继续负责候选/运行发布顺序，未为了合并请求放开这把锁。
因此普通准备入口通常仍串行；共享机制有并发回归证据，尚无生产提速比例或公网对照。
管理器本身不持有 HTTP 连接；连接复用归下文独立下载器，独立调度仍归 CoreStore。

## 桌面下载连接复用（2026-11-04）

`CoreStore::providerDownloader_` 保持在 Runtime 内复用，独立于控制器端点与 secret。
`downloadToFile` 在客户端内独占借用 easy handle，每个客户端最多保留 3 个空闲 handle，
每个 handle 的 libcurl 连接缓存上限为 3；活跃并发仍由现有 Provider Manager 限制为 3。
池锁只保护借出/归还，不包围网络传输；普通候选入口的生命周期锁并未放开。

复用身份核对原始站点 authority（含 URL 凭据）、证书校验选项、代理三态、相关代理/CA
环境值（区分未设置和空值）及请求头。仅 If-None-Match / If-Modified-Since 不分裂传输身份，仍逐次设置，
完整请求的在途合并和持久缓存身份继续逐字节核对所有头。身份及凭据只在内存，不进日志。
返回池前 reset 请求选项，清除回调、输出缓冲、请求头和错误缓冲指针；TLS/代理/跳转和
大小限制逐次设置。传输失败销毁该 handle；候选验证失败仍保留原文件，不影响其它请求。
传输在候选 validate 前归还，验证内嵌套准备 Provider 不占着外层连接。

依据 libcurl 的 [easy reset](https://curl.se/libcurl/c/curl_easy_reset.html) 与
[线程安全契约](https://curl.se/libcurl/c/threadsafe.html)：reset 保留连接/DNS/TLS session
缓存，同一 handle 任一时刻只由一个线程使用。缓存项不保存响应正文或候选批准。
Android Provider 仍使用 Java TLS 桥接，iOS URLSession 路径未改变，未宣称获得相同池化收益。
主订阅刷新当前仍创建自己的独立下载器；本批重点是一次准备内多个 Provider 和常驻 Runtime
后续 Provider 更新，不宣称跨进程 CLI、完全退出后或任意服务端均能复用连接。

## 诊断字段

所有时间来自单调时钟；并发工作字段是各项耗时之和，嵌套字段也有重叠，不能相加当总耗时。

| 日志/字段 | 定义 |
|---|---|
| `profile-download network_us` | 桌面主订阅 curl perform 耗时，含同步写回调 |
| `candidate_us` / `file_commit_us` | 主订阅校验回调（含锁等待和 provider 准备）/原文原子替换 |
| `profile-update apply_us` | 运行中订阅刷新后的运行重配耗时，包含其内部准备与交接 |
| `profile-providers total_ms` | 一次完整资源准备的墙钟时间 |
| `task_queue_us` / `download_queue_us` | 准备轮次等待工作线程/管理器等待下载名额 |
| `shared_wait_us` | 等待相同在途请求，不重复统计其网络/字节/建连工作 |
| `fetch_us` / `network_us` | 下载所有者执行 loader/实际传输调用的耗时；前者还含暂存读取和重试间隔 |
| `provider_validation_us` / `discovery_compile_us` | 缓存准备层内容验证/完整编译，格式转换也包含在编译耗时内 |
| `finalize_check_us` / `cache_commit_us` | 目标核对、GEO 准备与最终候选检查/缓存原子替换 |
| `download_requests` / `shared_requests` | 传输调用与重试次数（不拆分重定向跳数）/共享在途响应的调用数 |
| `cache_hits` / `stale_fallbacks` / `not_modified` | 准备层未到期内容复用/失败沿用旧内容/实际 304 响应数 |
| `download_bytes` | 桌面 curl 下载计数；Android 为平台返回的正文长度累计，失败时可能没有完整字节证据 |
| `measured_requests` / `new_connections` | 有 curl 建连证据的尝试数/`CURLINFO_NUM_CONNECTS` 计数 |

`new_connections=0` 且 `measured_requests=0` 表示没有建连证据，不能解读为连接复用。
Android 记录平台传输耗时和正文长度，不臆造系统连接池计数；主订阅仍沿用现有 Android
传输诊断。本批没有改变 TLS 默认校验、DIRECT 下载或单值请求头/重定向约束。
新日志只输出上述阶段/计数和数值来源 ID，不输出 URL、provider 名称、正文或请求头。

## 回归边界

`singbox` 覆盖同请求合并、请求策略隔离、成功/异常后的重试、并发/容量限制、取消等待者、
取消排队所有者后接手，以及两个完整候选共享规则/代理下载而独立检查提交。
`rule_provider_download` 通过真实 HTTP(S) 检查传输计数、304 和 TLS 拒绝。
`test_profile_selection_runtime.py` 通过生产 CLI 核对四项 provider 请求的诊断、失败留旧、
并发选择/编辑/删除与重开。当前命令和验收结果见 [主线状态](l1-l2-l3-status.md)。

## 单项刷新（2026-11-03）

订阅外部资源行和 `profile resource-refresh` 通过完整身份选择一个 HTTP provider。
显式刷新只强制这一项，其他过期资源沿用已验证快照；必需快照缺失则拒绝，并提示先应用订阅。
来源原文在准备前后核对，候选/目标检查通过才提交；交接失败恢复所选缓存。待命计划保留
无业务监听状态，停止或未参与来源只更新缓存，不自动启动。2026-11-03 起受支持的自定义 Geo DAT/MMDB 也复用此路径；MMDB 国家/ASN 声明也开放该按钮，见 [Geo 资源](geo-resources.md)。
全局原生规则集继续使用 sing-box remote rule_set 的缓存身份轮换，不经应用下载管理器；
未运行或未引用只安排下次引用下载，Android 不开放这一手动强制刷新。
生产隔离回归 `resource_management_runtime` 覆盖多 ID CRUD/重开、禁用引用删除保护、
只下载所选项、未使用代理集合刷新、坏响应留旧与下载中来源变更拒绝。原生 SRS 匹配与
TLS 拒绝继续由 `personal_rule_sets_runtime` 检查。完整产品契约见
[全局与订阅资源](subscription-personal-rules.md)，实际验收见[主线状态](l1-l2-l3-status.md)。

## 独立到期调度（2026-11-03）

应用 Runtime 持单份 30 秒任务，隐藏/重建窗口不重复注册，资源页不挂计时器。
CoreStore 在 worker 上扫描现存 Clash YAML 声明；HTTP rule/proxy provider 的正 interval
（秒）、Geo 开启 auto-update 后的正间隔（小时）独立于主订阅更新，未参与来源也可更新缓存。
每轮最多三项，轮换完整身份，运行锁忙则跳过；失败退避与原文 guard、取消、缓存时间保留
遵守 [Geo 生命周期](geo-resources.md#下载缓存与应用)。无网络操作进入 UI 或编译器。

成功只刷新所选项，不下载主订阅或其它未到期资源；所需的其它依赖必须已有可验证快照。
运行中配置字节改变才完整交接，可能重启 sing-box；停止/未参与只更新缓存，不启动代理。
更新后发布应用级资源修订，资源页可回读缓存状态；失败写应用日志并保留旧缓存/运行配置。
CLI `profile resources-update-due` 复用同一生产调度 pass，单实例转发共享退避状态。

此调度仅覆盖订阅显式 HTTP/Geo 声明。原生 remote rule_set 仍由 sing-box 调度；
未声明自定义 ASN MMDB 时的隐式 ipverse ASN 依赖仍在配置应用时检查每日到期，
本地文件未加监听。桌面应用完全退出、Android UI Runtime 被回收后暂停，不在系统服务/
libbox 后台进程写数据库。跨资源运行重配合批与跨验证/应用阶段准备复用仍未实现。
