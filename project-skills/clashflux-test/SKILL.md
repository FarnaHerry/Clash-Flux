---
name: clashflux-test
description: 构建、验证或修改 Clash-Flux 测试及 CI 门禁时使用；包括文档改动后的基础验证、DNS/TLS、平台和依赖回归。
---

# 构建与测试

命令均从仓库根执行。先检查现有构建目录、工具链和 CMakeCache，不重建用户的配置。
CMake 最低 4.4，本机/CI 基线 4.4.2；优先 Ninja 增量构建，UI 改动必须经过 HuxerUI codegen。

## 每次改动后的必做检查

```bash
cmake --build build --target clash-flux
./run.sh --version
git diff --check
```

Windows 用 `./run.ps1 --version`。MSVC 需在已加载开发环境的 PowerShell 中构建；不要因缺失 INCLUDE/LIB 改工程配置。
Windows 版本命令需要控制台时使用终端执行，确认实际版本输出与退出码。
脚本不会隐式编译。`run.ps1` 从 build/CMakeCache.txt 的 CMAKE_RUNTIME_OUTPUT_DIRECTORY 找二进制，未配置则回落 build/clash-flux.exe。
其它构建目录必须显式指定 `CLASHFLUX_BIN`：Linux 为 `CLASHFLUX_BIN=/绝对路径/clash-flux ./run.sh --version`；PowerShell 设置 `$env:CLASHFLUX_BIN` 后运行脚本。
需要 GUI 验证时再运行 `./run.sh` / `./run.ps1`；首帧图像或主题路径改动必须实际启动 GUI。
任何文件修改都执行基础检查，构建或版本验证失败不得宣称完成；报告实际执行的命令、结果和未覆盖范围。

## 桌面完整回归与 CI

```bash
cmake -S . -B build -DCLASHFLUX_REQUIRE_PROJECT_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure --no-tests=error -L clashflux-required
```

- 必跑名单以 [ProjectTestGate.cmake](../../cmake/ProjectTestGate.cmake) 为准；Windows 还包括不提权的 legacy_bundle_cleanup 隔离注册表/缓存回归。只有记录在案的 MSVC C4737 可跳过直接 ORM 测试，persistence 必跑；手机不启用该桌面门禁。
- 缺失/禁用/未注册不是通过。Python、固定打包内核和可执行 OpenSSL CLI 缺失必须让门禁失败。下载回归传明确 OpenSSL 路径及 `--require-tls`，不能省略证书拒绝场景。
- Windows CI 用 runner 的 Program Files/OpenSSL，构建前检查 CLI 与开发头文件，不重新通过 Chocolatey 安装。服务和 WFP 回归需要管理员测试环境。
- 本地针对改动选择相关回归；改测试注册、CI 或发布门禁时检查完整注册与禁用情况。文档改动不冒充完整平台验收。

## 按改动选择回归

| 改动 | 必须查阅/执行的验证 |
| --- | --- |
| DNS / TLS | 先构建 test_singbox，再执行下方 DNS 命令。TLS 依赖 OpenSSL；证书拒绝同时断言 TLS certificate alert 和没有 DNS payload，不能用超时替代；临时根证书只进独占测试配置，不改生产或系统信任库。 |
| 订阅 / 数据库 / flush | 读取 [数据技能](../clashflux-data/SKILL.md) 的真实锁竞争、多来源、多 ID、文件保留与重开要求，运行 persistence。 |
| UI / 分页 / 导入 | 读取 [UI 技能](../clashflux-ui/SKILL.md)，运行生产组件的 page_transition / ui_resources 及所需 GUI 验证。 |
| 依赖 / fork / 打包边界 | 读取 [依赖技能](../clashflux-dependencies/SKILL.md)，执行 CPS 重定位、版本拒绝、缓存完整性、TLS 及对应框架行为回归。 |
| 服务 / PPTP / 系统网络 | 读取 [运行控制技能](../clashflux-runtime/SKILL.md) 和对应 [平台约束](../clashflux-platform/SKILL.md)，依平台执行 service_protocol、vpn、routing、compensation 和 Windows 专属回归。 |

DNS 定向回归：

```bash
cmake --build build --target test_singbox
ctest --test-dir build --output-on-failure --no-tests=error -R '^(dns_hosts_runtime|dns_policy_runtime|dns_tls_runtime)$'
```

验收证据写入 [L1/L2/L3 状态](../../docs/l1-l2-l3-status.md)：日期、代码快照、真实检查及未完成边界。构建、配置检查、无窗口回归不能写成真机 VPN 或发布通过。
