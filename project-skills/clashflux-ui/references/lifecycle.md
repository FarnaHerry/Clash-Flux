# 生命周期、应用模型与异步动作

## 组合与资源

- UseString/UseEnvironment 等只能组合期调用；筛选名称、默认名先解析成拥有型字符串/DTO，再按值捕获给回调/任务。选择器取消/异常应保留表单、恢复 busy，并反馈失败。
- composable 返回值必须挂载到视图树：hcg 的 Scope 工厂只有挂载时执行，丢弃返回值会令 Lifecycle/副作用根本不注册。
- 一次性 API（如 SystemTrayHandle::OnActivate）不能在可重组体内直接注册；用 Lifecycle、call_once 或 ApplicationHook。框架已内置 Lifecycle/多观察者的 window.OnCloseRequest、application.OnLifecycleChanged 不套用该限制。
- 应用模型 State 必须带初值（例如 State<T> x{T{}}），默认构造无 cell，读写或当 Lifecycle 依赖会抛异常。要求非空的框架容器（如 IndexedPages）须在构造前判空并准备兜底。
- hook 调用顺序稳定；不可见一级页在所有 State/Lifecycle/UseXxx 之后用 active 门控重内容，保留页面 State 与生命周期。当前桌面 DesktopPageSwitcher 和移动端使用 Pager，不按旧的桌面 IndexedPages 描述实现。
- 门控重子树需验证滚动恢复；不能假设重新连接 ScrollController 会自动恢复偏移。历史设备性能数字只作历史证据，不当作当前验收结果。

## 共享数据与写透

- application_hooks 安装 application service，用 context.Provide 和 UseService<T>；模型持 State/StateList，由应用级数据泵在 UI 线程发布，页面组合期读。共享数据不由每页再挂定时器。
- 镜像模型到页面本地 State 时，Lifecycle 显式依赖模型 State 以及会阻止镜像的切换/拖动状态；条件解除后补齐镜像。
- 设置从 SettingsModel 读，不组合期直接 setting(...)；需要 hydrate 后补读的消费者用 SettingsView::ready。条件拉取的修订涵盖 hydrate，RequestSync 也强制读取，避免永久停在首帧空缓存。
- 用户操作直接写透权威模型，不能只改本地 State 等下一轮泵。KV 同步写缓存后发布同值；失败可能的慢操作完成后回读并发布，失败回退前检查当前目标/操作版本，不能覆盖更新意图。
- 发布前检查版本，包括 ABA；不能先写模型再拒绝旧结果。写透字段的权威来源与写入目标一致，区分配置意图和运行回读。

## 运行控制与订阅

- 首页、设置、托盘都用 UseCoreControl/CoreControl::Run；CoreModel::operations 持全应用唯一命令与版本，冲突动作单飞。应用级 TaskScope 持运行操作，发起视图卸载/缩放后仍完成回读与反馈；服务安装/退出清理保留各自边界。
- CoreView::tunIntent/modeIntent/systemProxyIntent 为配置，ProxyActive/TunActive/vpnState/systemProxyActive/core 为实际状态；桌面待命、监听与接管遵循[桌面生命周期](../../../docs/desktop-proxy-lifecycle.md)，停止内核不伪造保存意图已关闭，保存意图不伪造接管成功。
- 配置端口来自 SettingsModel、运行端口来自内核快照；系统代理和复制环境变量使用实际监听端口，新端口重启生效。统一 SettingsModel::SetPorts/SetEnvShell、CoreModel::SetAllowLan/SetIpv6，不复制校验流程。
- 日常运行控件在首页，托盘共用同入口；设置保留网络参数、运行中重启、版本/服务管理，Android 保活；配置页维护来源/规则。系统代理和 TUN 不改成互斥，桌面窄屏不隐藏能力。
- URL 导入用 profile_import_task.h 的 LaunchProfileImport：应用任务持工作/回调，UI 先结束 busy 再反馈，捕获异常、拒绝重复点击。文件选择器/相机/导航延时仍用页面生命周期，选定后的实际提交用应用任务。
- 增删/切换完成在 UI 线程 ProfilesModel::PublishCached，先发布内存权威列表再反馈/返回，不用空 reload 或仅 syncTick 代替。后台 profile_list_model 的快照只有发布成功才确认版本；切换/拖动延后不丢重读义务，动作淘汰旧快照，失败切换恢复权威选择。
- 订阅切换复用 LaunchProfileSelection：相同待切 ID 幂等，单份准备加最后一个待切目标；改选取消旧准备，发布前查请求号（含 ABA）。候选校验/交接/预览/提示复用产物，详见 UI 文档“操作控件”。

回归真实卸载 await 中的发起视图，覆盖成功/异常结束 loading、重试、版本/ABA、切换/拖动后镜像补齐和无额外数据库写入时的延迟重读。
持久化/文件提交的完整保护另读 [数据技能](../../clashflux-data/SKILL.md)，不要用 UI 测试代替真实 SQLite 锁竞争。
