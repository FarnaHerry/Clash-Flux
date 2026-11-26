#pragma once

#include <huxerui/huxerui.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "theme_colors.h"

namespace clashflux::ui {
namespace detail {

class SvgPathReader {
public:
    explicit SvgPathReader(std::string_view input) : input_(input) {}

    bool HasNextCommand() {
        SkipSeparators();
        return pos_ < input_.size();
    }

    char PeekChar() {
        SkipSeparators();
        return pos_ < input_.size() ? input_[pos_] : '\0';
    }

    char ReadCommand() {
        SkipSeparators();
        return pos_ < input_.size() ? input_[pos_++] : '\0';
    }

    bool HasNumber() {
        SkipSeparators();
        if (pos_ >= input_.size()) return false;
        const char ch = input_[pos_];
        return (ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.';
    }

    float ReadNumber() {
        SkipSeparators();
        std::size_t cursor = pos_;
        const std::size_t size = input_.size();
        if (cursor >= size) return 0.0F;

        bool negative = false;
        if (input_[cursor] == '-' || input_[cursor] == '+') {
            negative = input_[cursor] == '-';
            ++cursor;
        }

        double value = 0.0;
        bool hasDigits = false;
        while (cursor < size && input_[cursor] >= '0' && input_[cursor] <= '9') {
            hasDigits = true;
            value = value * 10.0 + static_cast<double>(input_[cursor] - '0');
            ++cursor;
        }
        if (cursor < size && input_[cursor] == '.') {
            ++cursor;
            double scale = 0.1;
            while (cursor < size && input_[cursor] >= '0' && input_[cursor] <= '9') {
                hasDigits = true;
                value += static_cast<double>(input_[cursor] - '0') * scale;
                scale *= 0.1;
                ++cursor;
            }
        }
        if (!hasDigits) return 0.0F;

        if (cursor < size && (input_[cursor] == 'e' || input_[cursor] == 'E')) {
            std::size_t expCursor = cursor + 1;
            bool expNegative = false;
            if (expCursor < size && (input_[expCursor] == '-' || input_[expCursor] == '+')) {
                expNegative = input_[expCursor] == '-';
                ++expCursor;
            }
            int exponent = 0;
            bool hasExpDigits = false;
            while (expCursor < size && input_[expCursor] >= '0' && input_[expCursor] <= '9') {
                hasExpDigits = true;
                exponent = exponent * 10 + (input_[expCursor] - '0');
                ++expCursor;
            }
            if (hasExpDigits) {
                value *= std::pow(10.0, static_cast<double>(expNegative ? -exponent : exponent));
                cursor = expCursor;
            }
        }

        pos_ = cursor;
        return static_cast<float>(negative ? -value : value);
    }

    bool ReadFlag() {
        SkipSeparators();
        if (pos_ >= input_.size()) return false;
        return input_[pos_++] == '1';
    }

private:
    void SkipSeparators() {
        while (pos_ < input_.size()) {
            const char ch = input_[pos_];
            if (ch == ',' || std::isspace(static_cast<unsigned char>(ch))) ++pos_;
            else break;
        }
    }

    std::string_view input_;
    std::size_t pos_ = 0;
};

inline huxerui::Path ParseSvgPath(std::string_view d) {
    huxerui::Path path;
    SvgPathReader reader(d);
    huxerui::Point current{};
    huxerui::Point subpathStart{};
    char command = '\0';

    while (reader.HasNextCommand()) {
        const char next = reader.PeekChar();
        if (std::isalpha(static_cast<unsigned char>(next))) {
            command = reader.ReadCommand();
        }
        if (command == '\0') break;
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
        const bool relative = std::islower(static_cast<unsigned char>(command));

        if (upper == 'Z') {
            path.Close();
            current = subpathStart;
            command = '\0';
            continue;
        }
        if (!reader.HasNumber()) {
            command = '\0';
            continue;
        }

        switch (upper) {
        case 'M': {
            const float x = reader.ReadNumber();
            const float y = reader.ReadNumber();
            current = relative ? huxerui::Point{current.x + x, current.y + y} : huxerui::Point{x, y};
            subpathStart = current;
            path.MoveTo(current);
            command = relative ? 'l' : 'L';
            break;
        }
        case 'L': {
            const float x = reader.ReadNumber();
            const float y = reader.ReadNumber();
            current = relative ? huxerui::Point{current.x + x, current.y + y} : huxerui::Point{x, y};
            path.LineTo(current);
            break;
        }
        case 'H': {
            const float x = reader.ReadNumber();
            current.x = relative ? current.x + x : x;
            path.LineTo(current);
            break;
        }
        case 'V': {
            const float y = reader.ReadNumber();
            current.y = relative ? current.y + y : y;
            path.LineTo(current);
            break;
        }
        case 'C': {
            const float x1 = reader.ReadNumber();
            const float y1 = reader.ReadNumber();
            const float x2 = reader.ReadNumber();
            const float y2 = reader.ReadNumber();
            const float x = reader.ReadNumber();
            const float y = reader.ReadNumber();
            const huxerui::Point c1 = relative ? huxerui::Point{current.x + x1, current.y + y1}
                                               : huxerui::Point{x1, y1};
            const huxerui::Point c2 = relative ? huxerui::Point{current.x + x2, current.y + y2}
                                               : huxerui::Point{x2, y2};
            current = relative ? huxerui::Point{current.x + x, current.y + y} : huxerui::Point{x, y};
            path.CubicTo(c1, c2, current);
            break;
        }
        case 'A': {
            const float rx = reader.ReadNumber();
            const float ry = reader.ReadNumber();
            const float rotDeg = reader.ReadNumber();
            const bool largeArc = reader.ReadFlag();
            const bool sweep = reader.ReadFlag();
            const float x = reader.ReadNumber();
            const float y = reader.ReadNumber();
            current = relative ? huxerui::Point{current.x + x, current.y + y} : huxerui::Point{x, y};
            path.ArcTo({rx, ry}, rotDeg * std::numbers::pi_v<float> / 180.0F,
                       largeArc ? huxerui::ArcSize::Large : huxerui::ArcSize::Small,
                       sweep ? huxerui::ArcDirection::Clockwise : huxerui::ArcDirection::CounterClockwise,
                       current);
            break;
        }
        default:
            command = '\0';
            break;
        }
    }
    return path;
}

inline huxerui::Path SvgCirclePath(huxerui::Point center, float radius) {
    huxerui::Path path;
    path.MoveTo({center.x, center.y - radius})
        .ArcTo({radius, radius}, 0.0F, huxerui::ArcSize::Large, huxerui::ArcDirection::Clockwise,
               {center.x, center.y + radius})
        .ArcTo({radius, radius}, 0.0F, huxerui::ArcSize::Large, huxerui::ArcDirection::Clockwise,
               {center.x, center.y - radius})
        .Close();
    return path;
}

struct NavIconShapeSpec {
    std::string_view fillPath{};
    huxerui::PathFillRule fillRule = huxerui::PathFillRule::NonZero;
    std::string_view strokePath{};
    float strokeWidth = 0.0F;
    bool hasCircle = false;
    huxerui::Point circleCenter{};
    float circleRadius = 0.0F;
    bool hasRoundedRect = false;
    huxerui::Rect rect{};
    float rectRadius = 0.0F;
};

inline constexpr std::array<NavIconShapeSpec, 8> kNavIconSpecs{{
    // 0: home_selected
    {.fillPath =
         "M2.4 10.2 11.4 3.2a1 1 0 0 1 1.2 0l9 7a1 1 0 0 1-1.2 1.6l-.4-.3V20a1 1 0 0 1-1 1h-4v-5a1 1 0 0 0-1-1h-4a1 1 0 0 0-1 1v5H5a1 1 0 0 1-1-1v-8.5l-.4.3a1 1 0 0 1-1.2-1.6Z"},
    // 1: request_selected
    {.fillPath =
         "M21.7 1.3 1.7 8.3a.8.8 0 0 0-.1 1.5l9 3.9 3.7 8.6a.8.8 0 0 0 1.5-.1l7-20a.8.8 0 0 0-1.1-.9Z"
         " M10.72 12.49 18.9 4.31a.5.5 0 1 1 .71.71l-8.18 8.18a.5.5 0 0 1-.71-.71Z",
     .fillRule = huxerui::PathFillRule::EvenOdd},
    // 2: proxies_selected
    {.fillPath =
         "M12 2a10 10 0 1 1 0 20 10 10 0 0 1 0-20Z"
         " M4.1 11.4h3.8c.1-2.9.9-5.4 2.3-7a.6.6 0 0 1 .9.8c-1.2 1.5-1.9 3.7-2 6.2h5.8c-.1-2.5-.8-4.7-2-6.2a.6.6 0 0 1 .9-.8c1.4 1.6 2.2 4.1 2.3 7h3.8a.6.6 0 0 1 0 1.2h-3.8c-.1 2.9-.9 5.4-2.3 7a.6.6 0 0 1-.9-.8c1.2-1.5 1.9-3.7 2-6.2H9.1c.1 2.5.8 4.7 2 6.2a.6.6 0 0 1-.9.8c-1.4-1.6-2.2-4.1-2.3-7H4.1a.6.6 0 0 1 0-1.2Z",
     .fillRule = huxerui::PathFillRule::EvenOdd},
    // 3: route_selected
    {.fillPath =
         "M17 2.9a.7.7 0 0 1 1.2-.5l4 3.1a.65.65 0 0 1 0 1l-4 3.1a.7.7 0 0 1-1.2-.5Z"
         "m0 12a.7.7 0 0 1 1.2-.5l4 3.1a.65.65 0 0 1 0 1l-4 3.1a.7.7 0 0 1-1.2-.5Z",
     .strokePath = "M5 12h4a3 3 0 0 0 3-3 3 3 0 0 1 3-3h4M9 12a3 3 0 0 1 3 3 3 3 0 0 0 3 3h4",
     .strokeWidth = 2.8F,
     .hasCircle = true,
     .circleCenter = {4.0F, 12.0F},
     .circleRadius = 3.0F},
    // 4: connections_selected
    {.fillPath =
         "M7.5 5.5h2a1.5 1.5 0 0 1 0 3h-2a3.5 3.5 0 0 0 0 7h2a1.5 1.5 0 0 1 0 3h-2a6.5 6.5 0 0 1 0-13Z"
         "m7 0h2a6.5 6.5 0 0 1 0 13h-2a1.5 1.5 0 0 1 0-3h2a3.5 3.5 0 0 0 0-7h-2a1.5 1.5 0 0 1 0-3Z",
     .hasRoundedRect = true,
     .rect = {7.0F, 10.4F, 10.0F, 3.2F},
     .rectRadius = 1.6F},
    // 5: logs_selected
    {.fillPath =
         "M13 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9.4h-5A2 2 0 0 1 13 7.4Z"
         "M8 11.25a.75.75 0 0 0 0 1.5h8a.75.75 0 0 0 0-1.5Z"
         "m0 4a.75.75 0 0 0 0 1.5h6a.75.75 0 0 0 0-1.5Z"
         "m6.5-12.8v4.95c0 .28.22.5.5.5h4.55Z",
     .fillRule = huxerui::PathFillRule::EvenOdd},
    // 6: website_test_selected
    {.fillPath =
         "M5 4h14a3 3 0 0 1 3 3v10a3 3 0 0 1-3 3H5a3 3 0 0 1-3-3V7a3 3 0 0 1 3-3Z"
         "m1.5 7.2a.8.8 0 0 0 0 1.6h3a.8.8 0 0 0 .67-.36l1.16-1.74 1.89 4.62a.8.8 0 0 0 1.4.14l1.78-2.66h1.1a.8.8 0 0 0 0-1.6H16a.8.8 0 0 0-.67.36l-1.16 1.74-1.89-4.62a.8.8 0 0 0-1.4-.14L9.07 11.2Z",
     .fillRule = huxerui::PathFillRule::EvenOdd},
    // 7: gear
    {.fillPath =
         "M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58c.18-.14.23-.41.12-.61l-1.92-3.32c-.12-.22-.37-.29-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54c-.04-.24-.24-.41-.48-.41h-3.84c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96c-.22-.08-.47 0-.59.22L2.74 8.87c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.09.63-.09.94s.02.64.07.94l-2.03 1.58c-.18.14-.23.41-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32c.12-.22.07-.47-.12-.61l-2.01-1.58zM12 15.6c-1.98 0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z"},
}};

struct NavIconGeometry {
    std::vector<huxerui::Path> fills;
    huxerui::PathFillRule fillRule = huxerui::PathFillRule::NonZero;
    huxerui::Path stroke;
    float strokeWidth = 0.0F;
};

inline const std::array<NavIconGeometry, 8>& NavIconGeometries() {
    static const auto geometries = [] {
        std::array<NavIconGeometry, 8> result;
        for (std::size_t i = 0; i < kNavIconSpecs.size(); ++i) {
            const auto& spec = kNavIconSpecs[i];
            auto& geo = result[i];
            geo.fillRule = spec.fillRule;
            if (!spec.fillPath.empty()) geo.fills.push_back(ParseSvgPath(spec.fillPath));
            if (spec.hasCircle) geo.fills.push_back(SvgCirclePath(spec.circleCenter, spec.circleRadius));
            if (spec.hasRoundedRect) {
                geo.fills.push_back(huxerui::Path::RoundedRect(spec.rect, huxerui::CornerRadii{spec.rectRadius}));
            }
            if (!spec.strokePath.empty()) {
                geo.stroke = ParseSvgPath(spec.strokePath);
                geo.strokeWidth = spec.strokeWidth;
            }
        }
        return result;
    }();
    return geometries;
}

} // namespace detail

inline huxerui::VectorAsset BuildNavigationGradientIcon(
    std::size_t entryIndex, const FluxMultiColorPalette& palette, bool dark) {
    const auto& geometries = detail::NavIconGeometries();
    const auto index = entryIndex < geometries.size() ? entryIndex : 0U;
    struct CacheEntry {
        std::size_t index = 0;
        bool dark = false;
        FluxMultiColorPalette palette;
        huxerui::VectorAsset asset;
    };
    static std::mutex cacheMutex;
    static std::vector<CacheEntry> cache;
    {
        std::lock_guard lock(cacheMutex);
        for (const auto& item : cache) {
            if (item.index == index && item.dark == dark && item.palette == palette) return item.asset;
        }
    }

    const auto& geo = geometries[index];
    const auto& stops = dark ? palette.darkStops : palette.lightStops;
    const auto base = dark ? palette.darkBase : palette.lightBase;
    constexpr huxerui::Rect kViewBox{0.0F, 0.0F, 24.0F, 24.0F};

    auto asset = huxerui::VectorAsset::Create(kViewBox, huxerui::Size{24.0F, 24.0F},
        [&](huxerui::VectorBuilder& builder) {
            const auto paintLayer = [&](const huxerui::Brush& brush) {
                for (const auto& fill : geo.fills) {
                    builder.FillPath(fill, brush, kViewBox, geo.fillRule);
                }
                if (geo.strokeWidth > 0.0F && !geo.stroke.IsEmpty()) {
                    builder.StrokePath(geo.stroke, brush, kViewBox,
                        huxerui::StrokeStyle{.width = geo.strokeWidth, .cap = huxerui::StrokeCap::Round});
                }
            };
            paintLayer(base);
            if (stops.size() == 4U) {
                struct NavFieldSpec {
                    huxerui::Point center;
                    huxerui::Size radius;
                    float stop;
                };
                constexpr std::array<NavFieldSpec, 4> kFourFields{{
                    {{0.48F / 24.0F, 12.48F / 24.0F}, {19.68F / 24.0F, 24.96F / 24.0F}, 0.80F},
                    {{12.0F / 24.0F, 24.0F / 24.0F}, {18.72F / 24.0F, 21.12F / 24.0F}, 0.79F},
                    {{23.52F / 24.0F, 11.52F / 24.0F}, {19.68F / 24.0F, 24.96F / 24.0F}, 0.80F},
                    {{12.0F / 24.0F, 0.96F / 24.0F}, {18.72F / 24.0F, 21.12F / 24.0F}, 0.78F},
                }};
                for (std::size_t i = 0; i < 4U; ++i) {
                    auto transparent = stops[i];
                    transparent.alpha = 0.0F;
                    paintLayer(huxerui::RadialGradient{
                        .center = kFourFields[i].center,
                        .radius = kFourFields[i].radius,
                        .stops = {{0.0F, stops[i]}, {kFourFields[i].stop, transparent}},
                    });
                }
            } else {
                const auto centers = MultiColorFieldCenters(stops.size());
                for (std::size_t i = 0; i < stops.size(); ++i) {
                    auto transparent = stops[i];
                    transparent.alpha = 0.0F;
                    paintLayer(huxerui::RadialGradient{
                        .center = centers[i],
                        .radius = {0.82F, 0.96F},
                        .stops = {{0.0F, stops[i]}, {0.82F, transparent}},
                    });
                }
            }
        });

    {
        std::lock_guard lock(cacheMutex);
        if (cache.size() >= 64U) cache.erase(cache.begin(), cache.begin() + 16U);
        cache.push_back({index, dark, palette, asset});
    }
    return asset;
}

} // namespace clashflux::ui
