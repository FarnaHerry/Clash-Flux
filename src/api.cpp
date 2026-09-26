// api.cpp — clashflux.api 实现单元（curl）。
//
// 每次调用新建 easy handle（API 调用频率低，省去连接复用的复杂度换线程安全）。
// 全程 CURLOPT_NOSIGNAL（多线程必须）；超时覆盖 DNS+连接+传输。
module;

#include <curl/curl.h>

module clashflux.api;

import std;
import nlohmann.json;
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

size_t onFileWrite(char* ptr, size_t size, size_t nmemb, void* userdata) noexcept {
    try {
        const size_t n = size * nmemb;
        auto* out = static_cast<std::ofstream*>(userdata);
        out->write(ptr, static_cast<std::streamsize>(n));
        return out->good() ? n : CURL_WRITEFUNC_ERROR;
    } catch (...) {
        return CURL_WRITEFUNC_ERROR;
    }
}

// 订阅响应头收集（名字统一小写；重定向多跳时后值覆盖前值 = 最后一跳生效）。
size_t onHeaderLine(char* ptr, size_t size, size_t nmemb, void* userdata) noexcept {
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    const std::string line(ptr, size * nmemb);
    const auto colon = line.find(':');
    if (colon == std::string::npos) return size * nmemb;
    std::string name = line.substr(0, colon);
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    std::string value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\r' || value.front() == '\n')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    (*headers)[std::move(name)] = std::move(value);
    return size * nmemb;
}

// 从错误响应体提取 message 字段（{"message": "..."}）。
std::string extractMessage(const std::string& body) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_object()) return j.value("message", "");
    return "";
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
        const CURLcode code = curl_easy_perform(easy);
        if (code == CURLE_OK) {
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &result.status);
            result.ok = result.status >= 200 && result.status < 300;
            if (!result.ok) {
                result.error = extractMessage(result.body);
                if (result.error.empty()) {
                    result.error = std::format("HTTP {}", result.status);
                }
            }
        } else {
            result.error = curl_easy_strerror(code);
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
    const nlohmann::json body = {{"name", name}};
    return impl_->request("PUT", "/proxies/" + percentEncode(group), body.dump());
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

ApiResult ClashApi::downloadToFile(const std::string& url,
                                   const std::filesystem::path& dest,
                                   const DownloadOptions& options) {
    ApiResult result;
    // 先写同目录临时文件，成功后才原子替换目标：旧实现直接 truncate 目标，
    // 刷新失败（订阅/规则集）会把上一份可用文件删掉。失败时只清理临时文件，
    // 已有内容原样保留。
    const std::filesystem::path temp = dest.string() + ".part";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.error = "无法写入: " + dest.string();
        return result;
    }
    CurlHandle handle;
    if (!handle) {
        result.error = "curl_easy_init failed";
        out.close();
        std::error_code removeError;
        std::filesystem::remove(temp, removeError);
        return result;
    }
    CURL* const easy = handle.get();
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &onFileWrite);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &out);
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
    std::error_code ec;
    if (result.ok) {
        out.close();
        std::filesystem::rename(temp, dest, ec);
        if (ec) {
            result.ok = false;
            result.error = "无法替换目标文件: " + dest.string();
        }
    }
    if (!result.ok) {
        std::error_code removeError;
        std::filesystem::remove(temp, removeError);
    }
    return result;
}

} // namespace api
