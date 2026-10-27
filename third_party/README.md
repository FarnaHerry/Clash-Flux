# third_party — vendored 依赖

第三方 tarball 提交在 `tarballs/`，版本、来源与 SHA 的唯一构建声明位于
`cmake/dependencies/Lock.cmake`。配置时验证归档与直提源码（包括热缓存），
独立 producer 构建、安装七个 CPS 包，应用通过 `find_package` 消费。展开源码
仍在 `build/vendor/`；包缓存区分平台、编译器、flags 与配置。详见
[依赖管理](../docs/cps-dependencies.md)。
nlohmann::json 是 single header，直接提交在 `json/`。Glaze 等本地归档可离线解包；HuxerUI 插件与内核首次获取仍可能需要网络。
归档版本及来源以本项目清单为准。

## 清单与来源

| 包 | 版本 | tarball | 来源 |
|----|------|---------|------|
| HuxerUI | 0.3.0 | `huxerui-sdk-0.3.0-linux-x86_64.tar.gz` | 由官方 0.3.0 SDK 安装前缀归档（shared 库 + headers + CMake 包 + hcg/hrc + 内置资源）。`HUXERUI_HOME` 可指向 0.3.0 SDK 安装目录或源码根目录；未设置时优先 `third_party/huxerui/` 源码，`CLASHFLUX_HUXERUI_FORCE_SDK=ON` 时使用 Linux 离线包。Linux 源码模式需 GTK ≥4.14、libepoxy ≥1.5、libsoup ≥3.0（Fedora：`gtk4-devel libepoxy-devel libsoup3-devel`）；macOS/Windows 必须通过 `HUXERUI_HOME` 提供 0.3.0 源码或 SDK。 |
| curl | 8.22.0 | `curl-8.22.0.tar.gz` | 上游 `curl/curl` release tarball。用于 sing-box clash_api external-controller REST API 与订阅下载。 |
| IXWebSocket | 12.0.1 | `ixwebsocket-12.0.1.tar.gz` | 上游 `machinezone/IXWebSocket` v12.0.1（client-only、无 TLS/无 zlib）。用于 sing-box clash_api `/logs` `/traffic` `/connections` `/memory` 推送流（ws:// 回环）。 |
| SQLite（ORM） | Lib-SQLite fork | 构建期获取，不入 tarball | `FarnaHerry/Lib-SQLite` 的 `farna/main`，固定 `88f610fe66c57260b1ac38e676fc36b23c900fc2`，自带固定 SQLite engine；包含启动只读查询、逐值 UTF-8 校验和主键 schema 兼容。订阅与设置持久化回归必跑，日志仍写 `core/*.log`。 |
| OpenSSL | 3.5.1 | `openssl-3.5.1-linux-x86_64.tar.gz` | 静态预编译产物（libssl.a/libcrypto.a + include），仅 linux x86_64 兜底；其他平台用系统 OpenSSL。 |
| 图表（Charts） | Lib-Charts fork | 构建期获取，不入 tarball | `FarnaHerry/Lib-Charts` 的 `farna/main`，固定 `e90cfa84707bdf72eb6029410bf05c420efa62fc`，包含圆角饼图、全零数据等分且中心 0%、180ms 悬停动画与 96pt 紧凑绘图面，用于首页流量与饼图。 |
| 相机（Camera） | Lib-Camera fork | 构建期获取，不入 tarball | `FarnaHerry/Lib-Camera` 的 `farna/main`，固定 `b8b3b9304646cdc1afb9722ead16d85cf2160610`，包含 ApplicationContext/UiWindow 兼容与库图模式早退。 |
| 文本编辑（CodeEditor） | Lib-CodeEditor fork | 构建期获取，不入 tarball | `FarnaHerry/Lib-CodeEditor` 固定 `e3695be698c88738d228525cd46ce3a550162464`，SweetEditor `bd4330e70eda5c71b71ecac54cf7312d82d2eae2`。预构建文档、视口编辑、只读复制、滚动和 Windows 公共头兼容；Linux 用固定 simdutf v7.7.0 源码提交 `94fb52ea0219cb37092d8ae294b4702bacf5d8df` 按消费者工具链构建，避免预编译归档与 libc++ 混用；其它平台使用同版本预编译包。SweetLine 仅由库的子模块获取，不链接到应用。 |
| Zstandard | 1.5.7 | `zstd-1.5.7.tar.gz` | Official facebook/zstd v1.5.7 tag archive (`https://codeload.github.com/facebook/zstd/tar.gz/refs/tags/v1.5.7`), SHA256 `37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3`. Static MRS decoder dependency on all platforms, no CLI/test/shared build. Upstream BSD/GPL license files remain in the archive. |
| Glaze | 9.0.0 | `glaze-9.0.0-headers.tar.gz` | 官方 tag revision `d78832c82289c61a9315bfbc35332cec9f4e93ca` 的 `include/` + MIT `LICENSE`。SHA256 `872764412db2ba94cf57e39cf6518f86c0e13728c1c668f7ed398801aadff812`。私有 `clashflux_wire` 静态库实现应用 JSON codec，不运行上游 CMake。 |
| nlohmann::json | 3.12.0 | `json/nlohmann/json.hpp`（single header） | 上游 `nlohmann/json` v3.12.0 `single_include` |
| QR-Code-generator | 1.8.0 | `qrcodegen/qrcodegen.{hpp,cpp}`（源码直提，同 json 先例） | 上游 `nayuki/QR-Code-generator` v1.8.0（MIT）。订阅分享二维码。 |
| yaml-cpp | 0.8.0 | `yaml-cpp-0.8.0.tar.gz` | 上游 `jbeder/yaml-cpp` v0.8.0（MIT）。Clash 订阅 YAML 解析（sing-box 配置编译器输入）。 |

Camera、Charts、SQLite、CodeEditor 通过 `huxerui_use_library(URL, COMMIT)` 获取到构建缓存，原生构建
与 Android 库图同源；不自动采用旧 `third_party/lib-*` 工作树，也不在配置期打补丁。
维护目录、审核清单与升级流程见 [HuxerUI fork](../docs/huxerui-fork.md)。

## 更新某个依赖

1. 更新归档或直提源码，并核对来源、许可证与实际 SHA256；
2. 更新 `cmake/dependencies/Lock.cmake` 中的版本、文件名和 SHA（不使用浮动分支）；
3. 更新独立 producer 的构建选项或 CPS 导出接口（如果上游 API 改变）；
4. 重新编译应用并运行必跑回归，包含 `cps_dependencies`、下载 TLS 与持久化。

HuxerUI 与其扩展仍遵守独立 fork 维护、固定提交和 Java/native 同源规则。
