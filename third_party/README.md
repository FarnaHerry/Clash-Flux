# third_party — vendored 依赖

第三方库全部 vendor 进本仓库：tarball 提交在 `tarballs/`（configure 期校验
SHA256 后解包到 `build/vendor/`，源码树不入库），nlohmann::json 是 single header
直接提交在 `json/`。构建完全离线、可复现。清单与姊妹项目 apitab 锁定一致。

## 清单与来源

| 包 | 版本 | tarball | 来源 |
|----|------|---------|------|
| HuxerUI | 0.2.0 | `huxerui-sdk-0.2.0-linux-x86_64.tar.gz` | 由官方 0.2.0 SDK 安装前缀归档（shared 库 + headers + CMake 包 + hcg/hrc + 内置资源）。`HUXERUI_HOME` 可指向 0.2.0 SDK 安装目录或源码根目录；未设置时优先 `third_party/huxerui/` 源码，`CLASHFLUX_HUXERUI_FORCE_SDK=ON` 时使用 Linux 离线包。Linux 源码模式需 GTK ≥4.14、libepoxy ≥1.5、libsoup ≥3.0（Fedora：`gtk4-devel libepoxy-devel libsoup3-devel`）；macOS/Windows 必须通过 `HUXERUI_HOME` 提供 0.2.0 源码或 SDK。 |
| curl | 8.22.0 | `curl-8.22.0.tar.gz` | 上游 `curl/curl` release tarball。用于 mihomo external-controller REST API 与订阅下载。 |
| IXWebSocket | 12.0.1 | `ixwebsocket-12.0.1.tar.gz` | 上游 `machinezone/IXWebSocket` v12.0.1（client-only、无 TLS/无 zlib）。用于 mihomo `/logs` `/traffic` `/connections` `/memory` 推送流（ws:// 回环）。 |
| SQLiteCpp | 3.3.3 | `SQLiteCpp-3.3.3.tar.gz` | 上游 `SRombauts/SQLiteCpp` v3.3.3（内置 sqlite3 amalgamation）。订阅与应用设置持久化。 |
| OpenSSL | 3.5.1 | `openssl-3.5.1-linux-x86_64.tar.gz` | 静态预编译产物（libssl.a/libcrypto.a + include），仅 linux x86_64 兜底；其他平台用系统 OpenSSL。 |
| nlohmann::json | 3.12.0 | `json/nlohmann/json.hpp`（single header） | 上游 `nlohmann/json` v3.12.0 `single_include` |
| QR-Code-generator | 1.8.0 | `qrcodegen/qrcodegen.{hpp,cpp}`（源码直提，同 json 先例） | 上游 `nayuki/QR-Code-generator` v1.8.0（MIT）。订阅分享二维码。 |

## 更新某个依赖

1. 用新版本源码打 tarball（保持顶层目录名，或同步改 `third_party/CMakeLists.txt`
   里 `clashflux_extract` 的 `topdir` 参数）；
2. `sha256sum` 新值写回 `third_party/CMakeLists.txt`；
3. 跑一次 configure 验证解包与 SHA 校验。
