# sing-box 官方 GUI 交互研究

研究日期：2026-09-30；方法为读取官方文档与源码，**没有运行官方 GUI**。
下面客户端源码为当日 `main`，会继续变化；不能把它的全部能力当成 Clash-Flux 固定
sing-box 1.14.2 的能力。内核字段仍逐项按固定 revision 核对。

## 来源与结构

官方[客户端目录](https://sing-box.sagernet.org/clients/)列出 Android、Apple、桌面
客户端。此次桌面交互以
[sing-box-for-desktop](https://github.com/SagerNet/sing-box-for-desktop) 和其官方
[dashboard 子模块](https://github.com/SagerNet/sing-box-for-desktop/blob/main/.gitmodules)
为依据；没有用第三方 Clash 面板代替官方实现。

桌面 renderer 通过 host adapter 挂载共享 dashboard：
[main.tsx](https://github.com/SagerNet/sing-box-for-desktop/blob/main/src/renderer/src/main.tsx)、
[host.ts](https://github.com/SagerNet/sing-box-for-desktop/blob/main/src/renderer/src/host.ts)。
这种结构可借鉴“界面消费能力与状态、平台层负责文件和服务”的边界；本项目继续使用
HuxerUI application service，不迁移到 Electron/React。

## 交互事实与本项目采用方式

| 领域 | 官方当前源码行为 | Clash-Flux 开发约束 / 后续采用 |
|---|---|---|
| 配置来源 | 一个 `selectedId`，选择后更新活动配置；更新非活动远程配置不会替换活动内容 | 手机保持单活动配置；桌面多来源计划另建模型，不能把“保存多配置”当成“同时生效” |
| 分组与测速 | 由组的 `selectable` 决定是否接受节点点击；测速期间按钮禁用，失败反馈；选择使用 pending 值等待权威状态 | 仅 selector 提供选择；urltest 展示自动结果。clash_api 没有 selectable 时使用已核对的内核组类型，不臆造接口字段 |
| 状态与数据 | daemon 类型化 RPC 和订阅流提供服务、组、连接、日志、端点等状态 | 先保持桌面 clash_api / Android CommandClient；可评估独立控制 adapter，不能为用一个字段先替换服务架构 |
| 连接 | 按活动/已关闭筛选、排序、看详情；支持关闭单条或全部；已关闭记录有数量上限 | 保持单一连接模型，补来源/规则追溯时依赖实际返回数据；关闭操作异步并反馈失败 |
| 日志 | 搜索/级别筛选，暂停时冻结显示快照，恢复后跟随新日志；底层与显示都有上限 | 暂停只冻结阅读，不停内核日志；现有文件日志边界保留，不写 SQLite |
| 原生配置编辑 | 编辑内容与保存内容分离；约 1s 调度内核配置检查，补全期间暂缓检查；平台 host 提供 schema、格式化和校验 | 优先补配置诊断，再做原生编辑器。检查不能阻塞 UI，错误不能让应用退出 |
| 高级端点工具 | API 版本决定 USB/IP、OpenVPN/OpenConnect、Taildrop 等可用性，独立端点状态视图 | 做版本 + 编译标签 + 平台权限能力表；只有实际接入生命周期和控制后才开放入口 |

上表对应官方源码：

- 单活动配置：[profiles.ts](https://github.com/SagerNet/sing-box-for-desktop/blob/main/src/main/profiles.ts)。
- 分组选择/测速：[GroupsView.tsx](https://github.com/SagerNet/sing-box-dashboard/blob/main/src/views/GroupsView.tsx)。
- 类型化状态订阅、日志和关闭连接缓存上限：[api/daemon.ts](https://github.com/SagerNet/sing-box-dashboard/blob/main/src/api/daemon.ts)。
- 连接操作：[ConnectionsView.tsx](https://github.com/SagerNet/sing-box-dashboard/blob/main/src/views/ConnectionsView.tsx)。
- 日志阅读：[LogsView.tsx](https://github.com/SagerNet/sing-box-dashboard/blob/main/src/views/LogsView.tsx)。
- 配置编辑：[ProfileViews.tsx](https://github.com/SagerNet/sing-box-dashboard/blob/main/src/views/ProfileViews.tsx)。
- 能力判定：[app/capabilities.ts](https://github.com/SagerNet/sing-box-dashboard/blob/main/src/app/capabilities.ts)。

Apple 官方[特性文档](https://sing-box.sagernet.org/clients/apple/features/)进一步显示
Network Extension 形态和系统匹配能力存在平台差异，因此功能入口不能只按布局宽度
开放。本项目 iOS 仍暂缓，不因官方有 Apple 客户端就宣称本项目已支持。

## 当前开发决策

1. 先补 L2 的原生映射与保真度：DNS policy/HTTP3、解析依赖、拨号字段、inline
   rule_set 与逻辑规则子集已接入；下一步补外部规则集生命周期与其余映射，
   继续沿用[能力审查](singbox-capability-audit.md)。
2. 随后补 L3 原生字段编辑及诊断，借鉴官方的状态驱动、检查反馈和能力门控。
3. 官方单活动配置不满足桌面主/次来源最终形态；按
   [桌面订阅编排设计](desktop-subscription-orchestration.md) 分阶段实现。
4. 本轮不新增多订阅 UI；手机继续单活动代理订阅，不增加多订阅编排。
