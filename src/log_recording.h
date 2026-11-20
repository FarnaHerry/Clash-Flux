#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <string_view>

namespace clashflux::logging {

enum class Level { Debug, Info, Warning, Error, Off };

inline constexpr char kLevelSetting[] = "app.log_level";
inline constexpr std::array<std::string_view, 5> kLevelNames{"debug", "info", "warning", "error", "off"};

constexpr std::string_view LevelName(Level level) noexcept {
    return kLevelNames[static_cast<std::size_t>(level)];
}

constexpr Level SavedLevel(std::string_view value) noexcept {
    for (std::size_t index = 0; index < kLevelNames.size(); ++index)
        if (value == kLevelNames[index]) return static_cast<Level>(index);
    return Level::Debug;
}

constexpr Level MessageLevel(std::string_view level) noexcept {
    if (level == "debug" || level == "trace") return Level::Debug;
    if (level == "warning" || level == "warn") return Level::Warning;
    if (level == "error" || level == "fatal" || level == "panic") return Level::Error;
    return Level::Info;
}

namespace detail {
inline std::atomic<Level> threshold{Level::Debug};
}

inline void SetRecordingLevel(Level level) noexcept {
    detail::threshold.store(level, std::memory_order_relaxed);
}

inline bool ShouldRecord(Level level) noexcept {
    const auto threshold = detail::threshold.load(std::memory_order_relaxed);
    return threshold != Level::Off && level >= threshold;
}

} // namespace clashflux::logging
