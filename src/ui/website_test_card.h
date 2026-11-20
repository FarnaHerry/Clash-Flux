#pragma once
#include "website_test_model.h"
#include "ui.h"
#include "app_resources.h"
#include "action_menu.h"
#include "country_flag.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <numbers>
#include <optional>

namespace clashflux::ui {

struct WebsiteTestPulse {
    class Extension;
    huxerui::Color color;
    float iconSize;
    bool busy;
    bool animate;
    bool operator==(const WebsiteTestPulse&) const = default;
};

class WebsiteTestPulse::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode&, const WebsiteTestPulse& pulse) : pulse_(pulse) {}

    void Update(huxerui::ViewNode&, const WebsiteTestPulse& pulse) {
        pulse_ = pulse;
        if (!pulse_.animate) startedAt_.reset();
        InvalidatePaint(PaintInvalidation::Foreground);
    }

    FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
        if (!pulse_.animate || frame.reduced_motion) {
            if (startedAt_) {
                startedAt_.reset();
                InvalidatePaint(PaintInvalidation::Foreground);
            }
            return {};
        }
        if (!startedAt_) startedAt_ = frame.timestamp;
        elapsed_ = frame.timestamp - *startedAt_;
        InvalidatePaint(PaintInvalidation::Foreground);
        return {.wake_after = 1.0 / 30.0};
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        if (!pulse_.busy) return;
        const auto bounds = node.ContentBounds();
        const float width = std::min(pulse_.iconSize * 1.6F, bounds.width * 0.8F);
        const float left = bounds.x + (bounds.width - width) * 0.5F;
        const float mid = bounds.y + bounds.height * 0.5F;
        const float scale = width / 60.0F;
        const float center = startedAt_ ? -0.25F + static_cast<float>(std::fmod(elapsed_, 1.5)) : 0.5F;
        const auto gaussian = [](float distance, float spread) {
            const float ratio = distance / spread;
            return std::exp(-ratio * ratio);
        };
        // Sample the reference's P-QRS-T waveform between the two fixed contacts.
        constexpr int samples = 44;
        huxerui::Path wave;
        wave.MoveTo({left, mid});
        for (int i = 1; i < samples; ++i) {
            const float x = static_cast<float>(i) / samples;
            const float d = x - center;
            const float y = -3.0F * gaussian(d + 0.10F, 0.05F) - 15.0F * gaussian(d, 0.028F)
                          + 7.0F * gaussian(d - 0.045F, 0.035F) - 4.0F * gaussian(d - 0.13F, 0.055F);
            wave.LineTo({left + width * x, mid + scale * y});
        }
        wave.LineTo({left + width, mid});
        auto glow = pulse_.color;
        glow.alpha *= 0.18F;
        const huxerui::StrokeStyle stroke{.width = 2.2F * scale, .cap = huxerui::StrokeCap::Round,
                                         .join = huxerui::StrokeJoin::Round};
        auto glowStroke = stroke;
        glowStroke.width *= 2.4F;
        paint.StrokePath(wave, glow, glowStroke);
        paint.StrokePath(std::move(wave), pulse_.color, stroke);
        const float blink = startedAt_ ? static_cast<float>(std::cos(2.0 * std::numbers::pi * elapsed_ / 1.2)) : 0.0F;
        auto contact = pulse_.color;
        contact.alpha *= startedAt_ ? 0.675F - 0.325F * blink : 0.9F;
        paint.DrawCircle({left, mid}, 2.5F * scale, contact);
        contact.alpha = pulse_.color.alpha * (startedAt_ ? 0.675F + 0.325F * blink : 0.9F);
        paint.DrawCircle({left + width, mid}, 2.5F * scale, contact);
    }

private:
    WebsiteTestPulse pulse_;
    std::optional<double> startedAt_;
    double elapsed_ = 0.0;
};

inline huxerui::View WebsiteTestAction(WebsiteTestPhase phase, huxerui::StringVariant label, std::function<void()> start) {
    return huxerui::Scope([phase, label, start = std::move(start)] {
        const auto& theme = huxerui::UseTheme();
        const bool busy = phase == WebsiteTestPhase::Queued || phase == WebsiteTestPhase::Running;
        const bool foreground = huxerui::UseWindow().LifecycleState() != huxerui::WindowLifecycleState::Background;
        auto style = huxerui::UseEnvironment<huxerui::IconButtonStyle>();
        if (busy) style.disabled_foreground = huxerui::Color::Transparent();
        const auto button = huxerui::IconButton(app::images::website_heartbeat, label)
            .With(huxerui::Tooltip(label),
                  huxerui::Enabled(!busy),
                  huxerui::Semantics{.role = huxerui::SemanticRole::Button, .label = label, .busy = busy},
                  WebsiteTestPulse{phase == WebsiteTestPhase::Queued ? theme.colors.on_surface_variant : theme.colors.primary,
                                    style.icon_size, busy,
                                    phase == WebsiteTestPhase::Running && foreground && !theme.motion.reduced_motion})
            .OnClick([busy, start] { if (!busy) start(); });
        return huxerui::ProvideEnvironment(style, button);
    });
}

inline std::string WebsiteTestTime(std::int64_t value, bool full = false) {
    if (value <= 0) return {};
    const auto time = static_cast<std::time_t>(value);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &time) != 0) return {};
#else
    if (!localtime_r(&time, &local)) return {};
#endif
    char text[32]{};
    std::strftime(text, sizeof(text), full ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S", &local);
    return text;
}

inline huxerui::View WebsiteCountry(std::string code) {
    return huxerui::Scope([code = std::move(code)] {
        std::vector<huxerui::View> views;
        views.push_back(CountryFlag(code, 20.0F, 15.0F));
        views.push_back(huxerui::Text(code));
        return huxerui::Row(std::move(views)).With(huxerui::Spacing(4.0F),
            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    });
}

inline huxerui::View WebsiteTestCard(
    WebsiteTestSite site, std::function<void()> start,
    std::function<void()> edit, std::function<void()> remove, huxerui::TaskScope menuTasks) {
    return huxerui::Scope([site, start, edit, remove, menuTasks] {
        const auto& theme = huxerui::UseTheme();
        const auto menu = UseActionMenu();
        const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
        const auto dialog = huxerui::UseDialog();
        const auto confirmDelete = [dialog, name = site.name, remove] {
            dialog.Show([name, remove](huxerui::DialogContext context) {
                return DialogScaffold(DialogHeader(Localized("删除网站")),
                    huxerui::Text(LocalizedFormat("确定删除“{}”吗？此操作无法撤销。", name)),
                    DialogActions({
                        DialogAction(Localized("取消"), [context] { context.Dismiss(); }, false),
                        DialogAction(Localized("删除"), [context, remove] { context.Dismiss(); remove(); },
                            true, true, true).Key("website-delete-confirm"),
                    }), DialogSize::Small);
            });
        };
        const auto& result = site.result;
        const auto testLabel = result.phase == WebsiteTestPhase::Queued ? Localized("等待测试") :
            result.phase == WebsiteTestPhase::Running ? Localized("测试中…") : Localized("测试");
        auto status = Localized("未测试");
        auto color = theme.colors.on_surface_variant;
        if (result.phase == WebsiteTestPhase::Queued) status = Localized("等待测试");
        if (result.phase == WebsiteTestPhase::Running) { status = Localized("测试中…"); color = theme.colors.primary; }
        if (result.phase == WebsiteTestPhase::Cancelled) status = Localized("已取消");
        const bool success = result.phase == WebsiteTestPhase::Responded && result.status >= 200 && result.status < 400;
        if (result.phase == WebsiteTestPhase::Failed || (result.phase == WebsiteTestPhase::Responded && !success)) {
            status = Localized("失败"); color = theme.colors.error;
        }
        if (success) { status = Localized("可访问"); color = theme.colors.on_surface_variant; }
        const bool located = success && !result.country.empty();
        const auto locatedHint = result.locationSource == WebsiteLocationSource::CoreOutbound
            ? LocalizedFormat("出口位置 {}，经本应用内核出站核实", result.country)
            : result.locationSource == WebsiteLocationSource::DirectEgress
            ? LocalizedFormat("出口位置 {}，经直连出口核实", result.country)
            : LocalizedFormat("出口位置 {}，来自网站同源检测", result.country);
        const auto hint = !result.error.empty() ? huxerui::StringVariant{result.error} :
            located ? locatedHint :
            success ? Localized("网站可访问，未能确认出口位置") : status;
        const auto showMenu = [edit, confirmDelete] {
            return std::vector<ActionMenuEntry>{
                ActionMenuItem(app::images::edit, Localized("编辑"), edit),
                ActionMenuItem(app::images::trash, Localized("删除"), confirmDelete).Danger(),
            };
        };
        const std::string completedTime = WebsiteTestTime(result.completedAt);
        constexpr float resultHeight = 22.0F;
        constexpr huxerui::Frame timeSlot{.width = 56.0F, .height = resultHeight};
        auto content = huxerui::Column {
          huxerui::Row {
            huxerui::Image(app::images::globe).Tint(theme.colors.primary).With(huxerui::Frame{.width = 20.0F, .height = 20.0F}),
            huxerui::Text(site.name, huxerui::TextRole::Title).LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis).With(huxerui::Grow(1.0F)),
            WebsiteTestAction(result.phase, testLabel, start)
                .Key("website-test-" + std::to_string(site.id)),
            compact ? huxerui::View{huxerui::IconButton(app::images::more_vertical, Localized("更多操作"))
                .With(menu.Target(), huxerui::Tooltip(Localized("更多操作")))
                .OnClick([menu, showMenu] { menu.Show(showMenu()); })
                .Key("website-menu-" + std::to_string(site.id))} : huxerui::View{},
          }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
          huxerui::Row {
            (located ? WebsiteCountry(result.country) : huxerui::View{huxerui::Text(status)
                .LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis).VerticalAlign(huxerui::TextVerticalAlign::Center)})
                .With(huxerui::Frame{.height = resultHeight}, huxerui::Foreground(color),
                      huxerui::Grow(1.0F), huxerui::Tooltip(hint))
                .Key("website-result-" + std::to_string(site.id)),
            (completedTime.empty()
                 ? PlaceholderSlot()
                 : huxerui::View{huxerui::Text(completedTime)
                       .LineLimit(1).Overflow(huxerui::TextOverflow::Ellipsis)
                       .Align(huxerui::TextAlign::Trailing).VerticalAlign(huxerui::TextVerticalAlign::Center)
                       .Style(huxerui::TextStyle{huxerui::Font::System(font_size::kCaption),
                                                 theme.colors.on_surface_variant})
                       .With(huxerui::Tooltip(WebsiteTestTime(result.completedAt, true)))})
                .With(timeSlot).Key("website-time-" + std::to_string(site.id)),
          }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        }.With(huxerui::Spacing(8.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        auto card = Card(std::move(content));
        if (compact) return card;
        return std::move(card).With(menu.Target(), huxerui::Focusable())
            .On<huxerui::ViewEvents::ContextMenuRequested>(
            [menu, menuTasks, showMenu](huxerui::Point position) {
                menuTasks.Launch([menu, position, entries = showMenu()]() mutable -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    menu.ShowAt(position, std::move(entries));
                });
            });
    });
}

} // namespace clashflux::ui
