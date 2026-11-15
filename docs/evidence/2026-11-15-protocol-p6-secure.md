# 2026-11-15 P6 第三批：HY2 / TUIC / AnyTLS URI

快照为 `894a8080455e066bb63811deb5097554e4c62cdd` 加前序规则/二维码、P0–P5、P6 SS/VLESS/Trojan 及本轮未提交工作区。应用 v0.3.110、sing-box 1.14.4（`c005f35300ce76427dc285481ec5695f5b179cd2`）、HUI `89359f43`、CodeEditor `ee5b0cf8`、Media `fee506de` 保持，没有新增依赖。

本轮交付[输入扩展计划](../node-subscription-plan.md)的阶段 2：HY2/TUIC/AnyTLS URI 直接进入 NodeSpec，可与前四类节点混合。**P6 尚未整体完成**；历史分享格式、能力清单一致性检查与新的扫码/直接粘贴节点 UI 入口未实施。

## 实现与规范边界

- 新增 [singbox_node_secure_uri.inc](../../src/converter/input/secure.inc)，直接填写协议/TLS 模型；不经过临时 Clash 节点。共享 URI 语法用显式选项区分默认端口、可选认证和端口集合，SS/VLESS/Trojan 仍使用原来的严格选项，VMess JSON 判断保持。
- HY2/hysteria2 与 AnyTLS 缺省端口 443；TUIC v5 必须显式单端口。HY2 auth 可以省略/为空，userpass 的冒号作为整个密码保留；AnyTLS 完整 userinfo 是非空密码，不裁空白；TUIC 必须 UUID:password，先分原始组件再各解码一次，密码可显式为空，不把旧 token 或编码冒号猜成合法结构。
- HY2 接入 salamander/gecko 和原文混淆密码；显式端口集合直接校验为中立 PortHopping，最多 128 段，拒绝反向、重叠、重复和坏成员，不修正成其它范围。集合不生成 server_port，两个时长均为原生默认 30s；不从 Clash 注入非标准 URI 带宽或时长选项。
- TUIC 既有凭据、拥塞控制、zero-RTT、disable-sni 共用模型；新增可选的类型化 TuicUdpRelayMode，输出固定原生 native/quic 中继。格式适配器严格处理已核对的参数/别名、同值合并和冲突拒绝；没有扩展既有 Clash 字段白名单或改变其默认。
- 三者必须启用 TLS，默认验证证书，SNI 缺省远端 host；QUIC 不注入 TCP uTLS。AnyTLS 使用该格式的 chrome 默认或显式指纹，保持原生会话参数，不继承外围 Clash 全局指纹；近似由共同账本记录。TUIC disable-sni 不取消 server_name 的证书身份校验。
- 组件只解码一次，+、百分号、Unicode 和密码空白保持；每 URI 64 KiB、query 32 项、整列表 8 MiB/4096 节点。重复/未知 query、冲突别名、错误语法或未支持语义整份拒绝，错误不输出原始 URI/UUID/密码。provider 注册 canonical hysteria2（含 hy2 scheme）/tuic/anytls 类型。

完整字段/默认/拒绝矩阵只维护在[节点订阅边界](../node-subscriptions.md)。格式依据：[HY2 URI](https://v2.hysteria.network/docs/developers/URI-Scheme/)、[端口跳转](https://v2.hysteria.network/docs/advanced/Port-Hopping/)、[AnyTLS URI](https://github.com/anytls/anytls-go/blob/main/docs/uri_scheme.md)；TUIC 对照 [v5 协议](https://github.com/tuic-protocol/tuic/blob/master/SPEC.md)、[v2rayN 分享实现](https://github.com/2dust/v2rayN/blob/master/v2rayN/ServiceLib/Handler/Fmt/TuicFmt.cs) 和固定参考库 [explodeTuic](https://github.com/Aethersailor/SubConverter-Extended/blob/144f7c6bf6f7dd3da3d280730914b77f9df54631/src/parser/subparser.cpp)。不沿用参考库的默认填充、坏值回落或忽略未知字段。

目标能力依据固定 [HY2](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/docs/configuration/outbound/hysteria2.md)、[TUIC](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/protocol/tuic/outbound.go) 和 [uTLS](https://github.com/SagerNet/sing-box/blob/c005f35300ce76427dc285481ec5695f5b179cd2/common/tls/utls_client.go)。pinSHA256 的完整证书摘要不能冒充原生公钥 pin；它与 ECH/Realm、TUIC v4/heartbeat/udp_over_stream、AnyTLS 会话 query 及其它未列字段均拒绝，即使空值也不忽略。

## 解析、入口与失败保护

[node_uri_secure_cases.inc](../../tests/node_uri_secure_cases.inc) 经生产公开 API 验证 **59 组正向、177 组拒绝**，独立断言完整协议 JSON、原文认证、默认端口/TLS/指纹、端口集合、中继模式和近似数量。还覆盖 IPv6 缺省/显式端口、128/超量范围、缺省显示名、单次转义、未知/重复/冲突参数、脱敏错误、文档识别、规则投影和只读目录。

HTTP/file provider 使用七协议混合和外层 Base64，核对真实类型筛选、先验证完整 URI 再排除、QUIC 不继承 random 指纹、AnyTLS 使用格式默认；桌面同名 AnyTLS 主/次来源隔离。生产 `rule_provider_cache::Prepare` 先批准并提交完整混合原文，再强制格式错误或候选拒绝，两次均无再次 commit，原已批准缓存字节保持。下载/候选批准使用独占夹具回调；真实数据库/文件失败保护由完整 persistence/resource 门禁覆盖，没有修改生产数据/schema/提交流程。

原 **627 组**冻结及纯模型、18 组跨入口、SS 49 正向/58 拒绝与 VLESS/Trojan 43 正向/111 拒绝全部通过，没有重录预期。前一批独立消费者与当前实现的 **27000 组**旧 Clash/VMess 观察零差分；另有 **2000 组 SS、1600 组 VLESS/Trojan** 的完整 outbounds/error/fidelity/warnings 不变。

Python 标准库独立编码器另生成 **1800 组**新格式输入（seed 20261115；三种协议各 600），覆盖原文/空凭据、UUID、IPv4/IPv6、默认端口、HY2 集合/混淆、TLS、ALPN、TUIC 中继/拥塞/zero-RTT/SNI 及外层包装，全部与独立预期字节一致，无额外节点近似。临时源码、消费者、脚本、输入和日志在 `/tmp/clashflux-protocol-p6-quic`；长期保护由仓库的公开入口和实际运行回归承担。

## 构建、固定内核与真实数据流

Linux Clang 21/libc++ Debug，既有 CLASHFLUX_REQUIRE_PROJECT_TESTS=ON：

```bash
cmake --build build --target clash-flux test_singbox
./build/test_singbox --protocol-baseline
./build/test_singbox --ss-uri
./build/test_singbox --v2ray-uri
./build/test_singbox --secure-uri-fixtures /tmp/clashflux-protocol-p6-quic/native-fixtures
cmake --build build
ctest --test-dir build --output-on-failure --no-tests=error -L clashflux-required
./run.sh --version
git diff --check
```

主目标/全部测试目标、版本 v0.3.110 和 diff 检查通过。59 份正向配置逐一执行 `build/engines/sing-box check -c <fixture> -D <exclusive-directory>`，**59/59 接受**；配置 check 不替代以下真实连接。

新增必跑 [test_secure_uri_runtime.py](../../tests/test_secure_uri_runtime.py)，生产 URI 编译的默认选择组连接两个官方固定内核和回环 HTTP/UDP echo：

| 协议 | 实际场景，每项检查 TCP 和 UDP 回包 |
| --- | --- |
| HY2（5 项） | userpass 原文密码、无 auth、salamander、gecko HTTP provider、单端口范围的引导连接 |
| TUIC（4 项） | 原生默认、native 中继 + BBR + zero-RTT 配置、quic 中继 + new_reno + disable-sni + 自定义 ALPN provider、显式空密码 + cubic |
| AnyTLS（3 项） | 原生会话默认、IP 证书身份、firefox 指纹/自定义 ALPN provider |

合计 **12 项、24 条成功数据流**，以服务端入站/目标日志和完整回包证明经过协议服务器。临时 OpenSSL 私有证书仅注入独占客户端配置，保留 URI 的 SNI/校验，不修改系统信任。AnyTLS 的 IP 场景另用透明 TCP 观察器读取 ClientHello 后原样转发到原生服务器：实际未发送 SNI 主机名，证书 SAN IP 校验及 TCP/UDP 仍成功。

三种协议各有错误证书身份场景，均同时检查 x509/bad-certificate 和目标 HTTP 请求计数未增加；TUIC 拒绝场景同时开启 disable_sni，证明未因此取消身份验证，不把超时当通过。provider 用测试专属 `--uri-provider-config` 注入外层 Base64 的已验证 HTTP 原文快照，没有声称真实远程订阅下载验收。所有进程和临时目录回收，只修改测试监听、控制面、日志和独占信任根。

```bash
python3 tests/test_secure_uri_runtime.py build/test_singbox build/engines/sing-box /usr/bin/openssl
```

端口范围场景使用单元素范围，验证生成、原生引导连接和数据流，**没有验收定时多端口切换**；zero-RTT 配置转发通过，不代表实测 TLS 恢复的零往返时序。

新运行项纳入必跑名单，缺失或禁用均让配置失败；project_test_gate 的 **63 项**注册/禁用/MSVC/TLS 检查通过。Linux 完整必跑门禁 **36/36，230.50 秒**；singbox 2.96 秒、secure_uri_runtime 2.72 秒，包含原有 DNS/TLS、SS/UOT、其它协议/provider、数据/资源保护、生命周期和 UI 回归。

Android NDK 29 / arm64-v8a：

```bash
cd platform/android
HUXERUI_HOME=/home/farna/.local/share/HuxerUI ./gradlew :app:externalNativeBuildDebug :app:compileDebugJavaWithJavac
```

**48 秒通过**，native 构建/链接及 mergeDebugNativeLibs 执行，Java 已是最新；HUI Java/native 维持相同固定源码。未重新打包 APK，未做真机运行。

## 源码指纹与性能

按 [P1 算法](2026-11-15-protocol-p1.md) 的原十一文件顺序，追加 `src/singbox_node_uri.inc`、`src/singbox_node_v2ray_uri.inc`，本轮前十三文件 SHA256 为 `412cc22b4f2f08ccc6059c947e18808e7a5cdf147c4e803b3dd7f13f51053c8d`；最终再追加 `src/singbox_node_secure_uri.inc`，十四文件 SHA256 为 `f028beaa12253163a70b229307269b17c1382a2ad2ba90586d74b0f47a8cecc3`。快照和消费者在上述临时目录，公开模块接口未改变。

完整门禁结束后，用同一工具链/独立消费者、4096 节点输入，对既有 VMess、SS、VLESS/Trojan 各做三轮交替前后测量，输出 SHA256 各自相同。新增 4096 条 HY2/TUIC/AnyTLS（含集合/混淆、中继和指纹）单独三轮观察，全部节点保留、输出稳定。原始记录见 [CSV](2026-11-15-protocol-p6-secure.csv)。

| Debug 输入 | 耗时中位数 | 峰值 RSS 中位数 |
| --- | --- | --- |
| 既有 VMess，before → after | 0.90 → 0.89 秒 | 34,404 → 34,784 KiB |
| 既有 SS，before → after | 0.50 → 0.49 秒 | 19,136 → 19,616 KiB |
| 既有 VLESS/Trojan，before → after | 0.63 → 0.66 秒 | 29,256 → 29,720 KiB |
| 新 HY2/TUIC/AnyTLS，仅 after | 0.62 秒 | 27,660 KiB |

测量包含包装/解析、编译和结果序列化；既有输入耗时接近、RSS 增量不足 0.5 MiB，不作跨协议或提速结论。未测 Release、GUI、真机、内核候选进程或代理吞吐。

Windows/macOS 原生构建、Android 真机/VPN、定时多端口跳转、0-RTT 恢复时序、全部参数组合和远端客户端互通、GUI 手工检查、签名/发布未覆盖；历史分享格式和能力清单一致性检查仍待后续。未提交、推送或发布。
