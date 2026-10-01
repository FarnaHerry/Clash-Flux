# 基础层继续补齐

日期：2026-10-01。延续已有 sing-box 1.14.2 与 Glaze 9.0.0 基线；本轮处理
编译语义、输入校验和诊断，不增加移动端多订阅编排或新 UI。

## 规则条件共用转换

`compileLeafCondition` 仅生成匹配条件，动作由外层规则添加。普通规则、AND/OR/NOT
和 inline classical 共用转换器，避免不同入口的 CIDR/端口/平台判定分叉。

- DOMAIN-REGEX 与平台进程/包名条件可进入逻辑及 inline classical。
- Linux 桌面 UID 可进入路由逻辑；IP-VERSION 的 4/6 可进入普通/逻辑路由。
- 固定版本 HeadlessRule 没有 user_id/ip_version；inline 中出现它们时整份拒绝，
  不能按 route schema 生成规则集。进程/包名仍按各平台能力限制。
- 非法目标 CIDR 不再原样写入启动配置。逻辑保留现有层数、条件数限制；
  未接入子条件仍整条拒绝。嵌套 GEO/RULE-SET 尚未转换。
- 正则保持原文，由内核校验其语法；本轮未增加 Go 正则解析器或宣称所有
  Clash/Go 正则扩展完全等价。

依据：固定版本的 [route schema](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/option/rule.go)
与 [HeadlessRule schema](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/option/rule_set.go)。

## 默认出口与规则顺序

第一个有效 MATCH 终止源规则列表；其后的源规则本来不可达，不提升到 MATCH 前执行。
显式 MATCH,DIRECT 保留 DIRECT，只有没有 MATCH 才采用首个 selector 作应用默认。
MATCH 的目标缺失、被拒绝或为 PASS 时编译失败，账本记 Rule/Unsupported，
不更换出口。现有调用方沿用编译失败处理，本轮不承诺新增完整计划事务或零中断重启。

REJECT-DROP 在普通规则与 MATCH 中使用 reject + method: drop；REJECT 保留内核
default 行为。这只涉及内核处理的连接，不提供停核后的系统级阻断承诺。依据：
[固定版本 reject 选项](https://github.com/SagerNet/sing-box/blob/af6e64c3b69e6132ebaee0e1a3d24e93903f6709/option/rule_action.go)。

## provider 与来源诊断

先统计所有 provider 声明，再转换内容。同名声明整体拒绝，避免先转换第一个后仍把
它当成有效来源。失败/重名声明继续遮蔽历史国内别名；不换用内置 CN 内容。
payload 失败包含索引、原条目和原因，整份原子转换；rules 根类型及非字符串行明确记账。
这些诊断仍属于当前订阅内的来源定位，未增加跨订阅 source ID 或下载/缓存管理。

## 节点专用 DNS policy

普通与节点专用 policy 复用完整域名和最长后缀优先级。新增
proxy-server-nameserver-policy，仅在 proxy-server-nameserver 有可转换默认服务器时
生效；只为匹配的域名代理节点选择 domain_resolver，不改变普通 DNS 查询 rules。
匹配时归一大小写与尾部点；未命中回节点默认 DNS。DNS 依赖仍参与图检查。
完整 DNS 通配、规则集策略、hosts/FakeIP 与 direct follow-policy 尚未实现。输入语义见
[Mihomo DNS 配置](https://wiki.metacubex.one/config/dns/#proxy-server-nameserver-policy)。

## 验证

本轮未新增或运行测试套件、远程握手或性能基准。

- `cmake --build build --target clash-flux`：Linux 目标构建通过。
- `./run.sh --version`：v0.3.10；`git diff --check`：通过。
- 在 Android 目录按既有环境执行
  `./gradlew :app:assembleDebug --offline --no-daemon`：完整 Debug APK 构建通过。
  使用已有固定 1.14.2 AAR，包含 C++ 与 Android 平台分支。
- `apksigner verify --verbose --min-sdk-version 23`：v1/v2 均为 true；
  APK 中国内 GEOIP/GEOSITE 两个 .srs 资产存在，未跳过打包任务。

Debug APK 路径仍为 `platform/android/app/build/outputs/apk/debug/app-debug.apk`，
本轮增量构建覆盖该产物；昨晚升级记录中的摘要对应当时产物。
没有安装 APK 或进行真机验证。Windows/macOS/iOS 本轮未编译，iOS 仍 TODO。


## 继续：资源与下载基础

- `DownloadOptions.validate` 在文件关闭且写入无错误之后、目标替换之前执行。
  每个请求使用同目录独占临时目录，RAII 按先关流再清理顺序处理失败/异常；
  不再让同目标并发请求共享 `.part`。并发提交仍是最后完成者覆盖，未增加版本仲裁。
- Windows 使用 `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)` 覆盖已有文件，
  失败保留旧目标；POSIX 用 rename。未宣称跨平台断电持久性保证。
- curl 响应头在每次 HTTP 状态行重置，最终响应不继承重定向跳的订阅元数据；
  C 回调捕获异常并向 curl 报错。
- 编译输出 typed `ruleSetResources`，只收录自有 GEO，包含已命中缓存；桌面在
  启动任务中按七天 mtime 刷新。原生 JSON 的来源与下载策略交给内核，不能从任意
  tag 拼本地路径。清理限定当前清单，Android 固定打包 GEO 不运行时更新或清理。
- 编译只读缓存，坏缓存清理由任务层负责。替换前轻量检查 SRS 魔数、固定版本
  允许的版本上限与 zlib 头，筛掉错误页/短头/未来版本。未实现完整解压、校验和
  或规则解析，不保证能识别所有尾部截断；内核仍执行完整规则读取。
- `compileFidelity` 与启动失败状态保留已生成的账本。语法错误只有 error 的情形
  仍不会虚构保真度条目。

这批改动建立后续外部 provider 所需的下载提交边界；没有实现外部 HTTP/file
provider 或普通订阅内容校验，没有新增多订阅功能。


### 此批验证范围

Linux `cmake --build build --target clash-flux` 与 `./run.sh --version`
（v0.3.10）通过，`git diff --check` 通过。Android 按上述既有环境执行
`./gradlew :app:assembleDebug --offline --no-daemon` 构建通过；
`apksigner verify --verbose --min-sdk-version 23` 的 v1/v2 均为 true，
APK 中两个国内 GEO 资产均存在。仍未新增/执行测试套件、实际下载回滚场景或
真机验证；Windows 的替换路径未在 Windows 编译运行，macOS/iOS 未编译。
