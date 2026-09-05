// utils.cppm — 纯 string/number 帮助函数（无 UI / 引擎依赖，store / core / UI 共用）。
module;

#include <time.h>  // localtime_r / localtime_s（C 函数不在 import std 里）

export module clashflux.utils;

import std;

// 去掉首尾空白（URL / 名称输入清洗）。
export std::string trim(std::string s) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::ranges::find_if(s, notSpace));
    s.erase(std::ranges::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

// 毫秒数格式化：1234.5 -> "1.23 s"，123.4 -> "123 ms"。
export std::string formatMs(double ms) {
    if (ms >= 1000.0) return std::format("{:.2f} s", ms / 1000.0);
    return std::format("{:.0f} ms", ms);
}

// 字节数格式化：2048 -> "2.0 KB"。
export std::string formatBytes(std::int64_t bytes) {
    if (bytes < 0) return "-";
    constexpr double k = 1024.0;
    if (bytes < k) return std::format("{} B", bytes);
    if (bytes < k * k) return std::format("{:.1f} KB", bytes / k);
    if (bytes < k * k * k) return std::format("{:.1f} MB", bytes / (k * k));
    return std::format("{:.2f} GB", bytes / (k * k * k));
}

// 速率格式化（字节/秒）：2048 -> "2.0 KB/s"。
export std::string formatRate(std::int64_t bytesPerSec) {
    return formatBytes(bytesPerSec) + "/s";
}

// Unix 秒 -> "MM-DD HH:MM"（本地时间）。
export std::string formatTime(std::int64_t unixSec) {
    const std::time_t t = static_cast<std::time_t>(unixSec);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return std::format("{:02d}-{:02d} {:02d}:{:02d}",
                       tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

// Unix 秒 -> "HH:MM:SS"（日志行时间戳）。
export std::string formatClock(std::int64_t unixSec) {
    const std::time_t t = static_cast<std::time_t>(unixSec);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return std::format("{:02d}:{:02d}:{:02d}", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

export std::int64_t nowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 判断开头是否为 RFC 3986 URI scheme（如 https:）。
export bool hasUriScheme(std::string_view value) {
    if (value.empty() || !((value.front() >= 'A' && value.front() <= 'Z') ||
                           (value.front() >= 'a' && value.front() <= 'z'))) {
        return false;
    }
    for (std::size_t i = 1; i < value.size(); ++i) {
        const char c = value[i];
        if (c == ':') return true;
        const bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        if (!valid) return false;
    }
    return false;
}

export std::string percentEncode(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (const unsigned char c : s) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

// 把启用的 query 参数拼进 URL（处理已有 ? / & 与空值）。
export std::string appendQuery(std::string url,
                               const std::vector<std::pair<std::string, std::string>>& params) {
    for (const auto& [k, v] : params) {
        if (k.empty()) continue;
        url.push_back(url.find('?') == std::string::npos ? '?' : '&');
        url += percentEncode(k);
        url.push_back('=');
        url += percentEncode(v);
    }
    return url;
}
