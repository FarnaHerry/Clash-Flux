# 2026-11-15 P6 第二批：VLESS / Trojan URI

快照为 `894a8080455e066bb63811deb5097554e4c62cdd` 加前序规则/二维码、P0–P5、P6 SS 及本轮未提交工作区。应用 v0.3.110、sing-box 1.14.4（`c005f35300ce76427dc285481ec5695f5b179cd2`）、HUI `89359f43`、CodeEditor `ee5b0cf8`、Media `fee506de` 保持，没有新增依赖。

本轮交付[输入扩展计划](../node-subscription-plan.md)的 1b：VLESS/Trojan URI 直接进入 NodeSpec，与 SS/VMess 混合并消费现有规则投影、目录、provider 和桌面次来源。**P6 尚未整体完成**；HY2/TUIC/AnyTLS URI、历史分享格式与新扫码/直接粘贴节点 UI 入口未实施。

## 实现与规范边界

- 新增 [singbox_node_v2ray_uri.inc](../../src/converter/input/v2ray.inc)，复用公共 URI 拆分/单次解码，直接填写协议、TLS、Reality 和传输 variant，不经过临时 Clash 节点。UUID 判断抽为公共助手，既有 VMess 的判断和错误文案保持；provider 筛选注册真实 vless/trojan 类型。
- VLESS 默认无 TLS、仅 encryption=none；Trojan 必须 TLS，完整 userinfo 保留为密码，包含冒号、+、百分号、Unicode 和两侧空白时不拆分、不 trim、不二次解码。显示名与源身份沿用已有规则。
- 本 URI 子集 TLS 默认 fp=chrome、SNI=远端 host；不继承外围 Clash 指纹，不把传输 Host 用作 SNI。默认验证证书，显式 insecure/allowInsecure 仅 true/false/1/0，同值别名接受、冲突拒绝；指纹近似由共同校验器记账。
- 仅开放 TCP/raw、WS、HTTPUpgrade、HTTP/2 和 gRPC 的明确子集；Vision 仅 TLS/Reality + 原生 TCP。HTTP/2 要求 TLS，避免原生明文 HTTP/1.1 回落；gRPC 仅 gun 且 serviceName 必填，不静默丢弃非空 authority 或其它模式。
- Reality 仅 VLESS TCP，必须显式 fp 与 32 字节无 padding Base64URL 公钥；sid 可缺省/空或至多 8 字节偶数 hex。不接受普通 TLS insecure 例外，即使 false。XHTTP/KCP/QUIC、MLKEM encryption、ECH/pin、spx/pqv、URI UDP 编码扩展和所有其它未知参数整份拒绝。
- query 保留重复项后拒绝，编码后重名不能覆盖；每 URI 64 KiB、query 32 项、整列表 8 MiB/4096 节点。共享主机、方括号 IPv6、端口、UTF-8 和控制字符检查，错误不含原始 URI/UUID/密码。

完整字段/默认/接受与拒绝矩阵只维护在[节点订阅边界](../node-subscriptions.md#vless--trojan-uri-子集p6-第二批)。格式依据为 [Xray 分享提案](https://github.com/XTLS/Xray-core/discussions/716)、[Trojan URL](https://github.com/trojan-gfw/trojan-url/blob/master/README.md) 和 [v2rayN 验证开关](https://github.com/2dust/v2rayN/blob/master/v2rayN/ServiceLib/Handler/Fmt/TrojanFmt.cs)；目标能力依据固定 [VLESS](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/protocol/vless/outbound.go)、[Reality 客户端](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/common/tls/reality_client.go) 和 [V2Ray 传输](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/docs/configuration/shared/v2ray-transport.md)。这些来源不代表本项目支持它们的全部格式或能力。

## 解析、入口与失败保护

[node_uri_v2ray_cases.inc](../../tests/node_uri_v2ray_cases.inc) 经生产公开 API 验证 **43 组正向、111 组拒绝**，独立断言完整协议 JSON、凭据字节、默认值、近似数量、文档验证、规则投影及只读目录。包含 IPv6、单次转义、完整 Trojan 密码、名称缺省/编码、外部 Base64、全部开放传输、Host/SNI 分离、指纹、显式安全开关、Reality/Vision 和冲突/未知字段的拒绝。

HTTP/file provider 覆盖四协议混合/外层 Base64、真实类型筛选、URI 格式检查先于排除、格式默认不继承外围 global-client-fingerprint；桌面同名 VLESS 主/次来源身份隔离。生产 `rule_provider_cache::Prepare` 先批准并提交完整混合原文，再分别强制格式错误或候选拒绝：两条失败路径均无再次 commit，已批准缓存字节保持。下载/批准操作使用独占夹具回调；数据库/文件保护的实际回归另由完整 persistence/resource 门禁覆盖，没有修改生产数据/schema/提交流程。

原有 **627 组**冻结及纯模型、18 组跨入口、SS 的 49 正向/58 拒绝检查均通过，没有重录预期。保存的 P6 SS 独立消费者与当前实现对 **27000 组**既有 Clash/VMess 输入，完整 outbounds/error/fidelity/warnings **零差分**；前批 **2000 组** SS 凭据/插件观察的完整结果也无变化。

另用 Python 标准库独立编码器生成 **1600 组** VLESS/Trojan 输入（seed 20261115），覆盖随机原文密码/UUID、IPv4/IPv6、TLS/无 TLS、SNI/Host、ALPN、传输、Vision 及外层包装，逐项与独立预期字节一致，节点无额外近似。临时脚本/输入/结果在 `/tmp/clashflux-protocol-p6-vt`；长期保护由仓库的公开入口和真实运行回归承担。

## 构建、固定内核与真实数据流

Linux Clang 21/libc++ Debug，既有 CLASHFLUX_REQUIRE_PROJECT_TESTS=ON：

```bash
cmake --build build --target clash-flux test_singbox
./build/test_singbox --protocol-baseline
./build/test_singbox --protocol-entries
./build/test_singbox --ss-uri
./build/test_singbox --v2ray-uri-fixtures /tmp/clashflux-protocol-p6-vt/native-fixtures
cmake --build build
ctest --test-dir build --output-on-failure --no-tests=error -L clashflux-required
./run.sh --version
git diff --check
```

主目标和全部测试目标构建、版本 v0.3.110、diff 检查通过；43 份导出正向配置逐一执行 `build/engines/sing-box check -c <fixture> -D <exclusive-directory>`，**43/43 接受**。该 check 不替代以下实际连接验证。

新增必跑 [test_v2ray_uri_runtime.py](../../tests/test_v2ray_uri_runtime.py)，两个官方固定内核作为客户端/服务端，经生产 URI 编译输出及默认选择组转发到回环 HTTP/UDP echo：

| 协议 | 实际场景，每项均检查 TCP 和 UDP 回包 |
| --- | --- |
| VLESS（9 项） | 明文 TCP、TLS TCP、TLS Vision、明文 WS、明文 gRPC、TLS gRPC provider、TLS HTTP/2、明文 HTTPUpgrade、Reality Vision |
| Trojan（5 项） | TLS TCP、TLS WS、TLS gRPC、TLS HTTP/2 provider、TLS HTTPUpgrade；原文密码含空格/+/%40/冒号/Unicode |

合计 **14 项、28 条成功数据流**，断言服务端原生入站和目标连接日志，避免误走直连。Host/SNI 故意不同；TLS fixture 用 OpenSSL 私有证书，仅注入独占客户端配置的信任根，保留生产默认校验和 SNI。Reality 临时生成密钥对、连接本地 TLS 1.3 伪装服务，保留自身认证校验，没有添加 insecure 或普通证书例外。另一个错误 TLS 身份场景同时检查 x509/bad-certificate 和目标 HTTP 请求计数不变，不能用超时当通过。

provider 场景用测试专属 `--uri-provider-config` 注入已验证的 HTTP 原文快照（外层 Base64），未声称真实远程 VLESS/Trojan 订阅下载验收。只替换测试监听、控制面、日志和独占信任根，不修改真实订阅/系统信任；所有进程和临时目录回收。脚本可直接复跑：

```bash
python3 tests/test_v2ray_uri_runtime.py build/test_singbox build/engines/sing-box /usr/bin/openssl
```

必跑名单新增 v2ray_uri_runtime，缺失/禁用均让配置失败；project_test_gate 的 **61 项**注册/禁用/MSVC/TLS 检查通过。Linux 完整必跑门禁 **35/35，214.48 秒**；singbox 2.70 秒、v2ray_uri_runtime 2.84 秒。包括原有 DNS/TLS、SS/UOT、其它协议、provider、数据/资源保护、生命周期和 UI 检查。

Android NDK 29 / arm64-v8a：

```bash
cd platform/android
HUXERUI_HOME=/home/farna/.local/share/HuxerUI ./gradlew :app:externalNativeBuildDebug :app:compileDebugJavaWithJavac
```

**38 秒通过**，native 构建/链接及 mergeDebugNativeLibs 执行，Java 已是最新；HUI Java/native 保持同一固定源码。未重新打包 APK，未做真机运行。

## 源码指纹与性能

按 [P1 算法](2026-11-15-protocol-p1.md) 的原十一文件顺序，追加 `src/singbox_node_uri.inc`，本轮前十二文件 SHA256 为 `495b2f8778fdcbb676959b84e05f87daff44c3bfd08cb33e9191c8ce57f92030`；最终再追加 `src/singbox_node_v2ray_uri.inc`，十三文件 SHA256 为 `412cc22b4f2f08ccc6059c947e18808e7a5cdf147c4e803b3dd7f13f51053c8d`。前源码、消费者、输入、日志和指纹在上述临时目录。

完整门禁结束后，同一工具链/独立消费者、4096 节点输入，对既有 VMess 和 SS 各做三轮交替前后测量；两组输出 SHA256 各自相同。新增 4096 条 VLESS/Trojan（TLS WS/gRPC）单独三轮观察，全部节点保留、输出稳定。原始记录见 [CSV](2026-11-15-protocol-p6-vt.csv)。

| Debug 输入 | 耗时中位数 | 峰值 RSS 中位数 |
| --- | --- | --- |
| 既有 VMess，before → after | 0.88 → 0.87 秒 | 34,292 → 35,324 KiB |
| 既有 SS，before → after | 0.47 → 0.47 秒 | 18,812 → 19,788 KiB |
| 新 VLESS/Trojan，仅 after | 0.61 秒 | 29,996 KiB |

测量包含包装/解析、编译和结果序列化；两种既有输入耗时接近，RSS 约增 1 MiB，不作跨协议提速结论。未测 Release、GUI、真机、内核候选进程或代理吞吐。

Windows/macOS 原生构建、Android 真机/VPN、全部参数组合和远端客户端互通、错误 Reality 公钥的实际拒绝、GUI 手工检查、签名/发布未覆盖；本地 Reality 正向转发不冒充完整抗探测验收。HY2/TUIC/AnyTLS 与历史格式仍未实施。未提交、推送或发布。
