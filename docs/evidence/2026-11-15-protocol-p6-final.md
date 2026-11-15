# 2026-11-15 P6 收尾：历史 SS 与输入能力一致性

快照为 `894a8080455e066bb63811deb5097554e4c62cdd` 加前序规则/二维码、P0–P5、P6 前三批及本轮未提交工作区。应用 v0.3.110、sing-box 1.14.4（`c005f35300ce76427dc285481ec5695f5b179cd2`）、HUI `89359f43`、CodeEditor `ee5b0cf8`、Media `fee506de` 保持，没有升级内核或依赖。

本轮完成[输入扩展计划](../node-subscription-plan.md)阶段 3 的必要历史兼容与能力一致性检查。**P0–P6 本轮范围已完成本机 Linux/Android 验证**：12 类 Clash 节点走统一模型，七类协议 URI、历史 SS 与一致性门禁已接入。其它历史格式、Windows/macOS 和真机验收不在本记录的通过范围。

## 历史格式的依据与边界

[Shadowsocks 官方配置文档](https://shadowsocks.org/doc/configs.html)仍给出整串 Base64 的公开样例：chacha20-ietf-poly1305、原文密码 test/!@#:、服务器 192.168.100.1:8888。该样例作为公开解析夹具保留，未连接其地址；实际转发使用独占回环服务器与合成密码。

新增 [singbox_node_legacy_ss.inc](../../src/converter/input/legacy_ss.inc)，按 authority 中有无原始 @ 独立分流，不在 SIP002 失败后回退。旧 payload 使用标准 Base64（可无 padding）；解码后先取首个冒号、最后一个 @，完整密码保留原文字节，**不做百分号解码**，%00/%2F 仍是字符串；服务器只接受原始 host:port/方括号 IPv6。fragment 单次解码，名称/顺序/重复与混合列表统一处理。

方法与非空密码复用已有 SS 校验，旧格式拒绝 2022、Base64URL payload、插件/query、编码主机、zone、路径、嵌套 scheme、错误 UTF-8/控制字符及资源越界。输入编码不改变协议语义，转换为 exact；warnings 只记录脱敏的识别事件，不增加虚假的 Node/Approx。provider 排除的历史节点不记录该事件，坏条目仍在筛选前拒绝整份 URI 集合。

完整语法、默认与拒绝范围只维护在[节点订阅](../node-subscriptions.md)。没有为缺少必要样本的 VMess authority/Shadowrocket/JSON v1 或其它旧格式添加猜测解析器；它们继续明确拒绝。原文、数据库/schema、下载器和候选提交流程保持。

## 能力清单参与生产分流

新增 [singbox_node_catalog.inc](../../src/converter/input/catalog.inc)：登记协议、scheme/大小写、默认显示名、provider 类型、原生类型及格式 ID。生产 scheme 分流、默认名和 provider 类型读取同一登记，保持 VMess 既有的 scheme 大小写边界和各格式默认。

公开 `NodeSubscriptionCapabilities()` 返回拥有型 `NodeInputCatalog`，共 **8 种格式、7 类协议、8 个不同 scheme**（含 hy2 别名）；它还返回经过审查的内核版本/revision。没有暴露私有模型或 JSON codec，也不持久化 IR；清单不是全字段矩阵或运行时内核能力探测，字段子集继续由节点订阅文档维护。

新增必跑 [test_node_input_catalog.py](../../tests/test_node_input_catalog.py)，对照独立编写的 [node-input-catalog.json](../../tests/fixtures/node-input-catalog.json)：

- 9 份样例覆盖全部 8 种格式，并额外核对 VMess net=tls + random 的两项 Node/Approx；普通来源、HTTP/file provider、只读目录、scheme 别名/大小写和原生字段由生产 API 验证。
- 校验清单的审查版本/revision 与桌面 CMake、Android Gradle、Android 构建脚本、CI pin、实际打包内核的 version/revision；另核对 iOS 脚本 revision，只是来源检查，未构建或验收 iOS。
- 用实际固定内核 check 全部 9 份代表配置与 18 份历史 SS 正向配置，共 **27/27 接受**。
- 对内存副本分别破坏版本、revision、缺项、重复 ID、原生类型、scheme、大小写和历史标志，**8/8 漂移拒绝**。未改真实 pin 或预期文件来制造通过。

核心身份为显式审查快照，不自动跟随版本字符串升级；后续升级须重新核对映射后更新登记并通过门禁。相关流程已同步测试/编译器/依赖技能和稳定升级文档。

## 解析、入口与失败保护

[node_input_cases.inc](../../tests/node_input_cases.inc) 新增 **18 组历史 SS 正向、27 组拒绝**：标准 Base64 +/ 与 padding、公开样例、原文 @/#/?/百分号/Unicode/空白、IPv6、外部包装、显示名、错误结构/控制字符、2022/插件拒绝及与 SIP002 跨格式重名。

HTTP/file provider 核对混合原文/外部包装、完整凭据、只读目录和筛选后识别事件；同名桌面主/次来源身份隔离。生产 `rule_provider_cache::Prepare` 先批准并提交混合原文，再分别强制格式失败或候选拒绝，两条失败路径均无再次 commit，旧缓存字节保持。下载/批准采用独占夹具回调；真实数据库/文件保护由完整 persistence/resource 门禁覆盖。

原 **627 组**冻结/纯模型、18 组跨入口及前三批 URI（SS 49 正向/58 拒绝、VLESS/Trojan 43/111、HY2/TUIC/AnyTLS 59/177）通过，没有重录预期。保存的上批消费者与当前实现对 **27000 组**既有 Clash/VMess 输入的 outbounds/error/fidelity/warnings 零差分；前批 **2000 SS + 1600 VLESS/Trojan + 1800 HY2/TUIC/AnyTLS** 的完整观察也不变。

另用独立 Python 标准库编码器生成 **1500 组**历史 SS（seed 20261115），覆盖原文认证、标准 Base64、padding、外部包装、IPv4/IPv6 和端口，全部与独立预期字节及单次识别事件一致，无节点近似。临时源码、消费者、脚本、输入和日志在 `/tmp/clashflux-protocol-p6-final`；长期保护由仓库测试承担。

## 构建与真实数据流

Linux Clang 21/libc++ Debug，既有 CLASHFLUX_REQUIRE_PROJECT_TESTS=ON：

```bash
cmake --build build --target clash-flux test_singbox
./build/test_singbox --protocol-baseline
./build/test_singbox --protocol-entries
./build/test_singbox --ss-uri
./build/test_singbox --v2ray-uri
./build/test_singbox --secure-uri
./build/test_singbox --node-input-fixtures /tmp/clashflux-protocol-p6-final/native-fixtures
python3 tests/test_node_input_catalog.py build/test_singbox build/engines/sing-box .
cmake --build build
ctest --test-dir build --output-on-failure --no-tests=error -L clashflux-required
./run.sh --version
git diff --check
```

主目标/全部测试目标、版本 v0.3.110 和 diff 检查通过。最终必跑门禁 **37/37，222.01 秒**；singbox 3.04 秒、node_input_catalog 0.91 秒、shadowsocks_runtime 1.91 秒。门禁本身的 **65 项**注册/禁用/MSVC/TLS 检查通过，缺失或禁用新能力项同样导致配置失败。

[shadowsocks_runtime](../../tests/test_shadowsocks_runtime.py) 保留前三批数据流，新增两个历史 SS 场景：普通来源 AES-256-GCM、外层 Base64 的 HTTP provider 快照 + chacha20-ietf-poly1305。生产编译的默认选择组连接两个固定内核和回环 HTTP/UDP echo，原文密码含空格/+/%40，服务端日志及回包证明实际走 SS。新增 **4 条**成功数据流，连同旧 SS/2022/UOT/URI 共 **19 条**；不修改生产路由/出站，只替换测试监听、控制面和日志。

provider 使用现有测试专属 CLI 注入已验证原文快照，没有声明真实远程旧格式订阅下载验收；所有进程与临时目录回收，未读写用户订阅或数据库。

Android NDK 29 / arm64-v8a，最后源码快照：

```bash
cd platform/android
HUXERUI_HOME=/home/farna/.local/share/HuxerUI ./gradlew :app:externalNativeBuildDebug :app:compileDebugJavaWithJavac
```

**20 秒通过**，native 构建/链接和 mergeDebugNativeLibs 执行，Java 已是最新；首次加入完整实现后的该命令另有 46 秒通过记录。没有重新打包 APK 或进行真机运行。

## 源码指纹与性能

按 [P1 算法](2026-11-15-protocol-p1.md) 的原十一文件顺序，追加前批三个 URI 文件，本轮前十四文件 SHA256 为 `f028beaa12253163a70b229307269b17c1382a2ad2ba90586d74b0f47a8cecc3`；最终再追加 `src/singbox_node_catalog.inc`、`src/singbox_node_legacy_ss.inc`，十六文件 SHA256 为 `723fbdb486052f1e73ae5ec0da1b461ac724f687c0da30091e4c21399c24347a`。该快照包括新增公开能力 DTO/API，原编译选项/结果布局未变。

最终门禁结束后，同一工具链/独立消费者，对四组既有 4096 节点输入各做三轮交替前后测量，输出 SHA256 各自相同；新增 4096 条历史 SS 单独三轮观察，全部节点及识别事件保留、输出稳定。原始记录见 [CSV](2026-11-15-protocol-p6-final.csv)。

| Debug 输入 | 耗时中位数 | 峰值 RSS 中位数 |
| --- | --- | --- |
| 既有 VMess，before → after | 0.88 → 0.89 秒 | 34,396 → 35,000 KiB |
| 既有 SIP002，before → after | 0.49 → 0.50 秒 | 19,028 → 19,892 KiB |
| 既有 VLESS/Trojan，before → after | 0.63 → 0.64 秒 | 28,944 → 29,844 KiB |
| 既有 HY2/TUIC/AnyTLS，before → after | 0.64 → 0.62 秒 | 27,052 → 27,752 KiB |
| 新历史 SS，仅 after | 0.51 秒 | 23,516 KiB |

测量包含包装/解析、编译及结果序列化；既有输入耗时接近，RSS 增量不足 1 MiB，不作跨格式提速或 Release/GUI/吞吐结论。

Windows/macOS 原生构建、Android 真机/VPN、GUI 手工检查、所有方法/参数远端互通、此前未覆盖的定时多端口跳转和 0-RTT 恢复时序、签名/发布仍未验收。其它历史格式继续按实际样本独立评估；本机门禁不代替全部平台通过。未提交、推送或发布。
