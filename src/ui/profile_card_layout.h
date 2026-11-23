#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "ui.h"

namespace clashflux::ui {

inline constexpr float kCardWidth = 340.0F;
inline constexpr float kWideProfileCardHeight = 152.0F;
inline constexpr float kCompactProfileCardHeight = 104.0F;
inline constexpr float kWideProfileCardHeadingHeight = 28.0F;
inline constexpr float kProfileCardTextRowHeight = 16.0F;
inline constexpr float kProfileCardRadius = 12.0F;
inline constexpr float kProfileCardSpacing = 12.0F;
inline constexpr float kProfileQuotaBarHeight = 4.0F;

constexpr float ProfileCardHeight(bool compact) {
    return compact ? kCompactProfileCardHeight : kWideProfileCardHeight;
}

inline huxerui::GridColumns ProfileGridColumns(bool compact, bool groupByType) {
    return compact || !groupByType ? huxerui::GridColumns::Fixed(1)
                                  : huxerui::GridColumns::Adaptive(kCardWidth);
}

inline std::string ProfileSubscriptionAddress(std::string_view url) {
    const auto uri = huxerui::Uri::Parse(url);
    if (!uri || !uri->Authority()) return {};
    auto authority = *uri->Authority();
    if (const auto userInfo = authority.rfind('@'); userInfo != std::string_view::npos)
        authority.remove_prefix(userInfo + 1);
    return authority.empty() ? std::string{} : std::string(authority);
}

constexpr float CompactProfileCardHeadingHeight(bool nativeVpn) {
    return nativeVpn ? 32.0F : 40.0F;
}

inline float ProfileTrafficProgress(std::int64_t used, std::int64_t total) {
    return total > 0 ? std::clamp(static_cast<float>(used) / static_cast<float>(total), 0.0F, 1.0F) : 1.0F;
}

inline huxerui::View ProfileQuotaBar(float fraction) {
    return huxerui::Scope([fraction] {
        const auto& colors = huxerui::UseTheme().colors;
        auto style = huxerui::UseEnvironment<huxerui::ProgressBarStyle>();
        style.height = kProfileQuotaBarHeight;
        style.indicator_color = colors.secondary;
        style.track_color = colors.secondary_container;
        return huxerui::ProvideEnvironment(style, huxerui::ProgressBar(fraction));
    });
}

inline huxerui::View ProfileCardRows(std::vector<huxerui::View> rows) {
    return huxerui::Column(std::move(rows)).With(huxerui::Spacing(4.0F), huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

inline huxerui::View ProfileCardTrafficContent(std::string traffic, std::string expiry,
    float progress, huxerui::Color muted) {
    huxerui::View bar = ProfileQuotaBar(progress).Key("profile-traffic-bar");
    if (traffic.empty() && expiry.empty()) return bar;

    // When one value is present, keep it on its own side of the row.
    const huxerui::TextStyle caption{huxerui::Font::System(font_size::kCaption), muted};
    return huxerui::Column {
      huxerui::Row {
        FittedSingleLineLabel(traffic, caption).With(huxerui::Grow(1.0F)).Key("profile-traffic-label"),
        FittedSingleLineLabel(expiry, caption, huxerui::TextAlign::Trailing)
            .With(huxerui::Grow(1.0F)).Key("profile-expiry-label"),
      }.With(huxerui::Frame{.height = kProfileCardTextRowHeight}, huxerui::Spacing(6.0F),
             huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
      bar,
    }.With(huxerui::Spacing(4.0F), huxerui::Grow(0.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

inline huxerui::View WideProfileCardHeading(std::string name, huxerui::Color foreground,
    huxerui::View drag, huxerui::View refresh) {
    return huxerui::Row {
      drag,
      FittedSingleLineLabel(name, huxerui::TextStyle{
          huxerui::Font::System(font_size::kBody).WithWeight(huxerui::FontWeight::SemiBold), foreground})
          .With(huxerui::Grow(1.0F)),
      refresh,
    }.With(huxerui::Frame{.height = kWideProfileCardHeadingHeight}, huxerui::Spacing(6.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

inline huxerui::View CompactProfileCardContent(std::string name, std::string status,
    const SelectableTileColors& colors, huxerui::View drag, huxerui::View more,
    huxerui::View details, bool nativeVpn = false, huxerui::View busy = {}, std::string updated = {}) {
    std::vector<huxerui::View> heading;
    heading.push_back(drag);
    heading.push_back(FittedSingleLineLabel(name, huxerui::TextStyle{
        huxerui::Font::System(font_size::kBody).WithWeight(huxerui::FontWeight::SemiBold), colors.fg})
        .With(huxerui::Grow(1.0F)).Key("profile-compact-name"));
    if (busy) heading.push_back(busy);
    if (!status.empty()) heading.push_back(huxerui::Text(status).Style(huxerui::TextStyle{
        huxerui::Font::System(font_size::kCaption), colors.muted}).Key("profile-compact-status"));
    heading.push_back(more);
    std::vector<huxerui::View> rows{
      huxerui::Row(std::move(heading)).With(
          huxerui::Frame{.height = CompactProfileCardHeadingHeight(nativeVpn)}, huxerui::Grow(0.0F),
          huxerui::Spacing(6.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
      details,
    };
    if (!updated.empty()) rows.push_back(FittedSingleLineLabel(updated, huxerui::TextStyle{
        huxerui::Font::System(font_size::kCaption), colors.muted})
        .With(huxerui::Frame{.height = kProfileCardTextRowHeight}).Key("profile-compact-updated"));
    return ProfileCardRows(std::move(rows));
}

} // namespace clashflux::ui
