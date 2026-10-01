# Glaze JSON codec 迁移记录

日期：2026-10-01。已落实评估中的第一阶段和内部路由策略迁移。
依赖选择依据见 [引入评估](glaze-evaluation.md)，此文描述实际代码边界。

## 依赖与编译

- 固定 Glaze **9.0.0**，正式 tag revision：
  `d78832c82289c61a9315bfbc35332cec9f4e93ca`。
- 仓库保存 `third_party/tarballs/glaze-9.0.0-headers.tar.gz`，包含该 revision
  的 include 和 MIT LICENSE。没有使用 Downloads checkout 的后续 main 提交。
- 归档 SHA256：`872764412db2ba94cf57e39cf6518f86c0e13728c1c668f7ed398801aadff812`；
  configure 时显式校验后解包，不依赖用户 Downloads 目录。
- `clashflux_wire` 编译唯一的 `src/wire_codec.cpp`；Glaze 头和 metadata 保持私有。
  项目仍为 C++23、最低 CMake 3.30，没有执行 Glaze 的上游 CMake 工程。
- `src/wire_codec.h` 仅提供拥有字符串、数组和明确整数类型的 C++ DTO/Result。
  不导出 Glaze 模板，不让 UI codegen 或 C++ module 接口携带 JSON 库类型。
- Android/iOS Legacy 兼容头引用同一 DTO，codec 单独编译，避免把 Glaze 模板
  合入整个领域代码的超大 TU。SQLite schema、旧库迁移和后台所有权不变更。

## 已迁移入口

| 入口 | 实际处理 |
|---|---|
| stream traffic/log | typed 解码；累计值用 int64_t，日志 payload 拥有存储 |
| 文件日志 | 显式固定 `at/level/payload` 键；缺少 payload 的历史行跳过 |
| REST API | 选择请求、错误 message、移动响应头用共享 codec；保留 curl/JNI/URLSession |
| 首页连接总量 | WebSocket 任务线程解码总量，首页只读取整数；失败保留有效计数 |
| 连接页 | 读取快照、解码和行投影在任务线程；UI 发布 StateList，过期任务不覆盖新帧 |
| 代理共享模型 | 唯一数据泵解码 /proxies，模型同时提供 typed 数据与扁平组 |
| 代理页 | 从共享 DTO 投影嵌套组，任务线程处理；版本过期的投影不发布 |
| routing/store policy | 不再导出 nlohmann JSON；EncodePolicy 返回 JSON 字符串，保留持久键名与规则顺序 |

代理 DTO 经不可变 shared_ptr 共享，UI 发起投影任务时不深拷贝整个节点表；
模型相等性按内容判断，不按新指针地址产生无效重组。解析边界集中后，页面没有第二个 JSON 解码入口。代理表内部仍需逐条验证 raw_json
记录；“共享一次解析”指同一条数据管线，不代表 JSON 字节只扫描一次。

## 数字、未知字段和失败策略

运行时 API 是投影契约：未知键允许存在，并验证被跳过的 JSON 语法。仅获取当前界面
使用的已知字段。这不适用于用户订阅或原生配置：其未知字段仍须保留或进入保真度账本。

- 流量/累计计数、日志时间、测速时间为明确 int64_t，不使用 double generic DOM。
- 可选择性与组成员采用 optional，区别缺省值、false 和空组；缺省按内核实际类型推导。
- JSON 根结构或已知字段类型错误返回 Result.error；不会把解析异常冒泡到 UI。
- 连接/代理表中语法有效但类型不符的记录跳过，记录 ignoredEntries；字符串链成员
  单独验证。整个 JSON 的语法错误不按半帧接收。
- 连接非法整帧保留上一份有效列表与总量；流量非法帧忽略；流日志解码失败保留原文。
- 代理非法整帧标记 Empty 并记录错误，不把残缺响应展示成有效 Live 数据。
- destinationPort 当前契约为字符串，与项目使用的 clash_api/libbox 快照一致；其它
  客户端若使用数值端口会被判为无效记录，未承诺接受任意第三方快照。
- 路由 policy 仍在领域层验证 match 类型与目标 ID；编码/解码错误沿用调用方的失败
  与回滚路径。序列化键名用显式 metadata 固定，不改变 SQLite user_version。
  L3 policy 现在编码 format_version=2，包含规则 ID、层级、开关、目标对象、失败策略、
  顺序与修订号；旧 v1 在领域层固化原排序。持久 policy 拒绝未知键、非法类型、
  无效枚举与未来版本，不能复用运行 API 的跳过未知键策略。

## 继续保留的解析

配置编译器仍用 nlohmann 生成/合并 sing-box JSON；原生 JSON 未知节点与大整数保持
现有 DOM 路径。Clash YAML 保留 yaml-cpp，OpenVPN/URI/CIDR 保留格式专用解析。
规则编辑继续局部修改源文本，不整文重写注释和格式。第三方框架解析未改动。

后续配置 IR 需要协议 variant、字段 presence、扩展 raw_json、来源定位与保真度映射，
再迁移生成器。不能直接用完整 DTO 替换原生 JSON，否则会丢掉尚未建模的内核字段。
本轮没有以两次序列化绕接旧 DOM 的方式宣称生成器已迁移。

## 实际验证与限制

- `cmake --build build --target clash-flux`：Linux GCC 16 构建通过，包含 HuxerUI codegen。
- `./run.sh --version`：应用版本输出 v0.3.10。
- `git diff --check`：通过。
- Android NDK 29 的 `aarch64-linux-android24-clang++ -std=c++23 -fsyntax-only`
  编译生产 `src/wire_codec.cpp`：通过。
- `cmake --build platform/android/app/.cxx/Debug/6b4s4t5h/arm64-v8a --target clash-flux`：
  完整 Android C++ 主目标通过，含 UI codegen、Legacy 领域代码与 JNI 桥接。
  修复桥接响应头 map→multimap 的插入转换。
- 从固定正式 tag 重新构建 sing-box 1.14.2 arm64 AAR 后，Android
  `./gradlew :app:assembleDebug --offline --no-daemon`：通过；执行了
  `stageBundledRuleSets`，Java 调用方与新 libbox 一起编译、打包。
- `apksigner verify --verbose --min-sdk-version 23`：Debug APK 的 v1/v2 均为 true。
  APK 内的 libbox 与新 AAR 经 NDK strip 后一致，国内两个 .srs 资产存在。
- 未运行测试套件、Glaze 示例、性能基准或远程线路握手；不提供加速倍数结论。
- Android 初次构建拒绝缺少身份元数据的旧 AAR；随后使用 Go 1.26.8、OpenJDK 17
  重新构建通过。依赖下载故障已处理，保留 go.sum 校验；工具及产物摘要见内核升级记录。
  尚未安装 APK 或验证真机运行。Windows/macOS/iOS 本轮未编译，iOS 仍按项目 TODO 暂缓。

本轮继续接入的内核映射见 [能力审查](singbox-capability-audit.md) 与
[保真度契约](singbox-layers-and-fidelity.md)。明日讨论事项见
[待决策记录](pending-decisions.md)。
