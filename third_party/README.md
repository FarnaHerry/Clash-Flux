# third_party — vendored 依赖

第三方 tarball 提交在 `tarballs/`，版本、来源与 SHA 的唯一构建声明位于
`cmake/dependencies/Lock.cmake`。配置时验证归档与直提源码（包括热缓存），
独立 producer 构建、安装六个 CPS 包，应用通过 `find_package` 消费。展开源码
仍在 `build/vendor/`；包缓存区分平台、编译器、flags 与配置。详见
[依赖管理](../docs/cps-dependencies.md)。
nlohmann::json 是 single header，直接提交在 `json/`。Glaze 等本地归档可离线解包；HuxerUI 插件与内核首次获取仍可能需要网络。
归档版本及来源以本项目清单为准。

应用当前的 HuxerUI fork 源码提交与检出命令见 [固定版本](../docs/huxerui-fork.md)；本机独立 checkout、CI 和 Android Java/native 共用该身份，不消费维护工作树。
当前源码基线为合入官方 `df065105`（Windows 彩色 emoji）、`9efef2d6`（全屏 topmost 修复）、`e90163a1`（CLI DEB/RPM 打包）并修复受控 Pager 拖动禁用的 `11849f99`；下表 0.4.0 是离线 SDK 归档身份，不是源码版本。官方 0.4.0 归档提供 huxerui CLI、hcg/hrc 宿主工具与官方运行时库，但不含 fork 专属的 `create_initial_window` API，不满足桌面源码构建要求（CMake 配置期守卫会拒绝）。`HUXERUI_HOME` 也可显式选择固定 fork 源码。

## 清单与来源

| 包 | 版本 | tarball | 来源 |
|----|------|---------|------|
| HuxerUI | 0.4.0 | `huxerui-sdk-0.4.0-linux-x86_64.tar.gz` | 由官方 0.4.0 SDK 安装前缀归档（huxerui CLI + hcg/hrc 宿主工具 + headers + CMake 包 + 官方 shared/静态库与 Android/Web 平台静态库）。Android CI 复用其中 CLI 并以 `--sdk` 构建固定 fork 源码；归档不含 fork 专属 `create_initial_window` API，桌面构建需 fork 源码（`HUXERUI_HOME` 可指向 SDK 安装目录或上方固定的源码根目录；未设置时优先 `third_party/huxerui/` 源码，`CLASHFLUX_HUXERUI_FORCE_SDK=ON` 时使用 Linux 离线包，配置期守卫校验 fork API）。Linux 源码模式需 GTK ≥4.14、libepoxy ≥1.5、libsoup ≥3.0（Fedora：`gtk4-devel libepoxy-devel libsoup3-devel`）；macOS/Windows 不提供离线 SDK 包，使用固定源码或显式提供已安装 SDK。 |
| curl | 8.23.0 | `curl-8.23.0.tar.gz` | 上游 `curl/curl` release tarball。用于 sing-box clash_api external-controller REST API 与订阅下载。 |
| IXWebSocket | 12.1.0 | `ixwebsocket-12.1.0.tar.gz` | 上游 `machinezone/IXWebSocket` v12.1.0（客户端及本机 PAC HTTP 服务端、无 TLS/无 zlib）。用于 sing-box clash_api `/logs` `/traffic` `/connections` `/memory` 推送流（ws:// 回环）。 |
| SQLite（ORM） | Lib-SQLite fork | 构建期获取，不入 tarball | `FarnaHerry/Lib-SQLite` 的 `farna/main`，固定 `882665a9772cc973b1cbfb7f491913703da1d636`，自带固定 SQLite engine；包含启动只读查询、逐值 UTF-8 校验和主键 schema 校验（仅单列 INTEGER rowid 别名可免显式 NOT NULL）。订阅与设置持久化回归必跑，日志仍写 `core/*.log`。 |
| OpenSSL | 3.5.1 | `openssl-3.5.1-linux-x86_64.tar.gz` | 静态预编译产物（libssl.a/libcrypto.a + include），仅 linux x86_64 兜底；其他平台用系统 OpenSSL。 |
| 图表（Charts） | Lib-Charts fork | 构建期获取，不入 tarball | `FarnaHerry/Lib-Charts` 的 `farna/main`，固定 `2251bab5ad2a5df8e6e133c039e0b76bbe237add`，包含圆角饼图、全零数据等分且中心 0%、180ms 悬停动画与 96pt 紧凑绘图面，用于首页流量与饼图。 |
| 相机（Camera） | Lib-Camera fork | 构建期获取，不入 tarball | `FarnaHerry/Lib-Camera` 的 `farna/main`，固定 `5d6b8e814e3e5d8c22e63a4e470c2b6c1cd82cd0`，包含 ApplicationContext/UiWindow 兼容、库图模式早退、Linux V4L2 预览/拍照、X11 与 ScreenCast/PipeWire 屏幕选区，以及 Android/iOS 窗口级模块工厂与 `AwaitCallback` 拍照等待。 |
| 媒体选择（Media） | Lib-Media 上游 | 构建期获取，不入 tarball | 官方 `HuxerUI/Lib-Media`，固定 `fbe5b7e652f8cb79191444fa2a51d0dd0c2299bb`，未打补丁。仅 Android 扫码页使用图片选择器；原生目标和 Android 库图共用此提交，桌面应用不链接 Media。当前上游整体构建，Android 引入 Media3 ExoPlayer 1.9.3，不能因未调用播放器就假定其依赖被移除；体积实测见[二维码扫描](../docs/qr-scanning.md)。 |
| 文本编辑（CodeEditor） | Lib-CodeEditor fork | 构建期获取，不入 tarball | `FarnaHerry/Lib-CodeEditor` 固定 `a332e7c3158818a891bb8181756d439a62df6268`，SweetEditor `bd4330e70eda5c71b71ecac54cf7312d82d2eae2`。预构建文档、视口编辑、只读复制、滚动和 Windows 公共头兼容；Linux 用固定 simdutf v7.7.0 源码提交 `94fb52ea0219cb37092d8ae294b4702bacf5d8df` 按消费者工具链构建，并以 build-only include 复用 SweetEditor 同版本兼容头，避免预编译归档与 libc++ 混用；其它平台使用同版本预编译包。SweetLine 仅由库的子模块获取，不链接到应用。 |
| Zstandard | 1.5.7 | `zstd-1.5.7.tar.gz` | Official facebook/zstd v1.5.7 tag archive (`https://codeload.github.com/facebook/zstd/tar.gz/refs/tags/v1.5.7`), SHA256 `37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3`. Static MRS decoder dependency on all platforms, no CLI/test/shared build. Upstream BSD/GPL license files remain in the archive. |
| Glaze | 9.1.0 | `glaze-9.1.0-headers.tar.gz` | 官方 tag revision `43b175e7446862b84ed07b2e5f5138433777680e` 的 `include/` + MIT `LICENSE`。SHA256 `9bb4c9897e8b0f0d6b66b3ee4c1519efbf4288a4c331fd382d8c9412e67a244a`。私有 `clashflux_wire` 静态库实现应用 JSON codec，不运行上游 CMake。 |
| nlohmann::json | 3.12.0 | `json/nlohmann/json.hpp`（single header） | 上游 `nlohmann/json` v3.12.0 `single_include` |
| Lib-QRCode | `23bc79b19a6a6d104efc00bfa3934cd7ab78b08f` | 构建期通过 `huxerui_use_library` 获取官方 `HuxerUI/Lib-QRCode` | 共享二维码的生成、绘制和 Android 识别。内置 ZXing-C++ 2.3.0。许可见库内 `LICENSE` 与 `THIRD_PARTY_NOTICES.md`。 |
| yaml-cpp | 0.9.0 | `yaml-cpp-0.9.0.tar.gz` | 上游 `jbeder/yaml-cpp` yaml-cpp-0.9.0（MIT）。Clash 订阅 YAML 解析（sing-box 配置编译器输入）。 |

Camera、Charts、SQLite、CodeEditor、Media 通过 `huxerui_use_library(URL, COMMIT)` 获取到构建缓存，原生构建
与 Android 库图同源；不自动采用旧 `third_party/lib-*` 工作树，也不在配置期打补丁。
维护目录、审核清单与升级流程见 [HuxerUI fork](../docs/huxerui-fork.md)。

## 更新某个依赖

1. 更新归档或直提源码，并核对来源、许可证与实际 SHA256；
2. 更新 `cmake/dependencies/Lock.cmake` 中的版本、文件名和 SHA（不使用浮动分支）；
3. 更新独立 producer 的构建选项或 CPS 导出接口（如果上游 API 改变）；
4. 重新编译应用并运行必跑回归，包含 `cps_dependencies`、下载 TLS 与持久化。

HuxerUI 与其扩展仍遵守独立 fork 维护、固定提交和 Java/native 同源规则。

## 2026-11-13 官方 Charts main 试编译（未切换）

官方 `HuxerUI/Lib-Charts` main 为 `13e5ffddc7f24ed7f33533f873249558466e9591`，
饼图实现和公开头文件已与当前 fork 一致；通用图表仍使用旧 PopupHandle 锚点 API，
在当时固定的 HUI `4b444b19` 下 HCG 后编译失败，且未包含 96pt 紧凑绘图区适配。
应用继续固定 `FarnaHerry/Lib-Charts@2251bab5ad2a5df8e6e133c039e0b76bbe237add`，
未修改 URL/COMMIT、库源码或平台库图。详见[fork 核对记录](../docs/huxerui-fork.md)。

## 2026-11-13 HuxerUI SDK 归档更新至 0.4.0（源码 pin eb190f3d）

离线 SDK 归档由 0.3.0 升级到官方 0.4.0（`huxerui-sdk-0.4.0-linux-x86_64.tar.gz`，
SHA256 `86837f3e872fe6716464d1804c10d894957742a6616a8d2977ea93e630799583`，
Lock.cmake 与 CI 一致校验），0.3.0 归档移除（历史仍可从 git 取回）。
应用源码与 CI 固定推进到 `eb190f3d7e20f29614042aec7bc59fe5143f29f7`：farna/main
合入官方 0.4.0 发布基线（`85d65da5`），CLI 新增已安装 SDK 版本管理（`2f079bae`）。
Android CI 改用归档内 0.4.0 CLI 并以 `--sdk huxerui-source` 与 Java/native 同源构建，
不再走旧 CLI 的 `--source`。CMake 新增 `create_initial_window` fork API 守卫：
官方归档 `app.h` 不含该 API（实测），fork 源码 `app.h` 含（`app.h:376`），配置期据此拒绝误用官方包。
本轮实测归档 SHA256、本机 checkout 与 CI pin 一致；按用户指示未重新编译主目标与必跑回归，
完成度边界见 [验收记录](../docs/l1-l2-l3-status.md)。

## 2026-11-13 HuxerUI 更新（ecf1646f）

应用源码与 CI 固定 `ecf1646feb38a082962a9ad4eca8a7078422d48f`，跟进已发布的 farna/main，
相对 4b444b19 前进 6 个提交，包含官方 df5f4ba0 对 FillViewport 与禁用容器控制器滚动的
吸收，以及禁用虚拟容器 ScrollToItem 对齐修复。源码 CLI 使用 --sdk，mcpp 使用
--project，并新增 b/r/pkg 别名。CI 固定旧离线 CLI 保留其 --source 参数；Java/native
均从同一新源码编译。HCG/HRC 源码、扩展、通用依赖和 sing-box/libbox 固定版本保持。
Linux 全量构建、32 项必跑回归和 Android Java/native 编译通过；详见
[fork 记录](../docs/huxerui-fork.md) 与 [验收记录](../docs/l1-l2-l3-status.md)。

## 2026-11-13 HuxerUI 更新（4b444b19）

应用源码与 CI 固定 `4b444b19da507efece20203372936439a74c4f3b`，跟进已发布的 farna/main，
包含官方 `babf9f2a` 的全屏状态与窗口快捷键、Apple 文本居中、Web 换行以及 Windows
安装器升级/重启处理；fork 另修复 Windows 全屏任务栏覆盖。定制安装器同步新重启提示的
说明和四份语言目录按钮。扩展、通用依赖和 sing-box/libbox 固定版本不变。
详见 [fork 记录](../docs/huxerui-fork.md) 与 [验收记录](../docs/l1-l2-l3-status.md)。

## 2026-11-13 HuxerUI 更新（e720ae5f）

应用源码与 CI 固定 `e720ae5f48690e43a5899127ace1b61645c120f4`，在 `cd66ef31` 上修复
macOS `AppKitPlatformViews` 构造参数与 `unique_ptr<AppKitRenderer>` 不匹配的合并错误。
核心 API、扩展 pin 与其它平台路径不变。详见 [fork 记录](../docs/huxerui-fork.md) 和
[验收记录](../docs/l1-l2-l3-status.md)。

## 2026-11-12 HuxerUI 更新（cd66ef31）

应用源码与 CI 固定 `cd66ef310bedbfa1916e535a34f1d608930fe7a8`，
合入官方 `ad28dd34` 的平台段落布局测量/绘制复用（Android StaticLayout、AppKit/UIKit
CTFrame、Windows DirectWrite、Web 行度量）；核心 `src/include` 与公开 API 不变，
扩展 pin 不变，Linux Pango 路径未改。各平台验证边界见
[fork 记录](../docs/huxerui-fork.md) 和 [验收记录](../docs/l1-l2-l3-status.md)。

## 2026-11-12 HuxerUI 更新（12231999）

应用源码与 CI 固定 `12231999142bd9c39626d033c6a9ae192291d105`，
包含官方 `00d1a486` 的文本布局/绘制复用、字素安全省略与传递依赖支持，
以及 Android 离屏文本绘制编译修复、Linux 自定义窗口 6 DIP 窄带拉伸与角落命中修复。
扩展与通用依赖版本不变，详见 [fork 记录](../docs/huxerui-fork.md) 和
[验收记录](../docs/l1-l2-l3-status.md)。

## 2026-11-11 稳定版本核对

通用库升级到 Glaze 9.1.0、curl 8.23.0、IXWebSocket 12.1.0、yaml-cpp 0.9.0。
来源分别为官方 tag `v9.1.0`、curl 官方 release tarball、`v12.1.0`、`yaml-cpp-0.9.0`；
Glaze 仅保留 include/ 与 LICENSE，归档元数据归零。所有构建输入摘要见 Lock.cmake。
Zstd 1.5.7、nlohmann/json 3.12.0 与 sing-box 1.14.4
核对官方最新稳定版本后保持不变，不采用 alpha/RC。

HuxerUI 更新到已发布 fork `2229125248d95fc47c334df1568533e56bf62350`，
Media 更新到官方 `fbe5b7e652f8cb79191444fa2a51d0dd0c2299bb`。
SQLite/Charts 已包含各自最新上游基线；Camera 与 CodeEditor 保留最新已发布 fork pin：
官方 Camera `7f6176c5` 新增帧处理/静默录制，CodeEditor `fb160968` 重构构建及命名，
尚未合入 fork。直接替换会绕过 Linux 摄像头、预构建文档、剪贴板与 simdutf 兼容修复，
须在独立维护仓库合并并验证后再升级。离线 HuxerUI SDK 0.3.0 仍是历史回落资产，
不满足当前 0.4 源码构建要求。系统 OpenSSL、Android Gradle/AGP 仍由现有工具链选择。
OpenSSL 最新稳定版为 4.1.0，但官方 release/codeload 下载多次 TLS 中断、超时或内容不完整；
未生成或替换兜底静态包，仍为 3.5.1。本轮 Linux 实际使用系统 OpenSSL 3.5.9，
不能据此视为旧兜底包已完成升级或验证。
实际验证与尚未完成边界见 [验收记录](../docs/l1-l2-l3-status.md)。
