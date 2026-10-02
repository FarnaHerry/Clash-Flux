#include "profile_link.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <utility>

namespace clashflux::profile_link {
namespace {
constexpr std::size_t kMaxLinkBytes = 65536;
std::mutex queueMutex;
std::vector<RemoteProfile> pending;
std::function<void()> wakeHandler;

std::string Lower(std::string_view value) {
    std::string result(value);
    for (char& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}

bool Decode(std::string_view value, std::string& output, bool form) {
    const auto hex = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    output.clear();
    for (std::size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == '%') {
            if (i + 2 >= value.size()) return false;
            const int a = hex(value[i + 1]), b = hex(value[i + 2]);
            if (a < 0 || b < 0) return false;
            c = static_cast<char>(a * 16 + b);
            i += 2;
        } else if (c == '+' && form) c = ' ';
        if (static_cast<unsigned char>(c) < 32 || c == 127) return false;
        output += c;
    }
    return true;
}
} // namespace

bool IsSupportedScheme(std::string_view scheme) {
    const auto value = Lower(scheme);
    return value == "sing-box" || value == "flclash" || value == "clash" ||
           value == "clashmeta" || value == "clash-flux";
}

bool IsHttpUrl(std::string_view value) {
    const auto separator = value.find("://");
    if (separator == std::string_view::npos) return false;
    const auto scheme = Lower(value.substr(0, separator));
    if (scheme != "http" && scheme != "https") return false;
    const auto rest = value.substr(separator + 3);
    const auto authority = rest.substr(0, rest.find_first_of("/?#"));
    if (authority.empty() || authority == ":" || authority.ends_with('@')) return false;
    for (unsigned char c : value) if (c <= 32 || c == 127 || c == '\\') return false;
    std::string ignored;
    return Decode(value, ignored, false);
}

std::optional<RemoteProfile> ParseParts(std::string_view scheme,
    std::string_view authority, std::string_view path, std::string_view query,
    std::string_view fragment, std::string& error) {
    error.clear();
    const auto fail = [&](const char* message) -> std::optional<RemoteProfile> {
        error = message;
        return std::nullopt;
    };
    if (scheme.size() + authority.size() + path.size() + query.size() + fragment.size() > kMaxLinkBytes)
        return fail("订阅唤醒链接过长");
    if (!IsSupportedScheme(scheme)) return fail("不支持的订阅唤醒协议");
    const auto command = Lower(authority);
    if (command != (Lower(scheme) == "sing-box" ? "import-remote-profile" : "install-config") ||
        (!path.empty() && path != "/")) return fail("不支持的订阅唤醒操作");
    RemoteProfile profile;
    bool foundUrl = false, foundName = false;
    while (!query.empty()) {
        const auto ampersand = query.find('&');
        const auto item = query.substr(0, ampersand);
        const auto equals = item.find('=');
        std::string key, value;
        if (!Decode(item.substr(0, equals), key, true) ||
            !Decode(equals == std::string_view::npos ? std::string_view{} : item.substr(equals + 1), value, true))
            return fail("订阅唤醒链接的百分号编码无效");
        if (key == "url") {
            if (foundUrl) return fail("订阅唤醒链接包含重复的 url 参数");
            foundUrl = true;
            profile.url = std::move(value);
        } else if (key == "name") {
            if (foundName) return fail("订阅唤醒链接包含重复的 name 参数");
            foundName = true;
            profile.name = std::move(value);
        }
        query = ampersand == std::string_view::npos ? std::string_view{} : query.substr(ampersand + 1);
    }
    if (!foundUrl || !IsHttpUrl(profile.url)) return fail("订阅唤醒链接需要有效的 HTTP 或 HTTPS url 参数");
    if (!fragment.empty() && !Decode(fragment, profile.name, false))
        return fail("订阅名称的百分号编码无效");
    return profile;
}

std::optional<RemoteProfile> Parse(std::string_view value, std::string& error) {
    const auto separator = value.find("://");
    if (separator == std::string_view::npos) { error = "不是有效的订阅唤醒链接"; return std::nullopt; }
    const auto scheme = value.substr(0, separator);
    auto rest = value.substr(separator + 3);
    const auto hash = rest.find('#');
    const auto fragment = hash == std::string_view::npos ? std::string_view{} : rest.substr(hash + 1);
    rest = rest.substr(0, hash);
    const auto question = rest.find('?');
    const auto query = question == std::string_view::npos ? std::string_view{} : rest.substr(question + 1);
    rest = rest.substr(0, question);
    const auto slash = rest.find('/');
    return ParseParts(scheme, rest.substr(0, slash),
        slash == std::string_view::npos ? std::string_view{} : rest.substr(slash), query, fragment, error);
}

bool Submit(RemoteProfile profile, std::string& error) {
    error.clear();
    std::function<void()> wake;
    {
        std::lock_guard lock(queueMutex);
        // Some shells expose the same cold-start URL in argv and activation.
        if (!pending.empty() && pending.back() == profile) return true;
        if (pending.size() >= 16) { error = "待处理的订阅链接过多，请先完成导入"; return false; }
        pending.push_back(std::move(profile));
        wake = wakeHandler;
    }
    if (wake) wake();
    return true;
}

std::vector<RemoteProfile> TakePending() {
    std::lock_guard lock(queueMutex);
    return std::exchange(pending, {});
}

void SetWakeHandler(std::function<void()> handler) {
    std::function<void()> wake;
    {
        std::lock_guard lock(queueMutex);
        wakeHandler = std::move(handler);
        if (!pending.empty()) wake = wakeHandler;
    }
    if (wake) wake();
}
} // namespace clashflux::profile_link
