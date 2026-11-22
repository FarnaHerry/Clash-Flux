#pragma once
#include <chrono>

namespace clashflux::website {
inline constexpr auto kTimeout = std::chrono::seconds{10};
inline constexpr const char* kTimeoutError = "网站测试超时（10 秒）";
} // namespace clashflux::website
