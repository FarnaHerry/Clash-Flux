// api.cpp — clashflux.api 实现单元（curl）。
//
// 每次调用新建 easy handle（API 调用频率低，省去连接复用的复杂度换线程安全）。
// 全程 CURLOPT_NOSIGNAL（多线程必须）；超时覆盖 DNS+连接+传输。
module;

#include <curl/curl.h>
#include "wire_codec.h"
#include "http_request_headers.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(CLASHFLUX_IOS)
#include <cstddef>
extern "C" bool clashflux_ios_download_to_file(
    const char* url, const char* temp_path, long timeout_seconds,
    bool allow_invalid_certificate, const char* proxy_url,
    bool allow_system_proxy, long* http_status, long long* expected_bytes,
    char* response_headers_json, std::size_t response_headers_capacity,
    char* error_buffer, std::size_t error_buffer_capacity) noexcept;
#endif

module clashflux.api;

import std;
import clashflux.utils;

namespace api {
namespace {

size_t onBodyWrite(char* ptr, size_t size, size_t nmemb, void* userdata) noexcept {
    try {
        const size_t n = size * nmemb;
        auto* body = static_cast<std::string*>(userdata);
        body->append(ptr, n);
        return n;
    } catch (...) {
        return CURL_WRITEFUNC_ERROR;
    }
}

struct DownloadSink {
    std::ofstream& out;
    std::size_t maxBytes = 0, received = 0;
    bool limitExceeded = false;
};

size_t onFileWrite(char* ptr, size_t size, size_t nmemb, void* userdata) noexcept {
    try {
        if (size && nmemb > std::numeric_limits<std::size_t>::max() / size) return CURL_WRITEFUNC_ERROR;
        const size_t n = size * nmemb;
        auto& sink = *static_cast<DownloadSink*>(userdata);
        if (sink.maxBytes && n > sink.maxBytes - sink.received) {
            sink.limitExceeded = true; return CURL_WRITEFUNC_ERROR;
        }
        sink.out.write(ptr, static_cast<std::streamsize>(n));
        sink.received += n;
        return sink.out.good() ? n : CURL_WRITEFUNC_ERROR;
    } catch (...) {
        return CURL_WRITEFUNC_ERROR;
    }
}

// 每一跳响应单独收集，重定向跳转的订阅元数据不能泄漏到最终响应。
size_t onHeaderLine(char* ptr, size_t size, size_t nmemb, void* userdata) noexcept {
    try {
        auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
        const std::string_view line(ptr, size * nmemb);
        if (line.starts_with("HTTP/")) {
            headers->clear();
            return line.size();
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) return line.size();
        std::string name(line.substr(0, colon));
        for (char& c : name) {
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        }
        auto value = line.substr(colon + 1);
        const auto whitespace = [](char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        };
        while (!value.empty() && whitespace(value.front())) value.remove_prefix(1);
        while (!value.empty() && whitespace(value.back())) value.remove_suffix(1);
        (*headers)[std::move(name)] = std::string(value);
        return line.size();
    } catch (...) {
        return CURL_WRITEFUNC_ERROR;
    }
}

// 原子创建同目录独占临时目录，避免同一目标的并发请求共用 .part。
// 必须先声明本对象、后声明输出流，让析构顺序先关文件再清理（Windows）。
class DownloadTemp {
public:
    explicit DownloadTemp(const std::filesystem::path& dest) {
        auto parent = dest.parent_path();
        if (parent.empty()) parent = ".";
        std::random_device random;
        for (int attempt = 0; attempt < 8; ++attempt) {
            auto candidate = parent / (".clash-flux-download-" +
                std::to_string(random()) + "-" + std::to_string(random()));
            auto payload = candidate / "payload";
            std::error_code ec;
            if (std::filesystem::create_directory(candidate, ec)) {
                directory_ = std::move(candidate);
                payload_ = std::move(payload);
                return;
            }
            if (ec && ec != std::errc::file_exists) return;
        }
    }
    ~DownloadTemp() {
        if (directory_.empty()) return;
        std::error_code ec;
        std::filesystem::remove(payload_, ec);
        std::filesystem::remove(directory_, ec);
    }
    DownloadTemp(const DownloadTemp&) = delete;
    DownloadTemp& operator=(const DownloadTemp&) = delete;
    explicit operator bool() const noexcept { return !directory_.empty(); }
    const std::filesystem::path& path() const noexcept { return payload_; }
private:
    std::filesystem::path directory_;
    std::filesystem::path payload_;
};

std::error_code replaceDownload(const std::filesystem::path& temp,
                                const std::filesystem::path& dest) {
#ifdef _WIN32
    // filesystem::rename 在 Windows 不覆盖已有目标；不能先删除旧文件。
    if (MoveFileExW(temp.c_str(), dest.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return {};
    return {static_cast<int>(GetLastError()), std::system_category()};
#else
    std::error_code ec;
    std::filesystem::rename(temp, dest, ec);
    return ec;
#endif
}

// 从错误响应体提取 message 字段（{"message": "..."}）。
std::string extractMessage(const std::string& body) {
    auto decoded = clashflux::wire::DecodeMessage(body);
    return decoded ? std::move(decoded.value) : std::string{};
}

// curl C API 的 RAII 包装：easy handle 与 header list 原本要在每条返回路径
// 手动 cleanup/free，中途只要抛一次 C++ 异常（std::format、bad_alloc）就会
// 泄漏句柄。析构里释放后，所有失败/异常路径都由编译器兜底。
class CurlHandle {
public:
    CurlHandle() : handle_(curl_easy_init()) {}
    ~CurlHandle() {
        if (handle_ != nullptr) curl_easy_cleanup(handle_);
    }
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
    [[nodiscard]] CURL* get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    CURL* handle_;
};

class CurlHeaderList {
public:
    CurlHeaderList() = default;
    ~CurlHeaderList() {
        if (list_ != nullptr) curl_slist_free_all(list_);
    }
    CurlHeaderList(const CurlHeaderList&) = delete;
    CurlHeaderList& operator=(const CurlHeaderList&) = delete;
    // curl_slist_append 失败时返回 nullptr 且不改动原链表，旧链继续有效。
    bool append(const char* header) {
        curl_slist* next = curl_slist_append(list_, header);
        if (next == nullptr) return false;
        list_ = next;
        return true;
    }
    [[nodiscard]] curl_slist* get() const noexcept { return list_; }

private:
    curl_slist* list_ = nullptr;
};

} // namespace

struct ClashApi::Impl {
    std::string base;    // http://127.0.0.1:29097
    std::string secret;

    // 通用请求。method 为空 = GET；body 非空按 application/json 发送。
    ApiResult request(const std::string& method, const std::string& path,
                      const std::string& body = {}, long timeoutSec = 10) {
        ApiResult result;
        CurlHandle handle;
        if (!handle) {
            result.error = "curl_easy_init failed";
            return result;
        }
        CURL* const easy = handle.get();
        const std::string url = base + path;
        CurlHeaderList headers;
        headers.append("Content-Type: application/json");
        if (!secret.empty()) {
            const std::string auth = "Authorization: Bearer " + secret;
            headers.append(auth.c_str());
        }
        curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http");
        // The Clash controller is always a local endpoint. Never let
        // http_proxy/https_proxy environment variables intercept it.
        curl_easy_setopt(easy, CURLOPT_NOPROXY, "*");
        if (!method.empty()) {
            curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method.c_str());
        }
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &onBodyWrite);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &result.body);
        curl_easy_setopt(easy, CURLOPT_TIMEOUT, timeoutSec);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_USERAGENT, "clash-flux/0.1");
        if (!body.empty()) {
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body.data());
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(body.size()));
        }
        // 错误必须能归因到具体调用：以前只回一句 "Couldn't connect to server"，
        // 调用方无法判断是 /proxies、/configs 还是 /version 挂了。
        const std::string verb = method.empty() ? std::string{"GET"} : method;
        const CURLcode code = curl_easy_perform(easy);
        if (code == CURLE_OK) {
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &result.status);
            result.ok = result.status >= 200 && result.status < 300;
            if (!result.ok) {
                const std::string message = extractMessage(result.body);
                // 用拼接而非 std::format：见 downloadToFile 里关于 libc++ 宽字符
                // 候选实例化的说明。
                result.error = verb + " " + path + " → HTTP " +
                               std::to_string(result.status) +
                               (message.empty() ? std::string{}
                                                : " · " + message);
            }
        } else {
            result.error = verb + " " + path + "：" +
                           curl_easy_strerror(code) + "（curl " +
                           std::to_string(static_cast<int>(code)) + "）";
        }
        return result;
    }
};

ClashApi::ClashApi(std::string baseUrl, std::string secret)
    : impl_(std::make_unique<Impl>()) {
    impl_->base = std::move(baseUrl);
    impl_->secret = std::move(secret);
}
ClashApi::~ClashApi() = default;

void ClashApi::setEndpoint(std::string baseUrl, std::string secret) {
    impl_->base = std::move(baseUrl);
    impl_->secret = std::move(secret);
}

const std::string& ClashApi::baseUrl() const { return impl_->base; }

ApiResult ClashApi::version() { return impl_->request("", "/version"); }
ApiResult ClashApi::configs() { return impl_->request("", "/configs"); }

ApiResult ClashApi::patchConfigs(const std::string& jsonBody) {
    return impl_->request("PATCH", "/configs", jsonBody);
}

ApiResult ClashApi::proxies() { return impl_->request("", "/proxies"); }

ApiResult ClashApi::selectProxy(const std::string& group, const std::string& name) {
    const auto body = clashflux::wire::EncodeSelection(name);
    if (!body) return {.error = body.error};
    return impl_->request("PUT", "/proxies/" + percentEncode(group), body.value);
}

ApiResult ClashApi::proxyDelay(const std::string& name, const std::string& testUrl,
                               int timeoutMs) {
    const std::string path = appendQuery("/proxies/" + percentEncode(name) + "/delay",
                                         {{"url", testUrl},
                                          {"timeout", std::to_string(timeoutMs)}});
    // 传输超时要比测速超时宽，否则慢节点的合法超时被 curl 先截断。
    return impl_->request("", path, {}, timeoutMs / 1000 + 5);
}

ApiResult ClashApi::connections() { return impl_->request("", "/connections"); }

ApiResult ClashApi::closeConnection(const std::string& id) {
    return impl_->request("DELETE", "/connections/" + percentEncode(id));
}

ApiResult ClashApi::closeAllConnections() {
    return impl_->request("DELETE", "/connections");
}

bool CommitFile(const std::filesystem::path& temp, const std::filesystem::path& dest,
                std::string& error) {
    const auto ec = replaceDownload(temp, dest);
    if (ec) { error = "无法替换目标文件: " + dest.string() + "：" + ec.message(); return false; }
    error.clear(); return true;
}

ApiResult ClashApi::downloadToFile(const std::string& url,
                                   const std::filesystem::path& dest,
                                   const DownloadOptions& options) {
    ApiResult result;
    if (!clashflux::http_request::ValidHeaders(options.headers)) {
        result.error = "下载请求头非法或不支持";
        return result;
    }
#if defined(CLASHFLUX_IOS)
    if (!options.headers.empty()) {
        result.error = "iOS 请求头下载尚未接入";
        return result;
    }
#endif
    DownloadTemp temporary(dest);
    if (!temporary) {
        result.error = "无法创建下载临时目录: " + dest.string();
        return result;
    }
    const auto temp = temporary.path();
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.error = "无法写入: " + dest.string();
        return result;
    }
#if defined(CLASHFLUX_IOS)
    // iOS 的 curl 构建不含 TLS。订阅和规则集下载改用系统 URLSession，
    // 默认走 Apple 信任链校验证书；只有用户显式开启危险选项时才跳过校验。
    out.close();
    std::vector<char> responseHeaders(64 * 1024, '\0');
    std::vector<char> errorBuffer(2048, '\0');
    long status = 0;
    long long expectedBytes = -1;
    const bool transferred = clashflux_ios_download_to_file(
        url.c_str(), temp.string().c_str(),
        options.timeoutSecs > 0 ? options.timeoutSecs : 60L,
        options.allowInvalidCert, options.proxyUrl.c_str(),
        options.allowProxyEnv, &status, &expectedBytes,
        responseHeaders.data(), responseHeaders.size(),
        errorBuffer.data(), errorBuffer.size());
    if (!transferred) {
        result.error = errorBuffer.front() == '\0'
                           ? "iOS URLSession 下载失败"
                           : std::string(errorBuffer.data());
    } else {
        result.status = status;
        result.ok = status >= 200 && status < 300;
        if (!result.ok) {
            result.error = "HTTP " + std::to_string(status);
        } else {
            auto headers = clashflux::wire::DecodeHeaders(responseHeaders.data());
            if (headers) result.headers = std::move(headers.value);
            if (expectedBytes > 0) {
                std::error_code sizeError;
                const auto received = std::filesystem::file_size(temp, sizeError);
                if (sizeError) {
                    result.ok = false;
                    result.error = "无法检查下载文件长度: " + sizeError.message();
                } else if (received < static_cast<std::uintmax_t>(expectedBytes)) {
                    result.ok = false;
                    result.error = "下载不完整（收到 " + std::to_string(received) +
                                   "/" + std::to_string(expectedBytes) + " 字节）";
                }
            }
        }
    }
#else
    CurlHeaderList requestHeaders;
    CurlHandle handle;
    if (!handle) {
        result.error = "curl_easy_init failed";
        return result;
    }
    CURL* const easy = handle.get();
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, options.headers.empty() ? 1L : 0L);
    for (const auto& [name, value] : options.headers) {
        const auto line = name + ": " + value;
        if (!requestHeaders.append(line.c_str())) {
            result.error = "无法分配下载请求头";
            return result;
        }
    }
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, requestHeaders.get());
    curl_easy_setopt(easy, CURLOPT_HEADEROPT, CURLHEADER_SEPARATE);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &onFileWrite);
    DownloadSink sink{out, options.maxBytes};
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &onHeaderLine);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &result.headers);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT,
                     options.timeoutSecs > 0 ? options.timeoutSecs : 60L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "clash-flux/0.1");
    // 失败原因（OpenSSL 文本，例如 "SSL certificate problem: unable to get
    // local issuer certificate"）比 curl_easy_strerror 的概括更有诊断价值。
    char errorDetail[CURL_ERROR_SIZE]{};
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, errorDetail);
#if defined(_WIN32) && defined(CURLSSLOPT_NATIVE_CA)
    // Windows 没有 /etc/ssl 这类系统 CA 路径，vendored curl/OpenSSL 也不自带
    // CA bundle：默认信任链为空，任何 https 订阅都会在握手阶段失败
    // （CURLE_PEER_FAILED_VERIFICATION）。改为使用系统「受信任的根证书颁发
    // 机构 + 中间证书颁发机构」存储，与系统浏览器/Windows 的信任结论一致。
    curl_easy_setopt(easy, CURLOPT_SSL_OPTIONS,
                     static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif
    if (options.allowInvalidCert) {
        // 「允许无效证书（危险）」：跳过对端校验（自签/过期订阅源兜底）。
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    // 代理三态：指定代理 > 环境变量代理 > 强制直连（清空代理，防 env 干扰）。
    if (!options.proxyUrl.empty()) {
        curl_easy_setopt(easy, CURLOPT_PROXY, options.proxyUrl.c_str());
    } else if (!options.allowProxyEnv) {
        curl_easy_setopt(easy, CURLOPT_PROXY, "");
    }
    // 订阅服务器 UA 嗅探常见：clash-meta 的 UA 通过率更高。
    const CURLcode code = curl_easy_perform(easy);
    out.flush();
    if (code == CURLE_OK) {
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &result.status);
        result.ok = result.status >= 200 && result.status < 300;
        if (!result.ok) {
            result.error = std::format("HTTP {}", result.status);
        } else {
            // 服务器声明了长度就核对实际写入字节数：中间设备/代理按
            // connection-close 提前收尾时 curl 会当作正常完成，落盘的却可能是
            // 截断文件（.srs 规则集被截断会让内核启动期 FATAL）。
            curl_off_t expected = -1;
            curl_off_t received = -1;
            curl_easy_getinfo(easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &expected);
            curl_easy_getinfo(easy, CURLINFO_SIZE_DOWNLOAD_T, &received);
            if (expected > 0 && received >= 0 && received < expected) {
                result.ok = false;
                result.error = "下载不完整（收到 " + std::to_string(received) +
                               "/" + std::to_string(expected) + " 字节）";
            }
        }
    } else {
        const std::string detail = errorDetail[0] != '\0'
                                       ? std::string(errorDetail)
                                       : std::string(curl_easy_strerror(code));
        // 刻意用普通拼接而不是 std::format：clang-21/libc++ 在为这些实参组合做
        // 重载解析时会实例化宽字符候选 basic_format_string<wchar_t, …>，而
        // formatter<std::string, wchar_t> 被 libc++ 显式禁用，直接硬错误编译失败。
        const std::string codeText = std::to_string(static_cast<int>(code));
        if (code == CURLE_PEER_FAILED_VERIFICATION ||
            code == CURLE_SSL_CACERT_BADFILE) {
            // 证书链不被信任：系统根证书已参与校验（Windows 走系统信任库），
            // 剩下的可能就是自签/私有 CA/过期证书，交给订阅级开关处理。
            result.error = "TLS 证书校验失败（curl " + codeText + "）：" + detail +
                           "；可在订阅编辑中开启「允许无效证书（危险）」后重试";
        } else {
            result.error = detail + "（curl " + codeText + "）";
        }
    }
    if (sink.limitExceeded) result.error = "下载超过大小上限";
#endif
    if (out.is_open()) {
        out.close();
        if (result.ok && out.fail()) {
            result.ok = false;
            result.error = "无法完成下载文件写入: " + dest.string();
        }
    }
    if (result.ok && options.maxBytes) {
        std::error_code sizeError;
        const auto size = std::filesystem::file_size(temp, sizeError);
        if (sizeError || size > options.maxBytes) {
            result.ok = false; result.error = "下载超过大小上限或无法核对长度";
        }
    }
    if (result.ok && options.validate) {
        try {
            result.error = options.validate(temp);
        } catch (const std::exception& error) {
            result.error = std::string("下载内容校验失败: ") + error.what();
        } catch (...) {
            result.error = "下载内容校验失败";
        }
        result.ok = result.error.empty();
    }
    if (result.ok) {
        const auto ec = replaceDownload(temp, dest);
        if (ec) {
            result.ok = false;
            result.error = "无法替换目标文件: " + dest.string() + "：" + ec.message();
        }
    }
    return result;
}

} // namespace api
