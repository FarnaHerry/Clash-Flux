#include <huxerui/huxerui.h>
#include "app_resources.h"
#include "cat_tricks.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>

using namespace huxerui;
using namespace std::chrono_literals;

namespace boxcat {

bool smoke = false;
bool smokePassed = false;
float preview = 0.0F;
bool previewFailure = false;
bool brandPreview = false;
bool tricksPreview = false;
constexpr double kFailureMotionDuration = 2.4;
constexpr double kIdleFrameInterval = 1.0 / 30.0;
constexpr float kCatRiseEnd = 0.63F;
constexpr const char* kSleepStatus = "睡觉中 · 点击猫咪唤醒";

float Segment(float value, float start, float end) {
  const float t = std::clamp((value - start) / (end - start), 0.0F, 1.0F);
  return t * t * (3.0F - 2.0F * t);
}

Color Alpha(Color color, float alpha) {
  color.alpha *= alpha;
  return color;
}

struct IdlePose {
  float leftEar = 0;
  float rightEar = 0;
  float tail = 0;
};

// Optional UI-thread-only instrumentation for the real retained animation.
// It is never observable State and cannot trigger a composition.
struct MotionProbe {
  std::uint64_t ticks = 0;
  bool looping = false;
  bool sawEars = false;
  bool sawTail = false;
  bool sawSleep = false;
  bool sawPetting = false;
  bool sawContent = false;
  std::uint64_t sleepTicks = 0;
  bool sleepLooping = false;
  bool handVisible = false;
  std::uint64_t loadingTicks = 0;
  bool loadingLooping = false;
  bool sawHandRetiredAfterRise = false;
};

IdlePose RunningPose(double seconds) {
  constexpr double pi = 3.141592653589793;
  const double phase = std::fmod(seconds, 5.6);
  const auto twitch = [phase](double start) {
    const double t = (phase - start) / 0.65;
    return t <= 0 || t >= 1 ? 0.0F : static_cast<float>(std::sin(2 * pi * t) * std::sin(pi * t));
  };
  const float settle = Segment(static_cast<float>(seconds), 0, 0.4F);
  return {.leftEar = -0.06F * twitch(0.75),
          .rightEar = 0.05F * twitch(1.05),
          .tail = settle * static_cast<float>(0.085 * std::sin(2 * pi * seconds / 3.8) +
                                              0.015 * std::sin(4 * pi * seconds / 3.8))};
}

// The carton stays open. The hand pets during the wait and rising pose,
// then retires as soon as the cat is upright, before the card expands.
struct BoxMotion {
  class Extension;
  VectorAsset catHead, tail, rear, front, logo, eyeLeft, eyeOpen, eyeWink;
  VectorAsset catRunning, earLeft, earRight;
  VectorAsset interior, eyesSleeping, eyesContent, hand, handFill;
  std::shared_ptr<MotionProbe> probe;
  float target = 0.0F;
  double duration = 1.2;
  bool immediate = false;
  bool reduced = false;
  bool control = false;
  bool failure = false;
  bool running = false;
  bool sleeping = false;
  bool loading = false;
  Color surface, outline, primary, foreground, muted;
};

class BoxMotion::Extension final : public NodeExtension {
public:
  Extension(ViewNode& node, const BoxMotion& value) { Update(node, value); }

  void Update(ViewNode&, const BoxMotion& value) {
    if (value.immediate || value.reduced) progress_.Set(value.target);
    else if (progress_.Target() != value.target) {
      progress_.AnimateTo(value.target, TweenSpec{
          .duration = std::max(0.05, value.duration * std::abs(value.target - progress_.Value())),
          .easing = Easing::Linear});
    }
    spec_ = value;
    if (!value.running || value.reduced || value.failure) StopIdle();
    if (!value.sleeping || value.reduced) StopSleep();
    if (!value.loading || value.reduced) StopLoading();
    InvalidatePaint();
  }

  FrameResult OnFrame(ViewNode&, const FrameInfo& frame) override {
    const auto result = progress_.Advance(frame);
    if (result.changed) InvalidatePaint();
    const bool sleeping = spec_.sleeping && !spec_.reduced && !frame.reduced_motion &&
        !progress_.IsRunning() && progress_.Value() <= 0.0001F;
    if (spec_.probe) spec_.probe->sleepLooping = sleeping;
    if (!sleeping && sleepActive_) { StopSleep(); InvalidatePaint(); }
    const bool loading = spec_.loading && progress_.Value() < kCatRiseEnd &&
        !spec_.reduced && !frame.reduced_motion;
    if (spec_.probe) spec_.probe->loadingLooping = loading;
    if (!loading && loadingActive_) { StopLoading(); InvalidatePaint(); }
    if (loading) {
      if (!loadingActive_) {
        loadingActive_ = true;
        loadingStart_ = frame.timestamp;
        lastLoadingFrame_ = frame.timestamp - kIdleFrameInterval;
      }
      const double elapsed = frame.timestamp - lastLoadingFrame_;
      if (elapsed + 0.00001 >= kIdleFrameInterval) {
        loadingSeconds_ = frame.timestamp - loadingStart_;
        lastLoadingFrame_ = frame.timestamp;
        InvalidatePaint();
        if (spec_.probe) ++spec_.probe->loadingTicks;
        return {.needs_frame = result.needs_frame, .wake_after = kIdleFrameInterval};
      }
      return {.needs_frame = result.needs_frame,
              .wake_after = std::max(0.001, kIdleFrameInterval - elapsed)};
    }
    const bool running = spec_.running && !spec_.failure && !spec_.reduced &&
        !frame.reduced_motion && !progress_.IsRunning() && progress_.Value() >= 0.9999F;
    if (spec_.probe) spec_.probe->looping = running;
    if (running) {
      if (!idleActive_) {
        idleActive_ = true;
        idleStart_ = frame.timestamp;
        lastIdleFrame_ = frame.timestamp - kIdleFrameInterval;
      }
      const double elapsed = frame.timestamp - lastIdleFrame_;
      if (elapsed + 0.00001 >= kIdleFrameInterval) {
        idlePose_ = RunningPose(frame.timestamp - idleStart_);
        lastIdleFrame_ = frame.timestamp;
        InvalidatePaint();
        if (spec_.probe) {
          ++spec_.probe->ticks;
          spec_.probe->sawEars |= std::abs(idlePose_.leftEar) > 0.001F || std::abs(idlePose_.rightEar) > 0.001F;
          spec_.probe->sawTail |= std::abs(idlePose_.tail) > 0.001F;
        }
        return {.wake_after = kIdleFrameInterval};
      }
      return {.wake_after = std::max(0.001, kIdleFrameInterval - elapsed)};
    }
    if (idleActive_) { StopIdle(); InvalidatePaint(); }
    if (sleeping) {
      if (!sleepActive_) {
        sleepActive_ = true;
        sleepStart_ = frame.timestamp;
        lastSleepFrame_ = frame.timestamp - kIdleFrameInterval;
      }
      const double elapsed = frame.timestamp - lastSleepFrame_;
      if (elapsed + 0.00001 >= kIdleFrameInterval) {
        sleepSeconds_ = frame.timestamp - sleepStart_;
        lastSleepFrame_ = frame.timestamp;
        InvalidatePaint();
        if (spec_.probe) ++spec_.probe->sleepTicks;
        return {.wake_after = kIdleFrameInterval};
      }
      return {.wake_after = std::max(0.001, kIdleFrameInterval - elapsed)};
    }
    return {.needs_frame = result.needs_frame, .wake_after = result.wake_after};
  }

  void PaintAboveContent(const ViewNode& node, PaintContext& paint) const override {
    const auto bounds = node.Bounds();
    const float p = progress_.Value();
    if (!spec_.control) {
      const float side = std::min(bounds.width, bounds.height);
      PaintMark(paint, {(bounds.width - side) / 2, (bounds.height - side) / 2, side, side}, p);
      return;
    }
    const float expand = spec_.failure ? 0.0F : Segment(p, 0.72F, 1.0F);
    const float width = 68 + (bounds.width - 68) * expand;
    const float height = 68 + (104 - 68) * expand;
    const Rect card{(bounds.width - width) / 2, (bounds.height - height) / 2, width, height};
    paint.DrawRect(card, spec_.surface, CornerRadii{20 - 6 * expand});
    paint.DrawBorder(card, spec_.outline, StrokeStyle{.width = 1}, CornerRadii{20 - 6 * expand});
    const float side = 54 - 10 * expand;
    const float markX = card.x + (width - side) / 2 - 53 * expand;
    const float markY = card.y + (height - side) / 2 - 19 * expand;
    PaintMark(paint, {markX, markY, side, side}, p);
    const float alpha = spec_.failure ? 0.0F : Segment(p, 0.84F, 1.0F);
    if (alpha <= 0.001F) return;
    const auto text = [&](Rect rect, const std::string& label, float size, Color color, bool bold = false) {
      auto font = Font::System(size);
      if (bold) font = font.WithWeight(FontWeight::SemiBold);
      paint.DrawText(rect, label, TextStyle{font, Alpha(color, alpha)},
                     {.align = TextAlign::Center, .vertical_align = TextVerticalAlign::Center});
    };
    text({markX + side + 4, card.y + 12, 102, 22}, spec_.loading ? "启动中" : "运行中", 14, spec_.foreground, true);
    text({markX + side + 4, card.y + 34, 102, 20}, spec_.loading ? "请稍候" : "00:15:39", 12, spec_.muted);
    const float cell = (width - 40) / 3;
    const std::array<std::string, 3> values{"128", "96", "32"};
    const std::array<std::string, 3> labels{"当前连接", "TCP", "UDP"};
    for (int i = 0; i < 3; ++i) {
      text({card.x + 20 + i * cell, card.y + 63, cell, 19}, values[i], 16, spec_.primary, true);
      text({card.x + 20 + i * cell, card.y + 83, cell, 16}, labels[i], 10, spec_.muted);
    }
  }

private:
  void StopLoading() {
    loadingActive_ = false;
    loadingSeconds_ = 0;
    if (spec_.probe) spec_.probe->loadingLooping = false;
  }

  void StopSleep() {
    sleepActive_ = false;
    sleepSeconds_ = 0;
    if (spec_.probe) spec_.probe->sleepLooping = false;
  }

  void StopIdle() {
    idleActive_ = false;
    idlePose_ = {};
    if (spec_.probe) spec_.probe->looping = false;
  }

  void PaintRunning(PaintContext& paint) const {
    const Rect full{0, 0, 128, 128};
    const auto hinged = [&](VectorAsset asset, Point pivot, float angle) {
      const float c = std::cos(angle);
      const float s = std::sin(angle);
      paint.PushTransform(Transform2D{.m11 = c, .m12 = s, .m21 = -s, .m22 = c,
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

  void PaintPeek(PaintContext& paint, float p, float cat, float alpha) const {
    const float wink = Segment(p, 0.52F, 0.56F) * (1 - Segment(p, 0.64F, 0.68F));
    const float tilt = -0.09F * Segment(p, 0.40F, 0.52F) * (1 - Segment(p, 0.68F, 0.82F));
    const float c = std::cos(tilt);
    const float s = std::sin(tilt);
    // Only the head peeks out. Its left eye stays open while the right eye
    // narrows into a curved wink, opens again, and disappears behind the box.
    paint.PushTransform(Transform2D{.m11 = c, .m12 = s, .m21 = -s, .m22 = c,
        .translate_x = 64 - c * 64 + s * 76,
        .translate_y = 12 * (1 - cat) + 76 - s * 64 - c * 76});
    const Rect full{0, 0, 128, 128};
    // A solid silhouette must occlude the box throughout expression changes.
    // Cross-fading two filled heads with source-over leaves the background visible.
    paint.DrawImage(spec_.catHead, full);
    paint.DrawImage(spec_.eyesSleeping, full, {}, 1 - alpha);
    paint.DrawImage(spec_.eyeLeft, full, {}, alpha);
    const float eyeHeight = 1 - 0.92F * wink;
    paint.PushTransform(Transform2D{.m22 = eyeHeight, .translate_y = 54 * (1 - eyeHeight)});
    paint.DrawImage(spec_.eyeOpen, full, {}, alpha * (1 - wink));
    paint.PopTransform();
    paint.DrawImage(spec_.eyeWink, full, {}, alpha * wink);
    paint.PopTransform();
  }

  void PaintContentCat(PaintContext& paint, float headY, float sway, float alpha) const {
    const float angle = -0.035F * sway;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    paint.PushTransform(Transform2D{.m11 = c, .m12 = s, .m21 = -s, .m22 = c,
        .translate_x = 64 - c * 64 + s * 76,
        .translate_y = headY + 76 - s * 64 - c * 76});
    const Rect full{0, 0, 128, 128};
    paint.DrawImage(spec_.catHead, full);
    paint.DrawImage(spec_.eyeLeft, full, {}, 1 - alpha);
    paint.DrawImage(spec_.eyeOpen, full, {}, 1 - alpha);
    paint.DrawImage(spec_.eyesContent, full, {}, alpha);
    paint.PopTransform();
    if (spec_.probe && alpha > 0) spec_.probe->sawContent = true;
  }

  void PaintHand(PaintContext& paint, float p, float headY, float sway) const {
    const float alpha = spec_.loading ? 1 - Segment(p, 0.55F, kCatRiseEnd) : 0;
    if (alpha <= 0) return;
    const float angle = 0.07F * sway;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    paint.PushTransform(Transform2D{.m11 = c, .m12 = s, .m21 = -s, .m22 = c,
        .translate_x = 64 - c * 64 + s * 43 + 8 * sway,
        .translate_y = headY + 45.5F - s * 64 - c * 43 - 5 * (1 - alpha)});
    paint.DrawImage(spec_.handFill, {0, 0, 128, 128}, {}, alpha);
    paint.DrawImage(spec_.hand, {0, 0, 128, 128}, spec_.foreground, alpha);
    paint.PopTransform();
    if (spec_.probe && std::abs(sway) > 0.05F) spec_.probe->sawPetting = true;
    if (spec_.probe) spec_.probe->handVisible = true;
  }

  void PaintSleepSymbols(PaintContext& paint) const {
    if (!spec_.sleeping) return;
    for (int i = 0; i < 3; ++i) {
      const float phase = static_cast<float>(std::fmod(sleepSeconds_ / 2.8 + 0.08 + i / 3.0, 1.0));
      const float alpha = Segment(phase, 0, 0.10F) * (1 - Segment(phase, 0.78F, 1.0F));
      const auto font = Font::System(7 + 10 * phase).WithWeight(FontWeight::SemiBold);
      paint.DrawText({81 + 22 * phase, 36 - 27 * phase, 18, 22}, "z",
                     TextStyle{font, Alpha(spec_.primary, 0.8F * alpha)});
    }
  }

  void PaintMark(PaintContext& paint, Rect rect, float p) const {
    if (spec_.probe) spec_.probe->handVisible = false;
    const float petting = spec_.loading ? 1 - Segment(p, 0.55F, kCatRiseEnd) : 0;
    const float wake = spec_.failure
        ? Segment(p, 0.31F, 0.50F) * (1 - Segment(p, 0.71F, 0.87F))
        : Segment(p, 0.30F, kCatRiseEnd);
    const float eyes = spec_.failure
        ? Segment(p, 0.31F, 0.43F) * (1 - Segment(p, 0.77F, 0.87F))
        : Segment(p, 0.30F, 0.46F);
    const float tail = spec_.failure ? 0 : Segment(p, 0.48F, 0.72F);
    // Use the running logo's fixed baseline for every pose. Neither the box
    // nor its four already-open covers move when the cat wakes or sleeps.
    paint.PushTransform(Transform2D{.m11 = rect.width / 128, .m22 = rect.height / 128,
        .translate_x = rect.x, .translate_y = rect.y - 6.5F * rect.height / 128});
    const Rect full{0, 0, 128, 128};
    if (!spec_.failure && petting <= 0 && p >= 0.7199F) {
      if (idleActive_ && spec_.running && !spec_.reduced) PaintRunning(paint);
      else paint.DrawImage(spec_.logo, full);
      paint.PopTransform();
      return;
    }
    paint.DrawImage(spec_.rear, full);
    paint.DrawImage(spec_.interior, full);
    paint.DrawImage(spec_.tail, {0, 36 * (1 - tail), 128, 128}, {}, tail);
    const float sway = static_cast<float>(std::sin(6.2831853 * loadingSeconds_ / 1.8));
    const float headY = 12 * (1 - wake) + 1.4F * (0.5F + 0.5F * sway) * petting;
    if (spec_.failure) {
      // Draw a single opaque head even at the first/last partial eye transition.
      // The foreground covers are painted afterwards and hide the returning head.
      paint.PushClip({0, 0, 128, 80});
      PaintPeek(paint, p, wake, eyes);
      paint.PopClip();
    } else if (spec_.loading) {
      PaintContentCat(paint, headY, sway * petting, petting);
    } else {
      const Rect head{0, 12 * (1 - wake), 128, 128};
      paint.DrawImage(spec_.catHead, head);
      paint.DrawImage(spec_.eyesSleeping, head, {}, 1 - eyes);
      paint.DrawImage(spec_.eyeLeft, head, {}, eyes);
      paint.DrawImage(spec_.eyeOpen, head, {}, eyes);
    }
    paint.DrawImage(spec_.front, full);
    PaintSleepSymbols(paint);
    PaintHand(paint, p, headY, sway);
    if (spec_.probe && spec_.loading && p >= kCatRiseEnd &&
        !spec_.probe->handVisible && !spec_.probe->loadingLooping)
      spec_.probe->sawHandRetiredAfterRise = true;
    if (spec_.probe && eyes < 0.001F) spec_.probe->sawSleep = true;
    paint.PopTransform();
  }

  BoxMotion spec_;
  MotionController progress_{0.0F};
  IdlePose idlePose_;
  bool idleActive_ = false;
  double idleStart_ = 0;
  double lastIdleFrame_ = 0;
  bool sleepActive_ = false;
  double sleepStart_ = 0;
  double lastSleepFrame_ = 0;
  double sleepSeconds_ = 0;
  bool loadingActive_ = false;
  double loadingStart_ = 0;
  double lastLoadingFrame_ = 0;
  double loadingSeconds_ = 0;
};

struct LabState {
  float target = 0;
  bool immediate = true;
  bool busy = false;
  bool reduced = false;
  bool failure = false;
  std::uint64_t serial = 0;
  std::string status = kSleepStatus;
};

View Label(std::string value, float size, Color color, bool bold = false) {
  auto font = Font::System(size);
  if (bold) font = font.WithWeight(FontWeight::SemiBold);
  return Text(std::move(value)).Style(TextStyle{font, color});
}

void SetTarget(State<LabState> state, float target, bool immediate, std::string status, bool busy) {
  state.Update([&](LabState& next) {
    next.target = target;
    next.immediate = immediate;
    next.status = std::move(status);
    next.busy = busy;
  });
}

void Reset(State<LabState> state) {
  state.Update([](LabState& next) {
    ++next.serial;
    next.target = 0;
    next.immediate = true;
    next.busy = false;
    next.failure = false;
    next.status = kSleepStatus;
  });
}

// Finite simulated transactions; reset, scrub and stop invalidate older work.
void Start(TaskScope tasks, State<LabState> state, float speed, bool slow, bool fail) {
  const auto serial = state.Get().serial + 1;
  state.Update([&](LabState& next) { next.serial = serial; next.failure = false; });
  SetTarget(state, 0, true, "准备唤醒", true);
  tasks.Launch([state, serial, speed, slow, fail]() -> Task<void> {
    co_await Delay(40ms);
    if (state.Get().serial != serial) co_return;
    SetTarget(state, 0.30F, false, "轻抚猫咪 · 等待模拟结果", true);
    co_await Delay(std::chrono::duration<double>{(slow ? 3.0 : 0.65) / speed});
    if (state.Get().serial != serial) co_return;
    if (fail) {
      state.Update([](LabState& next) {
        next.failure = true;
        next.target = 1;
        next.immediate = false;
        next.status = "启动失败 · 猫咪探头 wink 后回箱";
      });
      if (!state.Get().reduced)
        co_await Delay(std::chrono::duration<double>{2.0 / speed});
      if (state.Get().serial != serial) co_return;
      state.Update([](LabState& next) {
        next.target = 0;
        next.immediate = true;
        next.failure = false;
        next.busy = false;
        next.status = "唤醒失败 · 点击猫咪重试";
      });
      co_return;
    }
    SetTarget(state, 1, false, "正在启动 · 猫咪探头并展开", true);
    co_await Delay(std::chrono::duration<double>{1.3 / speed});
    if (state.Get().serial != serial) co_return;
    state.Update([&](LabState& next) {
      next.busy = false;
      next.status = "运行中 · 点击卡片停止";
    });
  });
}

void Stop(TaskScope tasks, State<LabState> state, float speed) {
  const bool failure = state.Get().failure;
  const auto serial = state.Get().serial + 1;
  state.Update([&](LabState& next) { next.serial = serial; });
  SetTarget(state, failure ? 1 : 0, false,
            failure ? "正在停止 · 猫咪回去睡觉" : "正在停止 · 猫咪回箱", true);
  tasks.Launch([state, serial, speed, failure]() -> Task<void> {
    co_await Delay(std::chrono::duration<double>{(failure ? 2.0 : 1.3) / speed});
    if (state.Get().serial != serial) co_return;
    state.Update([failure](LabState& next) {
      if (failure) { next.target = 0; next.immediate = true; }
      next.busy = false;
      next.failure = false;
      next.status = "已停止";
    });
  });
}

Task<void> WaitSettled(State<LabState> state, float target, const char* step) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (state.Get().busy && std::chrono::steady_clock::now() < deadline)
    co_await Delay(25ms);
  if (state.Get().busy || state.Get().target != target)
    throw std::runtime_error(std::string{"demo smoke failed: "} + step +
        " (busy=" + std::to_string(state.Get().busy) +
        ", target=" + std::to_string(state.Get().target) +
        ", status=" + state.Get().status + ")");
}

Task<void> WaitFailurePeek(State<LabState> state) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!state.Get().failure && state.Get().busy && std::chrono::steady_clock::now() < deadline)
    co_await Delay(25ms);
  if (!state.Get().failure || !state.Get().busy || state.Get().target != 1)
    throw std::runtime_error("demo smoke failed: failure peek was not presented");
}

[[huxerui::composable]] View Lab() {
  const auto theme = UseTheme();
  const auto tasks = UseTaskScope();
  const auto application = UseApplication();
  const bool visible = UseWindow().LifecycleState() != WindowLifecycleState::Background;
  auto motionProbe = UseState(std::make_shared<MotionProbe>());
  auto cardMotionProbe = UseState(std::make_shared<MotionProbe>());
  auto state = UseState(LabState{.target = preview, .failure = previewFailure,
                               .status = preview > 0 ? "静态进度预览" : kSleepStatus});
  auto speed = UseState(1.0F);
  auto scrub = UseState(preview);
  const auto current = state.Get();
  BoxMotion mark{
      .catHead = UseVectorImage(app::images::cat_head), .tail = UseVectorImage(app::images::tail),
      .rear = UseVectorImage(app::images::rear), .front = UseVectorImage(app::images::front),
      .logo = UseVectorImage(app::images::logo_completed), .eyeLeft = UseVectorImage(app::images::eye_left),
      .eyeOpen = UseVectorImage(app::images::eye_open), .eyeWink = UseVectorImage(app::images::eye_wink),
      .catRunning = UseVectorImage(app::images::cat_running),
      .earLeft = UseVectorImage(app::images::ear_left), .earRight = UseVectorImage(app::images::ear_right),
      .interior = UseVectorImage(app::images::interior),
      .eyesSleeping = UseVectorImage(app::images::eyes_sleeping),
      .eyesContent = UseVectorImage(app::images::eyes_content), .hand = UseVectorImage(app::images::petting_hand),
      .handFill = UseVectorImage(app::images::petting_hand_fill),
      .probe = smoke ? motionProbe.Get() : nullptr,
      .target = current.target,
      .duration = (current.failure ? kFailureMotionDuration : 1.2) / speed.Get(),
      .immediate = current.immediate, .reduced = current.reduced, .control = false,
      .failure = current.failure, .running = visible && !current.failure && !current.busy && current.target == 1,
      .sleeping = visible && !current.failure && !current.busy && current.target == 0,
      .loading = visible && !current.failure && current.target > 0 &&
          (current.busy || (current.immediate && current.target < 1)),
      .surface = theme.colors.surface_container, .outline = theme.colors.outline,
      .primary = theme.colors.primary, .foreground = theme.colors.on_surface,
      .muted = theme.colors.on_surface_variant};
  BoxMotion card = mark;
  card.control = true;
  card.probe = smoke ? cardMotionProbe.Get() : nullptr;
  const auto toggle = [=] {
    if (state.Get().busy || (!state.Get().failure && state.Get().target > 0)) Stop(tasks, state, speed.Get());
    else Start(tasks, state, speed.Get(), false, false);
  };
  Lifecycle([=] {
    if (smoke) tasks.Launch([=]() -> Task<void> {
      try {
        const auto probe = motionProbe.Get();
        const auto cardProbe = cardMotionProbe.Get();
        co_await Delay(180ms);
        const auto sleepBefore = probe->sleepTicks;
        const auto cardSleepBefore = cardProbe->sleepTicks;
        const auto sleepStarted = std::chrono::steady_clock::now();
        co_await Delay(500ms);
        const double sleepElapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - sleepStarted).count();
        if (!probe->sleepLooping || !cardProbe->sleepLooping || probe->sleepTicks <= sleepBefore ||
            cardProbe->sleepTicks <= cardSleepBefore || probe->sleepTicks - sleepBefore > sleepElapsed * 32 + 3 ||
            cardProbe->sleepTicks - cardSleepBefore > sleepElapsed * 32 + 3 ||
            probe->handVisible || cardProbe->handVisible)
          throw std::runtime_error("sleep zzz did not animate within budget, or waiting hand was visible");
        state.Update([](LabState& next) { next.reduced = true; });
        co_await Delay(120ms);
        const auto sleepPaused = probe->sleepTicks;
        const auto cardSleepPaused = cardProbe->sleepTicks;
        co_await Delay(160ms);
        if (probe->sleepLooping || cardProbe->sleepLooping || probe->sleepTicks != sleepPaused ||
            cardProbe->sleepTicks != cardSleepPaused)
          throw std::runtime_error("sleep zzz did not pause for reduced motion");
        state.Update([](LabState& next) { next.reduced = false; });
        co_await Delay(120ms);
        if (!probe->sleepLooping || !cardProbe->sleepLooping || probe->sleepTicks == sleepPaused ||
            cardProbe->sleepTicks == cardSleepPaused)
          throw std::runtime_error("sleep zzz did not resume");
        Start(tasks, state, 1, false, false);
        co_await WaitSettled(state, 1, "startup");
        co_await Delay(80ms);
        if (!probe->sawHandRetiredAfterRise || !cardProbe->sawHandRetiredAfterRise)
          throw std::runtime_error("petting hand did not retire after rising while startup was still busy");
        if (probe->loadingLooping || cardProbe->loadingLooping)
          throw std::runtime_error("loading clock continued after startup completed");
        if (probe->sleepLooping || cardProbe->sleepLooping || probe->handVisible || cardProbe->handVisible)
          throw std::runtime_error("sleep zzz or hand remained after waking");
        if (!probe->sawSleep || !probe->sawPetting || !probe->sawContent ||
            !cardProbe->sawSleep || !cardProbe->sawPetting || !cardProbe->sawContent)
          throw std::runtime_error("hero/card sleeping pose, petting stroke or content expression was not presented");
        const auto before = probe->ticks;
        const auto cardBefore = cardProbe->ticks;
        const auto started = std::chrono::steady_clock::now();
        co_await Delay(1600ms);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (!probe->looping || !probe->sawEars || !probe->sawTail || probe->ticks <= before ||
            probe->ticks - before > elapsed * 32 + 3)
          throw std::runtime_error("running ears/tail animation did not advance within its frame budget");
        if (!cardProbe->looping || !cardProbe->sawEars || !cardProbe->sawTail || cardProbe->ticks <= cardBefore ||
            cardProbe->ticks - cardBefore > elapsed * 32 + 3)
          throw std::runtime_error("actual-card running animation did not advance within its frame budget");
        state.Update([](LabState& next) { next.reduced = true; });
        co_await Delay(180ms);
        const auto paused = probe->ticks;
        const auto cardPaused = cardProbe->ticks;
        co_await Delay(180ms);
        if (probe->looping || probe->ticks != paused || cardProbe->looping || cardProbe->ticks != cardPaused)
          throw std::runtime_error("running animation did not pause for reduced motion");
        state.Update([](LabState& next) { next.reduced = false; });
        co_await Delay(180ms);
        if (!probe->looping || probe->ticks == paused || !cardProbe->looping || cardProbe->ticks == cardPaused)
          throw std::runtime_error("running animation did not resume");
        Stop(tasks, state, 1);
        co_await WaitSettled(state, 0, "stop");
        if (probe->looping || cardProbe->looping) throw std::runtime_error("running animation continued after stop");
        co_await Delay(120ms);
        if (!probe->sleepLooping || !cardProbe->sleepLooping || probe->handVisible || cardProbe->handVisible)
          throw std::runtime_error("stop did not restore sleeping zzz without the hand");
        Start(tasks, state, 1, false, true);
        co_await WaitFailurePeek(state);
        co_await WaitSettled(state, 0, "failure");
        if (state.Get().failure) throw std::runtime_error("failure mode remained after falling asleep");
        // Retry a failed request through the same production start path.
        Start(tasks, state, 1, false, false);
        co_await WaitSettled(state, 1, "retry after failure");
        Stop(tasks, state, 1);
        co_await WaitSettled(state, 0, "stop after retry");
        Start(tasks, state, 1, false, true);
        co_await WaitFailurePeek(state);
        Stop(tasks, state, 1);
        co_await WaitSettled(state, 0, "stop during failure peek");
        if (state.Get().failure || state.Get().status != "已停止")
          throw std::runtime_error("old failure result replaced the stop result");
        Start(tasks, state, 1, false, true);
        co_await WaitFailurePeek(state);
        Reset(state);
        co_await Delay(2200ms);
        co_await WaitSettled(state, 0, "reset during failure peek");
        if (state.Get().failure || state.Get().status != kSleepStatus)
          throw std::runtime_error("old failure result replaced reset");
        state.Update([](LabState& next) { next.reduced = true; });
        Start(tasks, state, 1, false, true);
        co_await WaitSettled(state, 0, "failure with reduced motion");
        state.Update([](LabState& next) { next.reduced = false; });
        Start(tasks, state, 1, true, false);
        co_await Delay(500ms);
        if (!probe->handVisible || !cardProbe->handVisible || !probe->loadingLooping ||
            !cardProbe->loadingLooping || probe->sleepLooping || cardProbe->sleepLooping)
          throw std::runtime_error("slow startup did not retain its animated petting hand");
        const auto loadingBefore = probe->loadingTicks;
        const auto cardLoadingBefore = cardProbe->loadingTicks;
        const auto loadingStarted = std::chrono::steady_clock::now();
        co_await Delay(500ms);
        const double loadingElapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - loadingStarted).count();
        if (probe->loadingTicks <= loadingBefore || cardProbe->loadingTicks <= cardLoadingBefore ||
            probe->loadingTicks - loadingBefore > loadingElapsed * 32 + 3 ||
            cardProbe->loadingTicks - cardLoadingBefore > loadingElapsed * 32 + 3)
          throw std::runtime_error("petting hand did not animate within its frame budget");
        state.Update([](LabState& next) { next.reduced = true; });
        co_await Delay(120ms);
        const auto loadingPaused = probe->loadingTicks;
        const auto cardLoadingPaused = cardProbe->loadingTicks;
        co_await Delay(160ms);
        if (probe->loadingLooping || cardProbe->loadingLooping || probe->loadingTicks != loadingPaused ||
            cardProbe->loadingTicks != cardLoadingPaused || !probe->handVisible || !cardProbe->handVisible)
          throw std::runtime_error("reduced motion did not preserve a static petting hand");
        state.Update([](LabState& next) { next.reduced = false; });
        co_await Delay(120ms);
        if (!probe->loadingLooping || !cardProbe->loadingLooping || probe->loadingTicks == loadingPaused ||
            cardProbe->loadingTicks == cardLoadingPaused)
          throw std::runtime_error("petting hand did not resume");
        Stop(tasks, state, 1);
        co_await WaitSettled(state, 0, "interrupted stop");
        co_await Delay(80ms);
        if (probe->handVisible || cardProbe->handVisible || probe->loadingLooping || cardProbe->loadingLooping)
          throw std::runtime_error("cancelled startup retained its petting hand");
        co_await Delay(3200ms);
        co_await WaitSettled(state, 0, "interrupted slow wake must not wake the cat");
        for (bool failure : {false, true}) {
          state.Update([failure](LabState& next) { next.failure = failure; });
          for (float p : {0.0F, 0.15F, 0.3F, 0.37F, 0.45F, 0.6F, 0.72F, 0.8F, 0.85F, 1.0F}) {
            SetTarget(state, p, true, "逐阶段检查", false);
            co_await Delay(120ms);
          }
        }
        std::puts("box-cat-demo: open-box growing zzz, rise-bound petting hand, sleep/running pause and frame budgets, hero/card ears/tail, pause/resume, failure wink, retry, cancellation, and scrub checks passed");
        smokePassed = true;
      } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
      }
      application.Quit();
    });
    return [] {};
  }, 0);
  const auto panel = [&](View content) {
    return std::move(content).With(Padding(24), CornerRadius(20),
        Background(theme.colors.surface_container_low));
  };
  auto trickIndex = UseState(0);
  auto trickSerial = UseState<std::uint64_t>(0);
  const auto trickRandom = std::make_shared<std::mt19937>(std::random_device{}());
  const auto trickCatalog = CatTrickCatalog();
  const auto playTrick = [trickIndex, trickSerial](int index) {
    trickIndex = index;
    trickSerial = trickSerial.Get() + 1;
  };
  const auto trickPreview = Stack {
      CatTrickPlayer(trickCatalog[trickIndex.Get()].trick, trickSerial.Get(), !current.reduced),
  }.With(Frame{.width = 240, .height = 200},
      Semantics{.role = SemanticRole::Button, .label = "再播一次"}, Indication{})
      .OnClick([trickSerial] { trickSerial = trickSerial.Get() + 1; }).Key("trick-preview");
  const auto trickButton = [playTrick, trickCatalog](std::size_t index) {
    return Button(trickCatalog[index].title)
        .OnClick([playTrick, index] { playTrick(static_cast<int>(index)); });
  };
  return ScrollView(Column {
    Row {
      Label("CLASH FLUX  /  MOTION LAB", 12, theme.colors.primary, true),
      Spacer(), Label("独立演示 · 模拟数据", 12, theme.colors.on_surface_variant),
    }.With(CrossAlign(CrossAxisAlignment::Center)),
    Label("抚摸唤醒", 30, theme.colors.on_surface, true),
    Label("轻抚唤醒 · 猫咪起身后手退场，再展开卡片。", 14, theme.colors.on_surface_variant),
    Row {
      panel(Column {
        Label("Logo 动画预览", 18, theme.colors.on_surface, true),
        Label("睡觉 → 轻抚唤醒 → 猫咪探头", 12, theme.colors.on_surface_variant),
        Stack {}.With(Frame{.width = 300, .height = 250}, mark,
            Semantics{.role = SemanticRole::Button, .label = "点击猫咪唤醒或停止"}, Indication{})
            .OnClick(toggle).Key("hero-animation"),
      }.With(Spacing(10), CrossAlign(CrossAxisAlignment::Center))).With(Grow(1)),
      panel(Column {
        Label("实际卡片预览", 18, theme.colors.on_surface, true),
        Label("点击后轻抚唤醒 · 再点停止", 12, theme.colors.on_surface_variant),
        Stack {}.With(Frame{.width = 360, .height = 250}, card,
            Semantics{.role = SemanticRole::Button, .label = "模拟启动或停止"},
            Indication{})
            .OnClick(toggle).Key("card-animation"),
      }.With(Spacing(10), CrossAlign(CrossAxisAlignment::Center))).With(Grow(1)),
    }.With(Spacing(20), CrossAlign(CrossAxisAlignment::Stretch)),
    Label(current.status, 14, theme.colors.primary, true),
    Row {
      Button("唤醒").OnClick([=] { Start(tasks, state, speed.Get(), false, false); }),
      Button("慢唤醒").OnClick([=] { Start(tasks, state, speed.Get(), true, false); }),
      Button("失败演示").OnClick([=] { Start(tasks, state, speed.Get(), false, true); }),
      Button("停止").OnClick([=] { Stop(tasks, state, speed.Get()); }),
      Button("重置").OnClick([=] {
        Reset(state);
        scrub = 0;
      }),
    }.With(Spacing(10)),
    panel(Column {
      Row {
        Label("手动进度", 14, theme.colors.on_surface, true), Spacer(),
        Button(current.failure ? "切换成功动画" : "切换失败动画").OnClick([=] {
          state.Update([](LabState& next) { ++next.serial; next.failure = !next.failure; });
          SetTarget(state, scrub.Get(), true, "手动预览 · 拖动检查任意一帧", false);
        }),
        Label(std::to_string(static_cast<int>(scrub.Get() * 100)) + "%", 12, theme.colors.primary),
      }.With(Spacing(12), CrossAlign(CrossAxisAlignment::Center)),
      Row {
        Slider(scrub).Range(0, 1).OnChanged([=](float value) {
          scrub = value;
          state.Update([](LabState& next) { ++next.serial; });
          SetTarget(state, value, true, "手动预览 · 拖动检查任意一帧", false);
        }).With(Grow(1)),
      }.With(Frame{.height = 40}, CrossAlign(CrossAxisAlignment::Center)),
      Row {
        Label("睡觉", 11, theme.colors.on_surface_variant), Spacer(),
        Label("轻抚 / 等待", 11, theme.colors.on_surface_variant), Spacer(),
        Label(current.failure ? "探头 / wink" : "猫咪探头", 11, theme.colors.on_surface_variant), Spacer(),
        Label(current.failure ? "回去睡觉" : "卡片展开", 11, theme.colors.on_surface_variant),
      },
      Row {
        Label("播放速度", 13, theme.colors.on_surface),
        Slider(speed).Range(0.25F, 2).Step(0.25F)
            .OnChanged([=](float value) { speed = value; }).With(Grow(1)),
        Label(std::to_string(static_cast<int>(speed.Get() * 100)) + "%", 12, theme.colors.primary),
        Checkbox(current.reduced).OnChanged([=](bool value) {
          state.Update([&](LabState& next) { next.reduced = value; });
        }),
        Label("减少动态效果", 12, theme.colors.on_surface_variant),
      }.With(Spacing(12), CrossAlign(CrossAxisAlignment::Center)),
    }.With(Spacing(14))),
    Label("仅使用本地模拟状态；连接数与时长为示例值。", 12, theme.colors.on_surface_variant),
    panel(Column {
      Label("彩蛋动画", 18, theme.colors.on_surface, true),
      Label("完整小猫的 8 个一次性动作 · 播完停帧；点击预览再播一次", 12,
          theme.colors.on_surface_variant),
      Column {
        trickPreview,
        Label(trickCatalog[trickIndex.Get()].title, 14, theme.colors.primary, true),
      }.With(Spacing(8), CrossAlign(CrossAxisAlignment::Center)),
      Row {
        trickButton(0), trickButton(1), trickButton(2), trickButton(3),
      }.With(Spacing(10)),
      Row {
        trickButton(4), trickButton(5), trickButton(6), trickButton(7),
      }.With(Spacing(10)),
      Button("随机一只小猫").OnClick([playTrick, trickRandom, trickCatalog] {
        std::uniform_int_distribution<int> pick(0, static_cast<int>(trickCatalog.size()) - 1);
        playTrick(pick(*trickRandom));
      }),
    }.With(Spacing(14), CrossAlign(CrossAxisAlignment::Stretch))),
  }.With(Padding(32), Spacing(18), CrossAlign(CrossAxisAlignment::Stretch)))
      .With(Background(theme.colors.background));
}

[[huxerui::composable]] View BrandPanel(bool dark) {
  const auto theme = UseTheme();
  const auto logo = dark ? app::images::project_logo_dark : app::images::project_logo_light;
  return Column {
    Label(dark ? "深色主题" : "浅色主题", 20, theme.colors.on_surface, true),
    Image(logo).Fit(ImageFit::Contain).With(Frame{.width = 240, .height = 240}),
    Row {
      Image(logo).Fit(ImageFit::Contain).With(Frame{.width = 32, .height = 32}),
      Image(logo).Fit(ImageFit::Contain).With(Frame{.width = 48, .height = 48}),
      Image(logo).Fit(ImageFit::Contain).With(Frame{.width = 64, .height = 64}),
    }.With(Spacing(24), CrossAlign(CrossAxisAlignment::Center)),
    Label("32 / 48 / 64 pt · 项目正式资源", 13, theme.colors.on_surface_variant),
  }.With(Padding(32), Spacing(28), Grow(1),
      CrossAlign(CrossAxisAlignment::Center), Background(theme.colors.background));
}

[[huxerui::composable]] View BrandPreview() {
  const auto application = UseApplication();
  const auto tasks = UseTaskScope();
  Lifecycle([=] {
    if (smoke) tasks.Launch([=]() -> Task<void> {
      co_await Delay(1500ms);
      std::puts("box-cat-demo: production logo resources mounted in light/dark themes");
      smokePassed = true;
      application.Quit();
    });
    return [] {};
  }, 0);
  return Row {
    MaterialTheme(MaterialLightThemeSpec(), BrandPanel(false)),
    MaterialTheme(MaterialDarkThemeSpec(), BrandPanel(true)),
  }.With(CrossAlign(CrossAxisAlignment::Stretch));
}

View App() {
  if (brandPreview) return BrandPreview();
  auto theme = MaterialDarkThemeSpec();
  theme.colors.primary = Color::Rgb(76, 190, 238);
  return MaterialTheme(theme, tricksPreview ? CatTrickLab() : Lab());
}

WindowOptions DemoWindow() {
  return {.title = tricksPreview ? "Clash Flux · 猫咪动作实验室" : "Clash Flux · 抚摸唤醒演示",
          .initial_size = tricksPreview ? Size{1040, 900} : Size{1000, 940},
          .minimum_size = Size{940, 800}};
}

} // namespace boxcat

namespace boxcat {
int RunDemoApplication() {
  // CLI mode must be selected before freezing the application's window options.
  const Application application{App, {.window = DemoWindow(), .show_debug_overlay = false}};
  return RunApplication();
}
} // namespace boxcat
