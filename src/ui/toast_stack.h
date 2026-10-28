#pragma once

#include <huxerui/huxerui.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "theme_colors.h"

namespace clashflux::ui {
inline constexpr std::size_t kMaximumVisibleToasts = 3;
inline constexpr float kToastStackSpacing = 8.0F;

struct ToastLayers {
    explicit ToastLayers(huxerui::LayerController value) : controller(std::move(value)) {}
    huxerui::LayerController controller;
};
inline void InstallToastLayers(huxerui::WindowContext& context) {
    context.Provide(std::make_shared<ToastLayers>(context.Layers()));
}
struct ToastEntry {
    huxerui::LayerId id;
    huxerui::StringVariant message;
    double duration;
    huxerui::State<bool> exiting{false};
    huxerui::TaskHandle expiration;
};
struct ToastPresentation {
    huxerui::ToastStyle style = huxerui::ToastStyle::Default();
    huxerui::Locale locale = huxerui::Locale::FromLanguageTag("en");
    bool reduced = false;
    bool operator==(const ToastPresentation&) const = default;
};

// Exiting rows retain their layout slot. New arrivals cannot cover their fade.
class ToastStack : public std::enable_shared_from_this<ToastStack> {
public:
    explicit ToastStack(huxerui::TaskScope tasks) : tasks_(std::move(tasks)) {}
    huxerui::State<std::vector<std::shared_ptr<ToastEntry>>> displayed{std::vector<std::shared_ptr<ToastEntry>>{}};
    huxerui::State<ToastPresentation> presentation{ToastPresentation{}};
    huxerui::LayerId Show(huxerui::StringVariant message, huxerui::ToastOptions options) {
        if (!std::isfinite(options.duration) || options.duration < 0.0)
            throw std::invalid_argument("Toast duration must be finite and non-negative");
        if (closed_) return 0;
        const auto id = ++serial_;
        wanted_.push_back(std::make_shared<ToastEntry>(id, std::move(message), options.duration));
        if (wanted_.size() > kMaximumVisibleToasts) Dismiss(wanted_.front()->id);
        FillSlots();
        return id;
    }
    bool Dismiss(huxerui::LayerId id) {
        if (closed_) return false;
        const auto found = std::find_if(wanted_.begin(), wanted_.end(),
            [id](const auto& entry) { return entry->id == id; });
        if (found == wanted_.end()) return false;
        const auto entry = *found;
        wanted_.erase(found);
        entry->expiration.Cancel();
        entry->exiting = true;
        if (std::find(displayed.Get().begin(), displayed.Get().end(), entry) == displayed.Get().end()) return true;
        tasks_.Launch([weak = weak_from_this(), id]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{kToastExitDuration + 0.04});
            if (auto stack = weak.lock(); stack && !stack->closed_) {
                auto rows = stack->displayed.Get();
                std::erase_if(rows, [id](const auto& row) { return row->id == id; });
                stack->displayed = std::move(rows);
                stack->FillSlots();
            }
        });
        return true;
    }
    void Close() {
        closed_ = true;
        for (const auto& entry : displayed.Get()) entry->expiration.Cancel();
        displayed = std::vector<std::shared_ptr<ToastEntry>>{};
        wanted_.clear();
    }
private:
    void FillSlots() {
        auto rows = displayed.Get();
        for (const auto& entry : wanted_) {
            if (rows.size() == kMaximumVisibleToasts) break;
            if (std::find(rows.begin(), rows.end(), entry) != rows.end()) continue;
            rows.push_back(entry);
            entry->expiration = tasks_.Launch([weak = weak_from_this(), entry]() -> huxerui::Task<void> {
                co_await huxerui::Delay(std::chrono::duration<double>{entry->duration});
                entry->expiration = {};
                if (auto stack = weak.lock()) stack->Dismiss(entry->id);
            });
        }
        displayed = std::move(rows);
    }
    huxerui::TaskScope tasks_;
    std::deque<std::shared_ptr<ToastEntry>> wanted_;
    huxerui::LayerId serial_ = 0;
    bool closed_ = false;
};
struct ToastStackContext {
    std::shared_ptr<ToastStack> stack;
    static ToastStackContext Default() { return {}; }
    bool operator==(const ToastStackContext&) const = default;
};
class FluxToastHandle {
public:
    explicit FluxToastHandle(std::shared_ptr<ToastStack> stack) : stack_(std::move(stack)) {}
    huxerui::LayerId Show(huxerui::StringVariant message, huxerui::ToastOptions options = {}) const {
        return stack_->Show(std::move(message), options);
    }
    bool Dismiss(huxerui::LayerId id) const { return stack_->Dismiss(id); }
private:
    std::shared_ptr<ToastStack> stack_;
};
inline FluxToastHandle UseFluxToast() {
    const auto stack = huxerui::UseEnvironment<ToastStackContext>().stack;
    if (!stack) throw std::logic_error("Toast stack requires the window presentation theme");
    return FluxToastHandle{stack};
}
inline huxerui::View ToastStackRow(std::shared_ptr<ToastStack> stack, std::shared_ptr<ToastEntry> entry,
    ToastPresentation presentation) {
    return huxerui::Scope([stack, entry, presentation] {
        const auto& style = presentation.style;
        const auto tasks = huxerui::UseTaskScope();
        auto shown = huxerui::UseState(false);
        huxerui::Lifecycle([tasks, shown] {
            tasks.Launch([shown]() -> huxerui::Task<void> {
                co_await huxerui::Delay(std::chrono::duration<double>{0});
                shown = true;
            });
            return [] {};
        });
        const bool exiting = entry->exiting.Get();
        const auto motion = style.motion.value_or(huxerui::PresentationMotion{});
        const huxerui::AnimationSpec animation = presentation.reduced ? huxerui::AnimationSpec{huxerui::SnapSpec{}}
            : exiting ? motion.exit : motion.enter;
        const float progress = shown.Get() && !exiting ? 1.0F : 0.0F;
        return huxerui::Text(entry->message).Style(style.text_style).With(
            huxerui::Frame{.max_width = style.maximum_width, .min_height = style.minimum_height},
            huxerui::Padding{style.padding}, huxerui::Background{style.background},
            huxerui::CornerRadius{style.corner_radii}, style.shadow,
            huxerui::Transition{huxerui::AnimateTo(progress, animation)}
                .Opacity(0.0F, 1.0F)
                .Scale(presentation.reduced ? 1.0F : kToastHiddenScale, 1.0F)
                .Offset({0.0F, presentation.reduced ? 0.0F : kToastSlideDistance}, {}),
            huxerui::Focusable{}, huxerui::Enabled(!exiting),
            huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                .live_region = huxerui::SemanticLiveRegion::Polite})
            .OnClick([stack, id = entry->id] { stack->Dismiss(id); })
            .Key("flux-toast-" + std::to_string(entry->id));
    });
}
inline huxerui::View ToastStackViewport(std::shared_ptr<ToastStack> stack) {
    return huxerui::Scope([stack] {
        const auto presentation = stack->presentation.Get();
        std::vector<huxerui::View> rows;
        for (const auto& entry : stack->displayed.Get())
            rows.push_back(ToastStackRow(stack, entry, presentation).Key("toast-row-" + std::to_string(entry->id)));
        return huxerui::ProvideEnvironment(presentation.locale, huxerui::Column {
          huxerui::Spacer().With(huxerui::Grow(1.0F)),
          huxerui::Row {
            huxerui::Spacer().With(huxerui::Grow(1.0F)),
            huxerui::Column(rows).With(huxerui::Spacing{kToastStackSpacing},
                huxerui::CrossAlign{huxerui::CrossAxisAlignment::Center}),
            huxerui::Spacer().With(huxerui::Grow(1.0F)),
          },
        }.With(huxerui::SafeAreaPadding{}, huxerui::Padding{presentation.style.viewport_padding}));
    });
}
inline huxerui::View FluxToastStack(huxerui::View content) {
    return huxerui::Scope([content] {
        const auto tasks = huxerui::UseTaskScope();
        const auto state = huxerui::UseState(std::make_shared<ToastStack>(tasks));
        const auto stack = state.Get();
        const auto layers = huxerui::UseService<ToastLayers>()->controller;
        const auto style = huxerui::UseEnvironment<huxerui::ToastStyle>();
        const auto locale = huxerui::UseEnvironment<huxerui::Locale>();
        const bool reduced = huxerui::UseTheme().motion.reduced_motion;
        huxerui::Lifecycle([stack, style, locale, reduced] {
            stack->presentation = ToastPresentation{style, locale, reduced};
            return [] {};
        }, style, locale, reduced);
        huxerui::Lifecycle([stack, layers] {
            const auto id = layers.Attach({.level = huxerui::LayerLevel::Notification,
                .pointer_policy = huxerui::LayerPointerPolicy::Content, .trap_focus = false,
                .cancel_policy = huxerui::LayerCancelPolicy::PassThrough},
                [stack] { return ToastStackViewport(stack); });
            return [stack, layers, id] { stack->Close(); layers.Dismiss(id); };
        });
        return huxerui::ProvideEnvironment(ToastStackContext{stack}, content);
    });
}
} // namespace clashflux::ui
