#pragma once

#include <algorithm>
#include <cmath>
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

struct MeasureProfileGridColumns {
    huxerui::State<std::size_t> columns;
    huxerui::TaskScope tasks;

    class Extension final : public huxerui::NodeExtension {
    public:
        Extension(huxerui::ViewNode&, const MeasureProfileGridColumns& spec)
            : columns_(spec.columns), tasks_(spec.tasks) {}

        void Update(huxerui::ViewNode&, const MeasureProfileGridColumns& spec) {
            columns_ = spec.columns;
            tasks_ = spec.tasks;
        }

        PaintInvalidation PrepareGeometry(huxerui::ViewNode& node, huxerui::TextMeasurer&) override {
            constexpr float preferredWidth = 300.0F;
            const float width = node.LayoutSize().width;
            const auto count = std::clamp(
                huxerui::GridColumns::Adaptive(preferredWidth).Resolve(width, kProfileCardSpacing),
                std::size_t{2}, std::size_t{4});
            if (width > 0.0F && columns_.Get() != count) {
                // Apply the count after geometry preparation so the next layout sees stable tracks.
                tasks_.Post([columns = columns_, count] { columns = count; });
            }
            return PaintInvalidation::None;
        }

    private:
        huxerui::State<std::size_t> columns_;
        huxerui::TaskScope tasks_;
    };
};

inline huxerui::VirtualGrid ApplyProfileGridColumns(huxerui::VirtualGrid grid,
    bool compact, bool groupByType, huxerui::State<std::size_t> columns, huxerui::TaskScope tasks) {
    if (compact || !groupByType)
        return std::move(grid).Columns(huxerui::GridColumns::Fixed(1));
    return std::move(grid).Columns(huxerui::GridColumns::Fixed(columns.Get()))
        .With(MeasureProfileGridColumns{columns, tasks});
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

namespace detail {

struct QuotaGradientFill {
    class Extension;
    float fraction;
    float cornerRadius;
    FluxMultiColorPalette palette = DefaultMultiColorPalette();
    bool operator==(const QuotaGradientFill&) const = default;
};

class QuotaGradientFill::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const QuotaGradientFill& fill) : fill_(fill) {}

    void Update(huxerui::ViewNode&, const QuotaGradientFill& fill) {
        fill_ = fill;
        InvalidatePaint(PaintInvalidation::Foreground);
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        auto bounds = node.Bounds();
        bounds.width *= fill_.fraction;
        if (bounds.IsEmpty()) return;
        const float radius = std::clamp(fill_.cornerRadius, 0.0F, std::min(bounds.width, bounds.height) * 0.5F);
        paint.PushClip(bounds, huxerui::CornerRadii{radius});
        paint.DrawImage(MultiColorPrimaryGradient(fill_.palette), bounds);
        paint.PopClip();
    }

private:
    QuotaGradientFill fill_;
};

inline float SegmentedProgressBezierCoord(float time, float first, float second) {
    const float inv = 1.0F - time;
    return 3.0F * inv * inv * time * first + 3.0F * inv * time * time * second + time * time * time;
}

inline float SegmentedProgressPosition(float phase, float delay, float duration) {
    if (phase <= delay) return 0.0F;
    if (phase >= delay + duration) return 1.0F;
    const float target = std::clamp((phase - delay) / duration, 0.0F, 1.0F);
    float lower = 0.0F;
    float upper = 1.0F;
    for (int i = 0; i < 16; ++i) {
        const float mid = (lower + upper) * 0.5F;
        if (SegmentedProgressBezierCoord(mid, 0.3F, 0.8F) < target) lower = mid;
        else upper = mid;
    }
    return SegmentedProgressBezierCoord((lower + upper) * 0.5F, 0.0F, 0.15F);
}

struct IndeterminateGradientFill {
    class Extension;
    float cornerRadius = 0.0F;
    double duration = 0.0;
    FluxMultiColorPalette palette = DefaultMultiColorPalette();
    bool operator==(const IndeterminateGradientFill&) const = default;
};

class IndeterminateGradientFill::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const IndeterminateGradientFill& spec)
        : spec_(spec), phase_(Animated(spec, false) ? 0.0F : 0.5F) {}

    void Update(huxerui::ViewNode&, const IndeterminateGradientFill& spec) {
        if (spec_ == spec) return;
        spec_ = spec;
        if (!Animated(spec_, false)) phase_ = 0.5F;
        InvalidatePaint(PaintInvalidation::Foreground);
    }

    FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
        if (!Animated(spec_, frame.reduced_motion)) {
            if (phase_ != 0.5F) {
                phase_ = 0.5F;
                InvalidatePaint(PaintInvalidation::Foreground);
            }
            return {};
        }
        phase_ += static_cast<float>(frame.delta_time / spec_.duration);
        phase_ -= std::floor(phase_);
        InvalidatePaint(PaintInvalidation::Foreground);
        return {.needs_frame = true};
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        const auto frame = node.Bounds();
        if (frame.IsEmpty()) return;
        constexpr float kCycle = 1750.0F;
        const float firstHead = SegmentedProgressPosition(phase_, 0.0F, 1000.0F / kCycle);
        const float firstTail = SegmentedProgressPosition(phase_, 250.0F / kCycle, 1000.0F / kCycle);
        const float secondHead = SegmentedProgressPosition(phase_, 650.0F / kCycle, 850.0F / kCycle);
        const float secondTail = SegmentedProgressPosition(phase_, 900.0F / kCycle, 850.0F / kCycle);
        const float trackRadius = std::clamp(spec_.cornerRadius, 0.0F, frame.height * 0.5F);
        const auto drawSegment = [&](float start, float end) {
            start = std::clamp(start, 0.0F, 1.0F);
            end = std::clamp(end, 0.0F, 1.0F);
            if (end <= start) return;
            const float x = frame.x + frame.width * start;
            const float width = frame.width * (end - start);
            const float radius = std::min(trackRadius, width * 0.5F);
            paint.PushClip({x, frame.y, width, frame.height}, huxerui::CornerRadii{radius});
            paint.DrawImage(MultiColorPrimaryGradient(spec_.palette), frame);
            paint.PopClip();
        };
        drawSegment(firstTail, firstHead);
        drawSegment(secondTail, secondHead);
    }

private:
    static bool Animated(const IndeterminateGradientFill& spec, bool reducedMotion) noexcept {
        return !reducedMotion && std::isfinite(spec.duration) && spec.duration > 0.0;
    }

    IndeterminateGradientFill spec_;
    float phase_ = 0.0F;
};

} // namespace detail

inline huxerui::View FluxIndeterminateProgressBar() {
    return huxerui::Scope([] {
        const auto& gradient = huxerui::UseEnvironment<FluxGradientStyle>();
        if (!gradient.enabled) return huxerui::View{huxerui::ProgressBar{}};
        auto style = huxerui::UseEnvironment<huxerui::ProgressBarStyle>();
        style.indicator_color = huxerui::Color::Transparent();
        return huxerui::View{huxerui::ProvideEnvironment(style, huxerui::ProgressBar{}
            .With(detail::IndeterminateGradientFill{
                style.corner_radius, style.animation_duration, ResolveGradientPalette(gradient)}))};
    });
}

inline huxerui::View ProfileQuotaBar(float fraction) {
    return huxerui::Scope([fraction] {
        const auto& gradient = huxerui::UseEnvironment<FluxGradientStyle>();
        auto style = huxerui::UseEnvironment<huxerui::ProgressBarStyle>();
        style.height = kProfileQuotaBarHeight;
        if (gradient.enabled) {
            // Keep native measurement, track and semantics. Paint the determinate segment with the shared vector.
            style.indicator_color = huxerui::Color::Transparent();
            const float progress = fraction > 0.0F ? std::min(fraction, 1.0F) : 0.0F;
            return huxerui::ProvideEnvironment(style, huxerui::ProgressBar(progress)
                .With(detail::QuotaGradientFill{
                    progress, style.corner_radius, ResolveGradientPalette(gradient)}));
        }
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
