#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "ui.h"

namespace clashflux::ui {

inline constexpr float kCardWidth = 340.0F;
inline constexpr float kCardHeight = 180.0F;
inline constexpr float kCompactProfileCardHeight = 88.0F;
inline constexpr float kCompactProfileCardSpacing = 6.0F;

constexpr float ProfileCardHeight(bool compact) {
    return compact ? kCompactProfileCardHeight : kCardHeight;
}

constexpr float CompactProfileCardHeadingHeight(bool nativeVpn) {
    return nativeVpn ? 32.0F : 40.0F;
}

inline float ProfileTrafficProgress(std::int64_t used, std::int64_t total) {
    return total > 0 ? std::clamp(static_cast<float>(used) / static_cast<float>(total), 0.0F, 1.0F) : 1.0F;
}

// Empty labels keep their slots. Unknown quota retains the wide card's full bar.
inline huxerui::View CompactProfileTrafficContent(std::string traffic, std::string expiry,
    float progress, huxerui::Color muted) {
    const huxerui::TextStyle caption{huxerui::Font::System(font_size::kCaption), muted};
    return huxerui::Column {
      huxerui::Row {
        FittedSingleLineLabel(traffic, caption).With(huxerui::Grow(1.0F)).Key("profile-traffic-label"),
        FittedSingleLineLabel(expiry, caption, huxerui::TextAlign::Trailing)
            .With(huxerui::Grow(1.0F)).Key("profile-expiry-label"),
      }.With(huxerui::Frame{.height = 20.0F}, huxerui::Spacing(6.0F),
             huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
      huxerui::ProgressBar(progress).With(huxerui::Frame{.height = 3.0F}).Key("profile-traffic-bar"),
    }.With(huxerui::Spacing(4.0F), huxerui::Grow(0.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

inline huxerui::View CompactProfileCardContent(std::string name, std::string status,
    const SelectableTileColors& colors, huxerui::View drag, huxerui::View more,
    huxerui::View details, bool nativeVpn = false, huxerui::View busy = {}) {
    std::vector<huxerui::View> heading;
    heading.push_back(drag);
    heading.push_back(FittedSingleLineLabel(name, huxerui::TextStyle{
        huxerui::Font::System(font_size::kBody).WithWeight(huxerui::FontWeight::SemiBold), colors.fg})
        .With(huxerui::Grow(1.0F)).Key("profile-compact-name"));
    if (busy) heading.push_back(busy);
    if (!status.empty()) heading.push_back(huxerui::Text(status).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kCaption), colors.muted}).Key("profile-compact-status"));
    heading.push_back(more);
    return huxerui::Column {
      huxerui::Row(std::move(heading)).With(
          huxerui::Frame{.height = CompactProfileCardHeadingHeight(nativeVpn)}, huxerui::Grow(0.0F),
          huxerui::Spacing(6.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
      details,
    }.With(huxerui::Spacing(4.0F), huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace clashflux::ui
