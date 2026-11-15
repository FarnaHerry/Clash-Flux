# 同仓转换子项目：结构整理证据

日期：2026-11-15。代码基线：`894a8080455e066bb63811deb5097554e4c62cdd` 加前序 P0–P6 和当前未提交工作区，应用 v0.3.110，sing-box 1.14.4。项目方向和职责边界以[转换子项目规划](../converter-project.md)为准。

## 本轮范围

十个实现片段归入 src/converter，保留 clashflux.singbox 模块、公开接口和现有构建/发布；未建立独立库目标、CPS 包或第二条转换实现。源码入口见[目录说明](../../src/converter/README.md)。

| 整理前路径 | 当前位置 |
| --- | --- |
| src/singbox_protocol_model.inc | [protocol/model.inc](../../src/converter/protocol/model.inc) |
| src/singbox_protocol_parse.inc | [protocol/clash.inc](../../src/converter/protocol/clash.inc) |
| src/singbox_protocol_validate.inc | [protocol/validate.inc](../../src/converter/protocol/validate.inc) |
| src/singbox_protocol_emit.inc | [protocol/singbox.inc](../../src/converter/protocol/singbox.inc) |
| src/singbox_node_catalog.inc | [input/catalog.inc](../../src/converter/input/catalog.inc) |
| src/singbox_node_uri.inc | [input/uri.inc](../../src/converter/input/uri.inc) |
| src/singbox_node_legacy_ss.inc | [input/legacy_ss.inc](../../src/converter/input/legacy_ss.inc) |
| src/singbox_node_v2ray_uri.inc | [input/v2ray.inc](../../src/converter/input/v2ray.inc) |
| src/singbox_node_secure_uri.inc | [input/secure.inc](../../src/converter/input/secure.inc) |
| src/singbox_node_subscription.inc | [input/subscription.inc](../../src/converter/input/subscription.inc) |

移动前保留本地副本，逐文件核对：九个片段字节完全相同；subscription.inc 仅五个局部 include 改名。[singbox.cpp](../../src/singbox.cpp) 仅五处 include 路径改变，包含顺序与私有实现域保持；[singbox.cppm](../../src/singbox.cppm) 字节不变。纯模型测试仅改三个 include 路径，断言和冻结样例未重录。

[AndroidLegacy.cmake](../../cmake/AndroidLegacy.cmake) 将原 subscription 依赖项换成十个新路径，每个登记一次。组/DNS/规则/provider、资源和应用策略的其余实现仍在原位置，待后续按契约收敛；未改订阅原文、数据库/schema、缓存提交或候选批准流程。

## 源码快照与等价核对

沿用 [P6 收尾](2026-11-15-protocol-p6-final.md) 的十六文件顺序，将旧路径按上表替换，再以相对路径 + NUL + 内容 + NUL 计算 SHA256：

- 移动前：`723fbdb486052f1e73ae5ec0da1b461ac724f687c0da30091e4c21399c24347a`，与 P6 收尾快照一致。
- 整理后：`daeb15c7c6597f89949c7dd98f8acb99a1e85b36646eb4065d3ad30030f5cac6`。

路径与 include 拼写本身进入哈希，指纹变化不代表语义变化。前述逐文件核对另检查实现/测试包含顺序和公开接口。历史证据的路径清单与哈希保留原快照含义，仅可点击的源码链接改指当前位置。

本地副本、移动清单、核对结果、指纹和构建/测试日志在 `/tmp/clashflux-converter-structure`；永久验证继续复用已有生产入口、冻结样例和运行回归。

## 实际检查

Linux Clang 21/libc++ Debug、CMake 4.4.2，现有配置启用 CLASHFLUX_REQUIRE_PROJECT_TESTS。执行：

```bash
cmake --build build --target clash-flux test_singbox
cmake --build build
./run.sh --version
git diff --check
./build/test_singbox --protocol-baseline
./build/test_singbox --protocol-entries
./build/test_singbox --ss-uri
./build/test_singbox --v2ray-uri
./build/test_singbox --secure-uri
./build/test_singbox --node-inputs
ctest --test-dir build --output-on-failure --no-tests=error -L clashflux-required
```

主目标与全部测试目标构建通过，版本输出 v0.3.110，diff 检查通过。六个定向入口均退出 0：既有冻结/纯模型、跨入口、SS、VLESS/Trojan、HY2/TUIC/AnyTLS、历史 SS 和能力清单检查通过。

完整必跑门禁 **37/37 通过，239.15 秒**，覆盖转换、能力清单、固定内核实际转发/TLS 拒绝及应用既有回归。最终文档补齐后再次执行主目标构建、版本与 diff 检查。

从 platform/android 执行：

```bash
HUXERUI_HOME=/home/farna/.local/share/HuxerUI ./gradlew :app:externalNativeBuildDebug :app:compileDebugJavaWithJavac
```

NDK 29 arm64-v8a native/Java 构建 **55 秒通过**，79 项任务中 10 项执行、69 项为最新；Java 无改动，编译任务为最新，未重新打包 APK。构建依赖中的 RuntimeStartPolicy、RuntimeModeReconciler 和 VpnUptime 测试也通过。

README 中英文入口、协议/节点计划、保真度文档和相关项目技能已同步；本轮入口及协议证据的本地文档链接有效。AGENTS.md 为 67 行、3,133 字符，全部项目 SKILL.md 满足 100 行上限。

本轮只整理结构和方向，未重跑 P6 的大规模随机差分或性能实验，未声称性能提升。独立库/外部消费、Windows/macOS、Android 真机/VPN、GUI、签名/发布未验收；未提交、推送或发布。
