# 2026-11-15 P6 首批：公共 URI 语法与 SS SIP002

快照为 `894a8080455e066bb63811deb5097554e4c62cdd` 加前序规则/二维码、P0–P5 及本轮未提交工作区。应用 v0.3.110、sing-box 1.14.4（`c005f35300ce76427dc285481ec5695f5b179cd2`）、HUI `89359f43`、CodeEditor `ee5b0cf8`、Media `fee506de` 保持，没有新增依赖。

本轮按[输入扩展计划](../node-subscription-plan.md)单独交付公共 URI 语法和 SS SIP002；**P6 尚未整体完成**。VLESS/Trojan 下一批，HY2/TUIC/AnyTLS 与历史兼容继续延后；只读/普通来源和现有 provider 消费新格式，没有扩大扫码/直接粘贴任意节点的 UI 入口。

## 实现与规范边界

- 新增 [singbox_node_uri.inc](../../src/converter/input/uri.inc)：拥有型 userinfo/host/port/path、query 条目和 fragment；先分原始组件，再各百分号解码一次，+ 不做表单转换。保留重复 query 再拒绝，检查 UTF-8/ASCII 控制字符、百分号、主机、方括号 IPv6、端口和资源上限。
- SS SIP002 适配器直接填 ShadowsocksOptions/NodeSpec；统一名称唯一性、选择策略、校验/生成、引用、目录和次来源路径。SS/VMess 可混合并沿用单层外部 Base64；provider 不再把所有 URI 标成 vmess，SS 的筛选类型为 ss。
- 接受 Base64URL userinfo 或原始 method/password 组件，密码原文不 trim、不二次 URL 解码；2022 userinfo 不得额外 Base64URL 包装。普通方法/2022 密钥、IPv4/IPv6、Unicode/空白、+/%2B/%252F、保留字符、重名和大小边界按[节点订阅边界](../node-subscriptions.md#ss-sip002-子集p6-首批)执行。密钥有效性和运行能力仍交完整固定内核候选检查。
- SIP003 参数经 URI 解码、专用转义解析、重复/白名单校验后进入插件 variant，再由共同生成器编码一次。只开放固定内置的 obfs-local/v2ray-plugin 子集；TLS 仅接受裸标志，不把 tls=false 当关闭，不放宽证书验证，不执行任意插件程序。
- 模型将插件 host/path 改为 optional，保留未提供与显式值的区别：Clash 适配器继续写入原来的 bing.com/path/mux 默认，SIP002 则保留原生未提供行为。特别是 v2ray-plugin 未提供 host 时，不因补一个显示用 host 而强制改变 TLS SNI。
- SS 单条 URI 限 64 KiB，列表原文及解包后限 8 MiB、4096 节点；query/SIP003 参数各限 32 项。SS 明确拒绝空密码、未知 query、重复参数、未映射插件/选项、非根路径、IPv6 zone、Unicode 域名、标准 Base64 的 +/ userinfo 字母表及旧式整串 Base64。显示名单次解码后裁两侧空白，缺省 Shadowsocks 序号，凭据字节保持。
- 不改原生 JSON DOM、应用数据/schema、文件提交、下载器、手机单普通来源能力、应用/内核版本或固定依赖。

格式依据：[SIP002](https://shadowsocks.org/doc/sip002.html)、[SIP003](https://shadowsocks.org/doc/sip003.html)；固定实现依据：[SS 方法](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/docs/configuration/outbound/shadowsocks.md)、[插件参数](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/transport/sip003/args.go)、[obfs](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/transport/sip003/obfs.go)、[v2ray-plugin](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/transport/sip003/v2ray.go)。未知 query 的整份拒绝是本项目明确的严格子集，不沿用规范示例中的忽略行为。

## 回归与失败保护

[node_uri_ss_cases.inc](../../tests/node_uri_ss_cases.inc) 经生产 API 验证 **49 组正向、58 组拒绝**；还覆盖准确 4096/超量、明文/包装/BOM/CRLF、保留字符与 Unicode、标准/2022 userinfo、插件缺省/显式空 path/转义/重复/非法组合、IPv6、脱敏错误与跨格式重名。

所有正向样本独立断言完整 SS 字段，检查普通文档识别、规则投影和带稳定来源 ID 的对象目录。HTTP/file provider 覆盖混合列表、外层 Base64、真实 ss/vmess 类型筛选、筛选前整表 URI 语法拒绝；桌面次来源覆盖同名节点身份隔离。没有 inline URI 字符串语法扩展。

生产 `rule_provider_cache::Prepare` 夹具先成功提交一份 SS/VMess 原文缓存，再强制刷新为格式错误或强制候选拒绝：两条失败路径都返回失败、没有再次 commit、原已批准原文缓存保持。下载/候选操作使用独占测试目录和显式夹具回调；该检查证明生产准备/缓存边界，数据库/订阅文件的真实锁竞争及失败保护另由完整 persistence/resource 门禁覆盖。

前序 **627 组**冻结及纯模型检查、P5 的 18 组跨入口样本通过，未重录预期。保存的 P5 独立消费者与当前实现对 **27000 组**既有 Clash/VMess 输入，完整 outbounds/error/fidelity/warnings 零差分。

另用 Python 标准库的编码器生成 **2000 组**新 SS 观察（seed 20261115；含 500 组插件）：百分号/Base64URL/外层包装、原文凭据、IPv6、插件 host/path 分隔符与转义均与独立预期字节一致，节点无额外近似。输入、消费者和脚本在 `/tmp/clashflux-protocol-p6-ss`；长期回归由仓库的 SS URI/模型/入口和真实运行测试承担。

## 构建、内核检查和真实数据流

Linux Clang 21/libc++ Debug，既有 CLASHFLUX_REQUIRE_PROJECT_TESTS=ON：

```bash
cmake --build build --target clash-flux test_singbox
./build/test_singbox --protocol-baseline
./build/test_singbox --ss-uri-fixtures /tmp/clashflux-protocol-p6-ss/native-fixtures
cmake --build build
ctest --test-dir build --output-on-failure --no-tests=error -L clashflux-required
./run.sh --version
git diff --check
```

主目标及全部测试目标重建、基线/模型/新格式检查通过，版本 v0.3.110、diff 检查通过。49 份新导出配置逐一执行 `build/engines/sing-box check -c <fixture> -D <exclusive-directory>`，**49/49 接受**；没有用 check 结果代替以下真实转发。

`shadowsocks_runtime` 保留原来的 9 条 AEAD/2022/UOT 数据流，并增加：

| 场景 | 路径与真实检查 |
| --- | --- |
| sip002-aead | Base64URL userinfo 的 SS URI，默认生成选择组，原文密码含空格/+/%40，真实 TCP/UDP |
| sip002-2022-provider | 明文组件百分号编码的 2022 URI、外层 Base64、HTTP provider 原文快照，真实 TCP/UDP |
| sip002-mixed-list | SS/VMess 同列表，默认选择第一条 SS，由服务端日志和完整回包证明实际经过 SS，真实 TCP/UDP |

新增 **6 条**成功数据流，总计 **15 条**；仍使用两个官方固定内核和回环 HTTP/UDP echo，只替换测试监听/控制面/日志，保留生产编译的出站及路由。provider 场景以测试专属 `--ss-uri-provider-config` 注入原文快照，没有声明真实远程 SS 订阅 HTTP 下载验收。夹具进程和目录均独占并回收；SIP003 插件此轮只验证配置/字节，没有真实插件服务端。

完整必跑门禁 **34/34，206.61 秒通过**；singbox 2.66 秒，shadowsocks_runtime 1.66 秒，protocol_runtime 1.58 秒。包含 DNS/TLS 拒绝、provider、资源/文件保护、数据库持久化、客户端生命周期和 UI 回归，新 SS URI 检查纳入现有 singbox 项。

Android NDK 29 / arm64-v8a：

```bash
cd platform/android
HUXERUI_HOME=/home/farna/.local/share/HuxerUI ./gradlew :app:externalNativeBuildDebug :app:compileDebugJavaWithJavac
```

**37 秒通过**，native 构建/链接和 mergeDebugNativeLibs 执行，Java 已是最新；宿主 JVM 策略检查通过，HUI Java/native 保持同一固定源码。未重新打包 APK，未做真机运行。

## 代码指纹与性能

迁移前按 [P1 算法](2026-11-15-protocol-p1.md) 的原十一文件顺序，路径 + NUL + 内容 + NUL SHA256 为 `4ab032e1da3a97380f2f05cd02938fc3d115db96ebfdec83490852bf90e57092`。最终在该顺序后追加新文件 `src/singbox_node_uri.inc`，十二文件 SHA256 为 `495b2f8778fdcbb676959b84e05f87daff44c3bfd08cb33e9191c8ce57f92030`；源码快照、旧/新消费者和验证日志在 `/tmp/clashflux-protocol-p6-ss`。

完整门禁结束后，同一工具链/独立消费者对 4096 条既有 VMess 三轮交替前后测量，输出 SHA256 相同；新增 4096 条 SS（明文/Base64URL userinfo 混合）单独三轮观察，全部节点保留，输出稳定。原始记录见 [CSV](2026-11-15-protocol-p6-ss.csv)。

| Debug 输入 | 耗时中位数 | 峰值 RSS 中位数 |
| --- | --- | --- |
| 既有 VMess，before → after | 0.86 → 0.86 秒 | 34,852 → 34,944 KiB |
| 新 SS 4096 节点，仅 after | 0.49 秒 | 19,632 KiB |

测量包括包装/解析、编译及结果序列化。不同协议输入复杂度不同，不作跨格式提速结论；未测 Release、GUI、真机、候选进程检查或代理吞吐。

Windows/macOS 原生构建、Android 真机/VPN、SIP003 插件实际握手、全部方法/参数远端互通、GUI 手工检查、签名/发布未覆盖。VLESS/Trojan、HY2/TUIC/AnyTLS、SS 历史整串 Base64 和其它旧格式未实施；当前支持边界以节点订阅文档为准。未修改用户订阅、数据库、活动配置、依赖或内核；未提交、推送或发布。
