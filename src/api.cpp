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

// 从 mihomo 的错误响应体提取 message 字段（{"message": "..."}）。
std::string extractMessage(const std::string& body) {
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_object()) return j.value("message", "");
    return "";
}

} // namespace

struct ClashApi::Impl {
    std::string base;    // http://127.0.0.1:9097
    std::string secret;

    // 通用请求。method 为空 = GET；body 非空按 application/json 发送。
    ApiResult request(const std::string& method, const std::string& path,
                      const std::string& body = {}, long timeoutSec = 10) {
        ApiResult result;
        CURL* easy = curl_easy_init();
        if (!easy) {
            result.error = "curl_easy_init failed";
            return result;
        }
        const std::string url = base + path;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (!secret.empty()) {
            const std::string auth = "Authorization: Bearer " + secret;
            headers = curl_slist_append(headers, auth.c_str());
        }
        curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http");
        if (!method.empty()) {
            curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method.c_str());
        }
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
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
        curl_slist_free_all(headers);
        curl_easy_cleanup(easy);
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

ApiResult ClashApi::reloadConfig(const std::string& path) {
    const nlohmann::json body = {{"path", path}};
    return impl_->request("PUT", "/configs?force=true", body.dump(), 20);
}

ApiResult ClashApi::restart() { return impl_->request("POST", "/restart", "{}", 20); }

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

ApiResult ClashApi::groupDelay(const std::string& group, const std::string& testUrl,
                               int timeoutMs) {
    const std::string path = appendQuery("/group/" + percentEncode(group) + "/delay",
                                         {{"url", testUrl},
                                          {"timeout", std::to_string(timeoutMs)}});
    return impl_->request("", path, {}, timeoutMs / 1000 + 15);
}

ApiResult ClashApi::rules() { return impl_->request("", "/rules"); }
ApiResult ClashApi::connections() { return impl_->request("", "/connections"); }

ApiResult ClashApi::closeConnection(const std::string& id) {
    return impl_->request("DELETE", "/connections/" + percentEncode(id));
}

ApiResult ClashApi::closeAllConnections() {
    return impl_->request("DELETE", "/connections");
}

ApiResult ClashApi::proxyProviders() { return impl_->request("", "/providers/proxies"); }

ApiResult ClashApi::updateProxyProvider(const std::string& name) {
    return impl_->request("PUT", "/providers/proxies/" + percentEncode(name), "{}",
                          60);
}

ApiResult ClashApi::healthcheckProvider(const std::string& name) {
    return impl_->request("", "/providers/proxies/" + percentEncode(name) +
                          "/healthcheck", {}, 60);
}

ApiResult ClashApi::downloadToFile(const std::string& url,
                                   const std::filesystem::path& dest,
                                   const DownloadOptions& options) {
    ApiResult result;
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
        result.error = "无法写入: " + dest.string();
        return result;
    }
    CURL* easy = curl_easy_init();
    if (!easy) {
        result.error = "curl_easy_init failed";
        return result;
    }
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
        if (!result.ok) result.error = std::format("HTTP {}", result.status);
    } else {
        result.error = curl_easy_strerror(code);
    }
    curl_easy_cleanup(easy);
    if (!result.ok) {
        std::error_code ec;
        std::filesystem::remove(dest, ec);
    }
    return result;
}

} // namespace api
