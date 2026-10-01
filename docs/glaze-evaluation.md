# Glaze 解析与序列化重构评估

评估日期：2026-10-01。结论：**适合引入作为结构明确数据的 JSON codec，分阶段减少
nlohmann 依赖；Clash YAML 兼容入口目前保留 yaml-cpp，格式专用解析继续独立维护。**
本文保留引入前的静态评估依据；2026-10-01 已完成第一阶段 JSON codec 与内部策略迁移，
实际接入范围、失败策略和验证结果见 [Glaze 迁移记录](glaze-migration.md)。

## 评估基线

直接审阅用户本地 `/home/farna/Downloads/glaze`，工作区干净：

- revision：`52971fe5fc97d0bac361c20c9cf4cf22e4dbdd12`，2026-09-29。
- describe：`v9.0.0-22-g52971fe5`，属于 v9.0.0 之后的 checkout，不等于发布 tag。
- 主要依据：本地 `docs/yaml.md`、`generic-json.md`、`unknown-keys.md`、
  `include/glaze/core/opts.hpp`、`include/glaze/yaml/read.hpp` 和 CMakeLists。
- 也静态查看了旁边 `glaze-test/main.cpp`；其普通结构体 JSON/YAML 场景不能覆盖
  本项目的模块接口、Android Legacy 合并编译和复杂订阅。没有运行这些二进制。

官方对应来源：[仓库](https://github.com/stephenberry/glaze)、
[稳定发布 v9.0.0](https://github.com/stephenberry/glaze/releases/tag/v9.0.0)、
[固定 revision YAML 文档](https://github.com/stephenberry/glaze/blob/52971fe5fc97d0bac361c20c9cf4cf22e4dbdd12/docs/yaml.md)、
[动态 JSON](https://github.com/stephenberry/glaze/blob/52971fe5fc97d0bac361c20c9cf4cf22e4dbdd12/docs/generic-json.md)、
[未知字段](https://github.com/stephenberry/glaze/blob/52971fe5fc97d0bac361c20c9cf4cf22e4dbdd12/docs/unknown-keys.md)。

## 当前项目的实际边界

| 入口 | 现状 | 建议 |
|---|---|---|
| 流量、日志、连接推送 | `src/stream.cpp` 与 UI 中的 nlohmann 动态解码 | 首批用 Glaze 解码为拥有字符串的 DTO，发布普通快照 |
| 代理/连接页、首页 | `common.cpp`、`proxies_page.cpp`、`connections_page.cpp`、`home_page.cpp` 都有 JSON 访问 | 收敛至共享 codec，页面仅投影已有模型 |
| REST 请求与响应 | `src/api.cpp` 的 JSON 请求体、message 和移动桥接响应头 | 可迁移，传输继续用现有 curl/JNI/URLSession |
| 内部路由策略 | `src/routing.cppm` 导出 JSON EncodePolicy，store/vpn 有转发门面 | 可迁移为明确 wire DTO；保持已有键名和存储内容兼容 |
| sing-box 配置生成 | `src/singbox*.inc` 动态构建 JSON | 第二阶段：公共配置模型 + 协议 variant，逐步替换 builder |
| 原生 sing-box JSON | 导入整个对象，合并托管项，保留其它节点 | 暂留动态 DOM；以后评估整数保真的 generic 与 raw_json 混合模型 |
| Clash YAML | yaml-cpp Node，经协议/组/规则/DNS 翻译并记保真度 | 当前保留 yaml-cpp，统一输入适配接口；不直接换成完整强类型加载 |
| 规则编辑 | store/profiles 文本扫描/插入，保留原文其余部分 | 需要源文本位置与局部编辑模型；Glaze 整文读写不是直接替代 |
| OpenVPN、URI、HTTP 头、CIDR | 各格式的语法与领域校验 | 继续专用适配器；不是 JSON/YAML 序列化库负责的协议 |
| SQLite 设置与 profiles | huxerui::sqlite ORM、缓存、异步写与迁移 | 保留；不因换 JSON 库更换数据库或持久化协议 |

第三方源码/HuxerUI 自己的解析不在这次应用级迁移范围内。

## 可以带来的收益

Glaze 的主要优势是直接把 JSON 读入结构体，减少动态 DOM 和页面中的反复取键；
显式 metadata 可以固定 wire 键名、容器和数字类型。它以 error_ctx / expected 返回
解析错误，便于统一转换成项目的失败结果；动态 generic 的 get/as 仍可能涉及异常，
分配失败也不会因此消失。不能把“使用无异常 API”当作进程绝不抛异常。

当前连接快照部分在 UI 线程解码；可与迁移一起搬到工作线程，UI 线程只发布模型。
保留现有唯一数据泵、订阅和用户动作写透约定，不新增页面定时器或第二份状态来源。

速度、内存、编译耗时和产物体积没有在本项目实测。上游 benchmark 与强类型场景
不能直接推成 Clash-Flux 的加速倍数；换 generic DOM 也不能获得相同收益。

## 必须处理的兼容性

### JSON 与原生配置保真度

1. 默认 `glz::generic` 所有数字存为 double，超过 2^53 的整数会失真。流量、ID、
   时间戳使用明确的 int64_t/uint64_t；动态配置候选用 `generic_u64` 等保真变体。
2. Glaze 默认拒绝未知键；仅设置 `error_on_unknown_keys=false` 会跳过未知键。
   内核快照可明确投影应用使用的子集；Clash 输入须保留未知项再进账本；原生
   JSON 的未知字段须保留并交给内核判断，不能静默丢弃或因 DTO 落后而拒绝合法新字段。
3. `unknown_read/unknown_write` 配合 extras/raw_json 可保留 JSON 扩展。持久快照
   的键和值必须有所有权，不能直接沿用文档中 string_view 键的示例跨线程传递。
4. 当前 opts 默认 `skip_null_members=true`、`error_on_missing_keys=false`。须按各
   数据契约指定选项，并区分缺省、显式 false/0、空字符串与 null；不能因为结构体
   默认初始化而把“字段未提供”变成“明确关闭”。
5. 接口错误/残缺帧的策略须逐入口定义。整体 DTO 解析失败与当前跳过无效行不是
   同一语义；失败时是否保留上一份有效快照也不能顺手改变。
6. JSON 键顺序可能改变；无需原始字节完全一致，但规则数组顺序、值、未知对象与
   用户缓存/DNS/TLS 设置必须保持。需要保留原始片段的地方使用 owning raw_json。

### YAML 与规则编辑

本地 Glaze 已提供 YAML 1.2 读写，包含 block/flow、块标量和部分锚点/别名。
文档明确：merge key 不支持，块集合锚点、别名作键支持有限，tag+anchor 组合不支持。
因此不能把“支持 YAML”当作可以覆盖现有 Clash 订阅语法。

现有 yaml-cpp 已解析普通 block/flow 锚点；本应用是否展开 `<<` 合并键仍须另行核对，
不能声称现在完整支持 merge。密码/PSK 的文本和空白、中文 tag、数字样式字符串、
DNS 的字符串/数组/对象多形态、规则引号与未知协议都须逐项对照。

替换文本编辑为整文序列化还会改变注释、锚点和格式。若要重构规则页，应让语义
解析与原文编辑共享条目位置，仍进行局部修改；Glaze 没有现成的 yaml-cpp Node
兼容门面或保持全文格式的编辑承诺。

## 工程接入方式

- 保持 C++23，使用显式 metadata 固定外部字段名；不因这次重构启用 C++26 reflection。
- 上游 CMake 最低 3.31。实际接入只编译头文件的私有 codec target，不运行上游
  CMake；本项目最低 CMake 3.30 保持有效。以后若改成 add_subdirectory 再同步要求。
- 当前本地源码未发现官方 .cppm/.ixx 模块接口。初期把 Glaze include 与实例化隔离
  在普通 .cpp 内，用函数向项目模块提供纯 C++ DTO/expected；不导出 Glaze 模板或
  模仿 nlohmann 包装后假定跨编译器模块问题已经解决。
- Android 会把模块领域代码转成 Legacy 兼容源并合为一个 TU。必须同步源码注册、
  头文件与私有命名空间处理；桌面模块通过编译并不等于 Android 通过。
- 只 include 需要的 JSON 头，避免把 HTTP/REST/其它格式也引入所有 UI TU；不替换
  现有网络与后台控制机制。同步评估 template 实例化、编译时间和 APK 大小。
- 依赖由正式版本 tag + revision + 归档 SHA256 固定，通过仓库现有 third_party
  机制接入。Downloads 路径仅用于此次源码审阅，不能成为 CI 的构建依赖。
- 若采用本地 checkout 中的额外修复，需要审查 v9.0.0 之后的差异再固定 revision；
  不跟随 main。保留 MIT 许可证与归档来源。

## 推荐迁移次序

1. **统一 JSON codec 和普通 DTO**：先处理 traffic/log/error message，再连接和代理
   快照；从页面移出 JSON 解码，保证拥有字符串和失败策略。这一阶段保留配置编译器。
2. **内部策略与移动桥接**：沿用旧键名和 JSON 格式，整理错误上下文与数字类型，
   删除 routing/store 的重复编码门面；不变更 SQLite schema 或快照交换格式。
3. **配置编译器模型**：建立协议 variant 与字段 presence，输入适配 → 规范模型 →
   保真度映射 → sing-box JSON。先迁移生成的托管配置，再评估原生 JSON 扩展保留。
4. **评估 YAML 替换**：在已授权验证的阶段检查复杂订阅兼容；满足基线才移除
   yaml-cpp，否则它继续作为外部 YAML 输入适配器，不影响其它部分采用 Glaze。

所有阶段保留现有 exact/approx/unsupported 契约、证书校验、规则顺序和平台能力。
不把库更换与手机多订阅、数据库重建、内核协议变化或新网络栈捆绑。

## 评估范围与后续验证

引入前的评估只读取源码，没有运行 Glaze 示例或基准。后续实际迁移已经实施，
验证范围见迁移记录；没有据此宣称性能提升。后续验证应关注数字与缺省值、未知字段
保留、错误帧、配置生成差异、典型 YAML 语法，以及连接快照的 UI 解码开销。
是否继续下一阶段应依据真实项目兼容性与收益，不以库自带 benchmark 替代。
