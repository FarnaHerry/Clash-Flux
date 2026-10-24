#include "box_cat.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

#include "app_resources.h"
#include "theme_colors.h"

namespace clashflux::ui {
namespace {

constexpr double kFailureDuration = 2.4;
constexpr double kTransitionDuration = 1.2;
constexpr double kLoopFrameInterval = 1.0 / 30.0;
constexpr float kCatRiseEnd = 0.63F;

float Segment(float value, float start, float end) {
    const float t = std::clamp((value - start) / (end - start), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

huxerui::Color Alpha(huxerui::Color color, float alpha) {
    color.alpha *= alpha;
    return color;
}

struct IdlePose {
    float leftEar = 0;
    float rightEar = 0;
    float tail = 0;
};

IdlePose RunningPose(double seconds) {
    constexpr double pi = 3.141592653589793;
    const double phase = std::fmod(seconds, 5.6);
    const auto twitch = [phase](double start) {
        const double t = (phase - start) / 0.65;
        return t <= 0 || t >= 1 ? 0.0F
            : static_cast<float>(std::sin(2 * pi * t) * std::sin(pi * t));
    };
    const float settle = Segment(static_cast<float>(seconds), 0, 0.4F);
    return {.leftEar = -0.06F * twitch(0.75),
            .rightEar = 0.05F * twitch(1.05),
            .tail = settle * static_cast<float>(0.085 * std::sin(2 * pi * seconds / 3.8) +
                                                0.015 * std::sin(4 * pi * seconds / 3.8))};
}

float PhaseProgress(BoxCatPhase phase) {
    switch (phase) {
        case BoxCatPhase::Sleeping:
        case BoxCatPhase::Stopping:
        case BoxCatPhase::Failed:
            return 0.0F;
        case BoxCatPhase::Starting:
            return 0.30F;
        case BoxCatPhase::Running:
            return 1.0F;
    }
    return 0.0F;
}

struct BoxCatCompletion {
    huxerui::TaskScope tasks;
    std::function<void()> callback;
};

struct BoxMotion {
    class Extension;
    huxerui::VectorAsset catHead, tail, rear, front, logo, eyeLeft, eyeOpen, eyeWink;
    huxerui::VectorAsset catRunning, earLeft, earRight;
    huxerui::VectorAsset interior, eyesSleeping, eyesContent, hand, handFill;
    std::shared_ptr<BoxCatProbe> probe;
    std::shared_ptr<BoxCatCompletion> completion;
    BoxCatPhase phase = BoxCatPhase::Sleeping;
    std::optional<BoxCatPhase> initialPose;
    bool motionEnabled = true;
    bool visible = true;
    huxerui::Color primary, foreground;

    auto PaintInputs() const {
        return std::tie(catHead, tail, rear, front, logo, eyeLeft, eyeOpen, eyeWink,
            catRunning, earLeft, earRight, interior, eyesSleeping, eyesContent,
            hand, handFill, primary, foreground);
    }

    bool operator==(const BoxMotion& other) const {
        if (PaintInputs() != other.PaintInputs() || probe != other.probe ||
            motionEnabled != other.motionEnabled || visible != other.visible) return false;
        // Framework reconciliation also invalidates paint for changed modifier
        // values. Frozen poses are equal by their artwork, not pending results
        // or callbacks; resume delivers the latest phase and callback together.
        if (!motionEnabled && visible) return true;
        return phase == other.phase && initialPose == other.initialPose &&
               completion == other.completion;
    }
};

class BoxMotion::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::ViewNode& node, const BoxMotion& value) { Update(node, value); }

    void Update(huxerui::ViewNode&, const BoxMotion& value) {
        const bool stableCapture = initialized_ && !staticFrame_ &&
            !spec_.motionEnabled && !value.motionEnabled && spec_.visible && value.visible &&
            spec_.PaintInputs() == value.PaintInputs();
        spec_ = value;
        if (stableCapture) {
            // A phase/result update is pending until playback resumes. The
            // frozen picture is unchanged; repainting it would invalidate the
            // SharedBounds snapshot and cut off the card's collapse/movement.
            SetLoopFlags(false, false, false);
            SyncProbe();
            return;
        }
        if (!initialized_) {
            initialized_ = true;
            // A returning card mounts a new button. Preserve its outgoing
            // pose while SharedBounds moves it, then reconcile to real state.
            phase_ = value.initialPose.value_or(value.phase);
            failureConsumed_ = phase_ == BoxCatPhase::Failed;
            handAllowed_ = phase_ == BoxCatPhase::Starting;
            SetProgress(PhaseProgress(phase_));
            StopIdle();
            StopSleep();
            StopLoading();
            if (value.visible && value.motionEnabled) ReconcilePhase(value.phase);
        } else if (!value.visible) {
            ReconcileStatic(value.phase);
        } else if (value.motionEnabled) {
            ReconcilePhase(value.phase);
        } else {
            SetLoopFlags(false, false, false);
        }
        if (phase_ != BoxCatPhase::Running || failurePlaying_) StopIdle();
        if (!handAllowed_) StopLoading();
        if (!IsSleepPose()) StopSleep();
        SyncProbe();
        InvalidatePaint();
    }

    FrameResult OnFrame(huxerui::ViewNode&, const huxerui::FrameInfo& frame) override {
        if (spec_.probe) {
            ++spec_.probe->frames;
            spec_.probe->visible = spec_.visible;
            spec_.probe->reducedFrame = frame.reduced_motion;
        }
        const bool staticPose = !spec_.visible || frame.reduced_motion;
        if (staticPose) {
            if (!staticFrame_ || phase_ != spec_.phase || transitioning_ || failurePlaying_ ||
                progress_ != PhaseProgress(spec_.phase)) {
                ReconcileStatic(spec_.phase);
                InvalidatePaint();
            }
            staticFrame_ = true;
            SetLoopFlags(false, false, false);
            SyncProbe();
            if (spec_.visible && spec_.motionEnabled && phase_ == BoxCatPhase::Running)
                NotifyRunningReady();
            return {};
        }
        staticFrame_ = false;
        if (!spec_.motionEnabled) {
            SetLoopFlags(false, false, false);
            SyncProbe();
            return {};
        }

        if (transitioning_) {
            transitionElapsed_ += frame.delta_time;
            const float t = static_cast<float>(std::clamp(
                transitionElapsed_ / transitionDuration_, 0.0, 1.0));
            SetProgress(std::lerp(transitionStart_, transitionTarget_, t));
            if (t >= 1.0F) {
                transitioning_ = false;
                if (failurePlaying_) {
                    failurePlaying_ = false;
                    failureConsumed_ = true;
                    SetProgress(0.0F);
                    if (spec_.probe) ++spec_.probe->failurePlays;
                } else {
                    SetProgress(transitionTarget_);
                }
            }
            InvalidatePaint();
        }

        const bool loading = handAllowed_ && progress_ < kCatRiseEnd;
        const bool running = phase_ == BoxCatPhase::Running && !failurePlaying_ &&
            !transitioning_ && progress_ >= 0.9999F;
        const bool sleeping = IsSleepPose();
        SetLoopFlags(running, sleeping, loading);

        FrameResult result{.needs_frame = transitioning_};
        if (loading) {
            if (!loadingActive_) {
                loadingActive_ = true;
                loadingCountdown_ = 0.0;
            }
            loadingSeconds_ += frame.delta_time;
            loadingCountdown_ -= frame.delta_time;
            if (loadingCountdown_ <= 0.00001) {
                loadingCountdown_ = kLoopFrameInterval;
                InvalidatePaint();
                if (spec_.probe) ++spec_.probe->loadingTicks;
            }
            result.wake_after = std::max(0.001, loadingCountdown_);
        } else if (loadingActive_) {
            StopLoading();
            InvalidatePaint();
        }

        if (running) {
            if (!idleActive_) {
                idleActive_ = true;
                idleCountdown_ = 0.0;
            }
            idleSeconds_ += frame.delta_time;
            idleCountdown_ -= frame.delta_time;
            if (idleCountdown_ <= 0.00001) {
                idleCountdown_ = kLoopFrameInterval;
                idlePose_ = RunningPose(idleSeconds_);
                InvalidatePaint();
                if (spec_.probe) {
                    ++spec_.probe->ticks;
                    spec_.probe->sawEars |= std::abs(idlePose_.leftEar) > 0.001F ||
                        std::abs(idlePose_.rightEar) > 0.001F;
                    spec_.probe->sawTail |= std::abs(idlePose_.tail) > 0.001F;
                }
            }
            result.wake_after = std::max(0.001, idleCountdown_);
        } else if (idleActive_) {
            StopIdle();
            InvalidatePaint();
        }

        if (sleeping) {
            if (!sleepActive_) {
                sleepActive_ = true;
                sleepCountdown_ = 0.0;
            }
            sleepSeconds_ += frame.delta_time;
            sleepCountdown_ -= frame.delta_time;
            if (sleepCountdown_ <= 0.00001) {
                sleepCountdown_ = kLoopFrameInterval;
                InvalidatePaint();
                if (spec_.probe) ++spec_.probe->sleepTicks;
            }
            result.wake_after = std::max(0.001, sleepCountdown_);
        } else if (sleepActive_) {
            StopSleep();
            InvalidatePaint();
        }
        SyncProbe();
        if (running) NotifyRunningReady();
        return result;
    }

    void PaintAboveContent(const huxerui::ViewNode& node, huxerui::PaintContext& paint) const override {
        const auto bounds = node.Bounds();
        const float side = std::min(bounds.width, bounds.height);
        PaintMark(paint, {(bounds.width - side) / 2, (bounds.height - side) / 2, side, side});
    }

private:
    void NotifyRunningReady() {
        if (runningReadySent_ || !spec_.completion) return;
        runningReadySent_ = true;
        const std::weak_ptr<std::uint64_t> gate = readyRevision_;
        const auto revision = *readyRevision_;
        // SharedBounds captures and State updates must run after this frame.
        // Retiring the node or changing phase invalidates a queued completion.
        spec_.completion->tasks.Post([gate, revision, complete = spec_.completion->callback] {
            const auto current = gate.lock();
            if (current && *current == revision) complete();
        });
    }

    void SetProgress(float value) {
        progress_ = value;
        if (spec_.probe) spec_.probe->progress = value;
    }

    void SyncProbe() {
        if (!spec_.probe) return;
        spec_.probe->progress = progress_;
        spec_.probe->transitioning = transitioning_;
        spec_.probe->failurePlaying = failurePlaying_;
    }

    void SetLoopFlags(bool running, bool sleeping, bool loading) {
        if (!spec_.probe) return;
        spec_.probe->looping = running;
        spec_.probe->sleepLooping = sleeping;
        spec_.probe->loadingLooping = loading;
    }

    bool IsSleepPose() const {
        return !transitioning_ && !failurePlaying_ && progress_ <= 0.0001F;
    }

    void StartTransition(float target, double duration, bool scale = true) {
        transitionStart_ = progress_;
        transitionTarget_ = target;
        transitionDuration_ = std::max(0.05,
            scale ? duration * std::abs(target - progress_) : duration);
        transitionElapsed_ = 0.0;
        transitioning_ = true;
    }

    void ReconcilePhase(BoxCatPhase next) {
        if (next == phase_ || (next == BoxCatPhase::Failed && failureConsumed_)) return;
        const BoxCatPhase previous = phase_;
        ++*readyRevision_;
        runningReadySent_ = false;
        phase_ = next;
        if (next == BoxCatPhase::Failed) {
            failurePlaying_ = true;
            failureConsumed_ = false;
            handAllowed_ = false;
            if (progress_ >= 0.9999F) SetProgress(0.0F);
            StartTransition(1.0F, kFailureDuration, false);
        } else {
            failurePlaying_ = false;
            failureConsumed_ = false;
            handAllowed_ = next == BoxCatPhase::Starting ||
                (next == BoxCatPhase::Running &&
                    (previous == BoxCatPhase::Starting || progress_ < kCatRiseEnd));
            StartTransition(PhaseProgress(next), kTransitionDuration);
        }
    }

    void ReconcileStatic(BoxCatPhase next) {
        if (phase_ != next) {
            ++*readyRevision_;
            runningReadySent_ = false;
        }
        phase_ = next;
        transitioning_ = false;
        failurePlaying_ = false;
        failureConsumed_ = next == BoxCatPhase::Failed;
        handAllowed_ = next == BoxCatPhase::Starting;
        SetProgress(PhaseProgress(next));
        StopIdle();
        StopSleep();
        StopLoading();
    }

    void StopLoading() {
        loadingActive_ = false;
        loadingSeconds_ = 0;
        loadingCountdown_ = 0;
        if (spec_.probe) spec_.probe->loadingLooping = false;
    }

    void StopSleep() {
        sleepActive_ = false;
        sleepSeconds_ = 0;
        sleepCountdown_ = 0;
        if (spec_.probe) spec_.probe->sleepLooping = false;
    }

    void StopIdle() {
        idleActive_ = false;
        idleSeconds_ = 0;
        idleCountdown_ = 0;
        idlePose_ = {};
        if (spec_.probe) spec_.probe->looping = false;
    }

    void PaintRunning(huxerui::PaintContext& paint) const {
        const huxerui::Rect full{0, 0, 128, 128};
        const auto hinged = [&](huxerui::VectorAsset asset, huxerui::Point pivot, float angle) {
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            paint.PushTransform(huxerui::Transform2D{.m11 = c, .m12 = s, .m21 = -s, .m22 = c,
                .translate_x = pivot.x - c * pivot.x + s * pivot.y,
                .translate_y = pivot.y - s * pivot.x - c * pivot.y});
            paint.DrawImage(asset, full);
            paint.PopTransform();
        };
        paint.DrawImage(spec_.rear, full);
        paint.DrawImage(spec_.interior, full);
        hinged(spec_.tail, {92, 74}, idlePose_.tail);
        hinged(spec_.earLeft, {47, 40}, idlePose_.leftEar);
        hinged(spec_.earRight, {80, 40}, idlePose_.rightEar);
        paint.DrawImage(spec_.catRunning, full);
        paint.DrawImage(spec_.front, full);
    }

    void PaintPeek(huxerui::PaintContext& paint, float p, float cat, float alpha) const {
        const float wink = Segment(p, 0.52F, 0.56F) * (1 - Segment(p, 0.64F, 0.68F));
        const float tilt = -0.09F * Segment(p, 0.40F, 0.52F) * (1 - Segment(p, 0.68F, 0.82F));
        const float c = std::cos(tilt);
        const float s = std::sin(tilt);
        paint.PushTransform(huxerui::Transform2D{.m11 = c, .m12 = s, .m21 = -s, .m22 = c,
            .translate_x = 64 - c * 64 + s * 76,
            .translate_y = 12 * (1 - cat) + 76 - s * 64 - c * 76});
        const huxerui::Rect full{0, 0, 128, 128};
        paint.DrawImage(spec_.catHead, full);
        paint.DrawImage(spec_.eyesSleeping, full, {}, 1 - alpha);
        paint.DrawImage(spec_.eyeLeft, full, {}, alpha);
        const float eyeHeight = 1 - 0.92F * wink;
        paint.PushTransform(huxerui::Transform2D{.m22 = eyeHeight, .translate_y = 54 * (1 - eyeHeight)});
        paint.DrawImage(spec_.eyeOpen, full, {}, alpha * (1 - wink));
        paint.PopTransform();
        paint.DrawImage(spec_.eyeWink, full, {}, alpha * wink);
        paint.PopTransform();
        if (spec_.probe && wink > 0.5F) spec_.probe->sawWink = true;
    }

    void PaintContentCat(huxerui::PaintContext& paint, float headY, float sway, float alpha) const {
        const float angle = -0.035F * sway;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        paint.PushTransform(huxerui::Transform2D{.m11 = c, .m12 = s, .m21 = -s, .m22 = c,
            .translate_x = 64 - c * 64 + s * 76,
            .translate_y = headY + 76 - s * 64 - c * 76});
        const huxerui::Rect full{0, 0, 128, 128};
        paint.DrawImage(spec_.catHead, full);
        paint.DrawImage(spec_.eyeLeft, full, {}, 1 - alpha);
        paint.DrawImage(spec_.eyeOpen, full, {}, 1 - alpha);
        paint.DrawImage(spec_.eyesContent, full, {}, alpha);
        paint.PopTransform();
        if (spec_.probe && alpha > 0) spec_.probe->sawContent = true;
    }

    void PaintHand(huxerui::PaintContext& paint, float p, float headY, float sway) const {
        const float alpha = handAllowed_ ? 1 - Segment(p, 0.55F, kCatRiseEnd) : 0;
        if (alpha <= 0) return;
        const float angle = 0.07F * sway;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        paint.PushTransform(huxerui::Transform2D{.m11 = c, .m12 = s, .m21 = -s, .m22 = c,
            .translate_x = 64 - c * 64 + s * 43 + 8 * sway,
            .translate_y = headY + 45.5F - s * 64 - c * 43 - 5 * (1 - alpha)});
        paint.DrawImage(spec_.handFill, {0, 0, 128, 128}, {}, alpha);
        paint.DrawImage(spec_.hand, {0, 0, 128, 128}, spec_.foreground, alpha);
        paint.PopTransform();
        if (spec_.probe && std::abs(sway) > 0.05F) spec_.probe->sawPetting = true;
        if (spec_.probe) spec_.probe->handVisible = true;
    }

    void PaintSleepSymbols(huxerui::PaintContext& paint) const {
        if (!IsSleepPose()) return;
        for (int i = 0; i < 3; ++i) {
            const float phase = static_cast<float>(std::fmod(sleepSeconds_ / 2.8 + 0.08 + i / 3.0, 1.0));
            const float alpha = Segment(phase, 0, 0.10F) * (1 - Segment(phase, 0.78F, 1.0F));
            const auto font = huxerui::Font::System(7 + 10 * phase).WithWeight(huxerui::FontWeight::SemiBold);
            paint.DrawText({81 + 22 * phase, 36 - 27 * phase, 18, 22}, "z",
                huxerui::TextStyle{font, Alpha(spec_.primary, 0.8F * alpha)});
        }
    }

    void PaintMark(huxerui::PaintContext& paint, huxerui::Rect rect) const {
        const float p = progress_;
        if (spec_.probe) spec_.probe->handVisible = false;
        const float petting = handAllowed_ ? 1 - Segment(p, 0.55F, kCatRiseEnd) : 0;
        const float wake = failurePlaying_
            ? Segment(p, 0.31F, 0.50F) * (1 - Segment(p, 0.71F, 0.87F))
            : Segment(p, 0.30F, kCatRiseEnd);
        const float eyes = failurePlaying_
            ? Segment(p, 0.31F, 0.43F) * (1 - Segment(p, 0.77F, 0.87F))
            : Segment(p, 0.30F, 0.46F);
        const float tail = failurePlaying_ ? 0 : Segment(p, 0.48F, 0.72F);
        paint.PushTransform(huxerui::Transform2D{.m11 = rect.width / 128, .m22 = rect.height / 128,
            .translate_x = rect.x, .translate_y = rect.y - 6.5F * rect.height / 128});
        const huxerui::Rect full{0, 0, 128, 128};
        if (!failurePlaying_ && petting <= 0 && p >= 0.7199F) {
            if (idleActive_ && phase_ == BoxCatPhase::Running && spec_.visible && !staticFrame_)
                PaintRunning(paint);
            else
                paint.DrawImage(spec_.logo, full);
            paint.PopTransform();
            return;
        }
        paint.DrawImage(spec_.rear, full);
        paint.DrawImage(spec_.interior, full);
        paint.DrawImage(spec_.tail, {0, 36 * (1 - tail), 128, 128}, {}, tail);
        const float sway = static_cast<float>(std::sin(6.2831853 * loadingSeconds_ / 1.8));
        const float headY = 12 * (1 - wake) + 1.4F * (0.5F + 0.5F * sway) * petting;
        if (failurePlaying_) {
            paint.PushClip({0, 0, 128, 80});
            PaintPeek(paint, p, wake, eyes);
            paint.PopClip();
        } else if (handAllowed_) {
            PaintContentCat(paint, headY, sway * petting, petting);
        } else {
            const huxerui::Rect head{0, 12 * (1 - wake), 128, 128};
            paint.DrawImage(spec_.catHead, head);
            paint.DrawImage(spec_.eyesSleeping, head, {}, 1 - eyes);
            paint.DrawImage(spec_.eyeLeft, head, {}, eyes);
            paint.DrawImage(spec_.eyeOpen, head, {}, eyes);
        }
        paint.DrawImage(spec_.front, full);
        PaintSleepSymbols(paint);
        PaintHand(paint, p, headY, sway);
        if (spec_.probe && handAllowed_ && p >= kCatRiseEnd &&
            !spec_.probe->handVisible && !spec_.probe->loadingLooping)
            spec_.probe->sawHandRetiredAfterRise = true;
        if (spec_.probe && eyes < 0.001F) spec_.probe->sawSleep = true;
        paint.PopTransform();
    }

    BoxMotion spec_;
    std::shared_ptr<std::uint64_t> readyRevision_ = std::make_shared<std::uint64_t>(0);
    bool runningReadySent_ = false;
    BoxCatPhase phase_ = BoxCatPhase::Sleeping;
    bool initialized_ = false;
    bool staticFrame_ = false;
    bool transitioning_ = false;
    bool failurePlaying_ = false;
    bool failureConsumed_ = false;
    bool handAllowed_ = false;
    float progress_ = 0.0F;
    float transitionStart_ = 0.0F;
    float transitionTarget_ = 0.0F;
    double transitionDuration_ = 0.0;
    double transitionElapsed_ = 0.0;
    IdlePose idlePose_;
    bool idleActive_ = false;
    double idleSeconds_ = 0;
    double idleCountdown_ = 0;
    bool sleepActive_ = false;
    double sleepSeconds_ = 0;
    double sleepCountdown_ = 0;
    bool loadingActive_ = false;
    double loadingSeconds_ = 0;
    double loadingCountdown_ = 0;
};

} // namespace

[[huxerui::composable]] huxerui::View BoxCatAnimation(
    BoxCatPhase phase, bool motionEnabled, std::shared_ptr<BoxCatProbe> probe,
    std::optional<huxerui::Color> symbolColor, std::function<void()> onRunningReady,
    std::optional<BoxCatPhase> initialPose) {
    const auto theme = huxerui::UseTheme();
    const auto tasks = huxerui::UseTaskScope();
    const bool visible = huxerui::UseWindow().LifecycleState() != huxerui::WindowLifecycleState::Background;
    return huxerui::Stack {}.With(BoxMotion{
        .catHead = huxerui::UseVectorImage(app::images::box_cat_cat_head),
        .tail = huxerui::UseVectorImage(app::images::box_cat_tail),
        .rear = huxerui::UseVectorImage(app::images::box_cat_rear),
        .front = huxerui::UseVectorImage(app::images::box_cat_front),
        .logo = huxerui::UseVectorImage(app::images::box_cat_logo_completed),
        .eyeLeft = huxerui::UseVectorImage(app::images::box_cat_eye_left),
        .eyeOpen = huxerui::UseVectorImage(app::images::box_cat_eye_open),
        .eyeWink = huxerui::UseVectorImage(app::images::box_cat_eye_wink),
        .catRunning = huxerui::UseVectorImage(app::images::box_cat_cat_running),
        .earLeft = huxerui::UseVectorImage(app::images::box_cat_ear_left),
        .earRight = huxerui::UseVectorImage(app::images::box_cat_ear_right),
        .interior = huxerui::UseVectorImage(app::images::box_cat_interior),
        .eyesSleeping = huxerui::UseVectorImage(app::images::box_cat_eyes_sleeping),
        .eyesContent = huxerui::UseVectorImage(app::images::box_cat_eyes_content),
        .hand = huxerui::UseVectorImage(app::images::box_cat_petting_hand),
        .handFill = huxerui::UseVectorImage(app::images::box_cat_petting_hand_fill),
        .probe = std::move(probe),
        .completion = onRunningReady ? std::make_shared<BoxCatCompletion>(
            BoxCatCompletion{tasks, std::move(onRunningReady)}) : nullptr,
        .phase = phase,
        .initialPose = initialPose,
        .motionEnabled = motionEnabled,
        .visible = visible,
        .primary = symbolColor.value_or(theme.colors.on_surface),
        .foreground = IsDarkTheme(theme) ? theme.colors.on_surface : theme.colors.surface});
}

[[huxerui::composable]] huxerui::View BoxCatButtonContent(
    BoxCatPhase phase, huxerui::Color symbolColor, bool motionEnabled,
    std::shared_ptr<BoxCatProbe> probe, std::function<void()> onRunningReady,
    std::optional<BoxCatPhase> initialPose) {
    return huxerui::Stack {
        BoxCatAnimation(phase, motionEnabled, std::move(probe), symbolColor,
                        std::move(onRunningReady), initialPose)
            .With(huxerui::Frame{.width = 48.0F, .height = 48.0F})
            .Key("box-cat-artwork"),
    }.With(huxerui::Frame{.width = 56.0F, .height = 56.0F},
           huxerui::Align(huxerui::HorizontalAlignment::Center,
                          huxerui::VerticalAlignment::Center));
}

[[huxerui::composable]] huxerui::View BoxCatStatusIcon(
    BoxCatPhase phase, bool motionEnabled, std::shared_ptr<BoxCatProbe> probe) {
    const auto theme = huxerui::UseTheme();
    return huxerui::Stack {
        BoxCatAnimation(phase, motionEnabled, std::move(probe), theme.colors.on_surface)
            // The paint-only Stack has no intrinsic size. Center alignment
            // measures children loosely, so the badge's frame cannot size it.
            .With(huxerui::Frame{.width = 36.0F, .height = 36.0F})
            .Key("box-cat-artwork"),
    }.With(huxerui::Frame{.width = 36.0F, .height = 36.0F},
           huxerui::Align(huxerui::HorizontalAlignment::Center,
                          huxerui::VerticalAlignment::Center));
}

} // namespace clashflux::ui
