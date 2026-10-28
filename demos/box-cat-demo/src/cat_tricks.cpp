#include "cat_tricks.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <stdexcept>
#include <vector>

#include "app_resources.h"

namespace boxcat {
extern bool smoke;
extern bool smokePassed;
namespace {
using namespace huxerui;
constexpr double kFrameInterval = 1.0 / 30.0;
constexpr float kPi = 3.14159265358979323846F;

float Ease(float t, float from, float to) {
  t = std::clamp((t - from) / (to - from), 0.0F, 1.0F);
  return t * t * (3 - 2 * t);
}
float Beat(float t, float in, float peak, float out) {
  return Ease(t, in, peak) * (1 - Ease(t, peak, out));
}
float Wave(float t, float cycles) { return std::sin(2 * kPi * cycles * t); }

struct Pose {
  float x = 0, y = 0, rotation = 0, headRotation = 0, pawReach = 0;
  float sx = 1, sy = 1, leftEar = 0, rightEar = 0, tail = 0;
  float closed = 0, happy = 0, gaze = 0;
};

Pose Evaluate(CatTrick trick, float t) {
  Pose p;
  if (t <= 0 || t >= 1) return p;
  const float envelope = Ease(t, 0, 0.12F) * (1 - Ease(t, 0.82F, 1));
  switch (trick) {
    case CatTrick::Tilt: {
      const float look = Ease(t, 0.10F, 0.30F) - Ease(t, 0.68F, 0.96F);
      p.headRotation = -0.22F * look + 0.09F * Beat(t, 0.40F, 0.55F, 0.74F);
      p.rotation = -0.025F * look;
      p.gaze = -2.2F * look;
      p.leftEar = -0.15F * look;
      p.rightEar = 0.12F * Beat(t, 0.32F, 0.41F, 0.52F);
      p.tail = 0.08F * Wave(t, 1.5F) * envelope;
      break;
    }
    case CatTrick::Hop: {
      const float windup = Beat(t, 0, 0.14F, 0.25F);
      const float air = Beat(t, 0.18F, 0.43F, 0.70F);
      const float landing = Beat(t, 0.62F, 0.73F, 0.95F);
      p.y = 6 * windup - 30 * air + 5 * landing;
      p.sx = 1 + 0.12F * windup - 0.06F * air + 0.10F * landing;
      p.sy = 1 - 0.14F * windup + 0.10F * air - 0.12F * landing;
      p.happy = Ease(t, 0.14F, 0.26F) * (1 - Ease(t, 0.74F, 0.94F));
      p.tail = 0.21F * Wave(t, 2) * envelope;
      p.leftEar = -0.12F * air;
      p.rightEar = 0.12F * air;
      break;
    }
    case CatTrick::Stretch: {
      const float stretch = Ease(t, 0.06F, 0.38F) * (1 - Ease(t, 0.65F, 0.94F));
      p.sx = 1 + 0.15F * stretch;
      p.sy = 1 - 0.17F * stretch;
      p.y = 7 * stretch;
      p.rotation = -0.09F * stretch;
      p.happy = stretch;
      p.tail = -0.32F * stretch;
      p.leftEar = -0.20F * stretch;
      p.rightEar = 0.16F * stretch;
      p.pawReach = 3 * stretch;
      break;
    }
    case CatTrick::Roll: {
      const float roll = Ease(t, 0.18F, 0.80F);
      p.rotation = -0.18F * Beat(t, 0, 0.16F, 0.29F) + 2 * kPi * roll;
      p.x = 12 * std::sin(kPi * roll);
      p.y = -10 * std::sin(kPi * roll);
      p.sx = p.sy = 1 - 0.06F * envelope;
      p.happy = envelope;
      p.tail = 0.22F * Wave(t, 1) * envelope;
      break;
    }
    case CatTrick::TailWag:
      p.tail = 0.40F * Wave(t, 4) * envelope;
      p.rotation = -0.035F * Wave(t, 4) * envelope;
      p.gaze = 1.2F * envelope;
      p.rightEar = -0.10F * Beat(t, 0.30F, 0.40F, 0.53F);
      break;
    case CatTrick::Startle: {
      const float jump = Beat(t, 0.08F, 0.20F, 0.60F);
      const float settle = Beat(t, 0.54F, 0.67F, 0.95F);
      p.y = -25 * jump + 4 * settle;
      p.sx = 1 - 0.08F * jump + 0.10F * settle;
      p.sy = 1 + 0.13F * jump - 0.10F * settle;
      p.leftEar = -0.28F * jump;
      p.rightEar = 0.28F * jump;
      p.tail = -0.24F * jump;
      p.closed = Beat(t, 0.68F, 0.74F, 0.83F);
      break;
    }
    case CatTrick::Doze: {
      const float sleep = Ease(t, 0.04F, 0.30F) * (1 - Ease(t, 0.64F, 0.72F));
      const float wake = Beat(t, 0.70F, 0.78F, 0.94F);
      p.rotation = 0.16F * sleep - 0.08F * wake;
      p.y = 7 * sleep - 13 * wake;
      p.sy = 1 - 0.08F * sleep + 0.01F * Wave(t, 3) * sleep;
      p.closed = sleep;
      p.leftEar = -0.20F * wake;
      p.rightEar = 0.20F * wake;
      p.tail = -0.12F * sleep;
      break;
    }
    case CatTrick::Rub:
      p.x = 9 * Wave(t, 1.5F) * envelope;
      p.rotation = 0.16F * Wave(t, 1.5F) * envelope;
      p.y = 3 * envelope;
      p.happy = envelope;
      p.tail = -0.22F * envelope + 0.10F * Wave(t, 3) * envelope;
      p.leftEar = -0.08F * envelope;
      p.rightEar = 0.08F * envelope;
      break;
  }
  return p;
}

const CatTrickSpec kCatalog[] = {
    {CatTrick::Tilt, "歪头好奇", 2.2, "先看向一边，歪头停一会儿，再抖抖耳朵。"},
    {CatTrick::Hop, "开心蹦跳", 1.8, "蹲下蓄力、腾空眯眼，落地时软软地缓冲。"},
    {CatTrick::Stretch, "伸懒腰", 2.5, "慢慢压低身体，耳朵舒展开，尾巴伸直再放松。"},
    {CatTrick::Roll, "打个滚", 2.0, "先向反方向借力，开心地滚一圈，再回到原位。"},
    {CatTrick::TailWag, "摇尾巴", 2.4, "身体稳稳待着，尾巴摆动，耳朵偷偷回应一下。"},
    {CatTrick::Startle, "惊讶一跳", 1.7, "耳朵竖起、身体弹起，落地后眨眨眼缓过神。"},
    {CatTrick::Doze, "打盹惊醒", 3.2, "眼睛慢慢闭上，低头打个盹，再突然清醒过来。"},
    {CatTrick::Rub, "蹭蹭你", 2.6, "眯起弯弯的眼睛，左右轻蹭，尾巴也跟着摇。"},
};

// Transform2D maps x with m11/m21 and y with m12/m22. Translate the
// transformed pivot, including nonuniform scale, so squashes do not drift.
Transform2D Around(Point pivot, float angle, float sx = 1, float sy = 1, Point offset = {}) {
  const float c = std::cos(angle), s = std::sin(angle);
  Transform2D transform{.m11 = c * sx, .m12 = s * sx, .m21 = -s * sy, .m22 = c * sy};
  transform.translate_x = pivot.x + offset.x - transform.m11 * pivot.x - transform.m21 * pivot.y;
  transform.translate_y = pivot.y + offset.y - transform.m12 * pivot.x - transform.m22 * pivot.y;
  return transform;
}

struct TrickMotion {
  class Extension;
  VectorAsset head, torso, paws, earLeft, earRight, tail, eyeLeft, eyeRight, eyesSleeping, eyesContent;
  std::shared_ptr<CatTrickProbe> probe;
  CatTrick trick = CatTrick::Tilt;
  std::uint64_t serial = 0;
  bool motionEnabled = true;
  double speed = 1;
  std::optional<float> progress;
  Color primary, shadow;
  bool operator==(const TrickMotion&) const = default;
};

class TrickMotion::Extension final : public NodeExtension {
public:
  Extension(ViewNode& node, const TrickMotion& value) { Update(node, value); }
  void Update(ViewNode&, const TrickMotion& value) {
    const bool replay = value.serial != spec_.serial || value.trick != spec_.trick;
    spec_ = value;
    if (replay && value.serial > 0) { elapsed_ = 0; countdown_ = 0; playing_ = true; }
    if (!value.motionEnabled || value.progress) playing_ = false;
    if (replay && spec_.probe) ++spec_.probe->plays;
    Publish();
    InvalidatePaint();
  }
  FrameResult OnFrame(ViewNode&, const FrameInfo& frame) override {
    if (spec_.probe) ++spec_.probe->frames;
    if (frame.reduced_motion || !spec_.motionEnabled) {
      const bool changed = playing_ || elapsed_ != Duration();
      elapsed_ = Duration();
      playing_ = false;
      Publish();
      if (changed) InvalidatePaint();
      return {};
    }
    if (!playing_) return {};
    elapsed_ = std::min(Duration(), elapsed_ + frame.delta_time * spec_.speed);
    playing_ = elapsed_ < Duration();
    countdown_ -= frame.delta_time;
    if (countdown_ <= 0 || !playing_) {
      countdown_ = kFrameInterval;
      Publish();
      InvalidatePaint();
    }
    return playing_ ? FrameResult{.wake_after = std::max(0.001, countdown_)} : FrameResult{};
  }
  void PaintAboveContent(const ViewNode& node, PaintContext& paint) const override {
    const auto bounds = node.Bounds();
    const float side = std::min(bounds.width, bounds.height);
    const float scale = side / 144;
    const float t = spec_.progress.value_or(spec_.motionEnabled ? Progress() : 1.0F);
    const Pose p = Evaluate(spec_.trick, t);
    paint.PushTransform(Transform2D{.m11 = scale, .m22 = scale,
        .translate_x = (bounds.width - side) / 2 + 8 * scale,
        .translate_y = (bounds.height - side) / 2 + 14 * scale});
    const float shadowScale = 1 - std::clamp(-p.y / 80, 0.0F, 0.4F);
    paint.PushTransform(Around({64, 103}, 0, shadowScale, 0.14F));
    paint.DrawCircle({64, 103}, 28, spec_.shadow);
    paint.PopTransform();
    paint.PushTransform(Around({64, 64}, p.rotation, p.sx, p.sy, {p.x, p.y}));
    const Rect full{0, 0, 128, 128};
    const auto hinge = [&](const VectorAsset& asset, Point pivot, float angle) {
      paint.PushTransform(Around(pivot, angle));
      paint.DrawImage(asset, full);
      paint.PopTransform();
    };
    paint.PushTransform(Transform2D{.translate_x = -5, .translate_y = 10});
    hinge(spec_.tail, {92, 74}, p.tail);
    paint.PopTransform();
    paint.DrawImage(spec_.torso, full);
    paint.PushTransform(Transform2D{.translate_y = p.pawReach});
    paint.DrawImage(spec_.paws, full);
    paint.PopTransform();
    paint.PushTransform(Around({64, 72}, p.headRotation));
    hinge(spec_.earLeft, {47, 40}, p.leftEar);
    hinge(spec_.earRight, {80, 40}, p.rightEar);
    paint.DrawImage(spec_.head, full);
    const float open = 1 - std::max(p.closed, p.happy);
    if (open > 0.001F) {
      paint.PushTransform(Around({64, 54}, 0, 1, std::max(0.05F, open), {p.gaze, 0}));
      paint.DrawImage(spec_.eyeLeft, full, {}, open);
      paint.DrawImage(spec_.eyeRight, full, {}, open);
      paint.PopTransform();
    }
    if (p.closed > 0) paint.DrawImage(spec_.eyesSleeping, full, {}, p.closed);
    if (p.happy > 0) paint.DrawImage(spec_.eyesContent, full, {}, p.happy);
    paint.PopTransform();
    paint.PopTransform();
    if (spec_.trick == CatTrick::Doze && p.closed > 0.1F) {
      for (int i = 0; i < 3; ++i) {
        auto color = spec_.primary;
        color.alpha *= p.closed * (0.8F - i * 0.18F);
        paint.DrawText({87.0F + 9 * i, 28.0F - 10 * i, 20, 22}, "z",
            TextStyle{Font::System(10.0F + 3 * i).WithWeight(FontWeight::SemiBold), color});
      }
    }
    paint.PopTransform();
  }
private:
  double Duration() const { return kCatalog[static_cast<std::size_t>(spec_.trick)].duration; }
  float Progress() const { return static_cast<float>(std::clamp(elapsed_ / Duration(), 0.0, 1.0)); }
  void Publish() {
    if (spec_.probe) { spec_.probe->playing = playing_; spec_.probe->progress = spec_.progress.value_or(Progress()); }
  }
  TrickMotion spec_;
  double elapsed_ = 0, countdown_ = 0;
  bool playing_ = false;
};
} // namespace

std::span<const CatTrickSpec> CatTrickCatalog() { return kCatalog; }

[[huxerui::composable]] huxerui::View CatTrickPlayer(CatTrick trick, std::uint64_t serial,
    bool motionEnabled, std::shared_ptr<CatTrickProbe> probe, double speed, std::optional<float> progress) {
  const auto& theme = UseTheme();
  auto shadow = theme.colors.on_surface;
  shadow.alpha = 0.08F;
  return Stack {}.With(TrickMotion{
      .head = UseVectorImage(app::images::cat_trick_head),
      .torso = UseVectorImage(app::images::cat_trick_torso),
      .paws = UseVectorImage(app::images::cat_trick_paws),
      .earLeft = UseVectorImage(app::images::cat_trick_ear_left),
      .earRight = UseVectorImage(app::images::cat_trick_ear_right),
      .tail = UseVectorImage(app::images::cat_trick_tail),
      .eyeLeft = UseVectorImage(app::images::cat_trick_eye_left),
      .eyeRight = UseVectorImage(app::images::cat_trick_eye_right),
      .eyesSleeping = UseVectorImage(app::images::cat_trick_eyes_sleeping),
      .eyesContent = UseVectorImage(app::images::cat_trick_eyes_content),
      .probe = probe, .trick = trick, .serial = serial,
      .motionEnabled = motionEnabled && !theme.motion.reduced_motion,
      .speed = std::clamp(speed, 0.25, 2.0), .progress = progress,
      .primary = theme.colors.primary, .shadow = shadow});
}

[[huxerui::composable]] huxerui::View CatTrickLab() {
  const auto& theme = UseTheme();
  auto selected = UseState(0);
  auto serial = UseState<std::uint64_t>(0);
  auto speed = UseState(1.0F);
  auto scrub = UseState(0.0F);
  auto manual = UseState(false);
  auto reduced = UseState(false);
  auto random = UseState(std::make_shared<std::mt19937>(std::random_device{}()));
  auto probe = UseState(std::make_shared<CatTrickProbe>()).Get();
  auto tasks = UseTaskScope();
  auto application = UseApplication();
  Lifecycle([tasks, application, selected, serial, manual, scrub, reduced, probe] {
    if (smoke) tasks.Launch([=]() -> Task<void> {
      using namespace std::chrono_literals;
      try {
        co_await Delay(150ms);
        for (int index = 0; index < 8; ++index) {
          selected = index; ++serial;
          co_await Delay(100ms);
          if (!probe->playing) throw std::runtime_error("trick did not start");
          co_await Delay(std::chrono::duration<double>{kCatalog[index].duration + 0.25});
          if (probe->playing || probe->progress < 0.999F)
            throw std::runtime_error("trick did not settle at its endpoint");
          co_await Delay(100ms);
          const auto frames = probe->frames;
          co_await Delay(160ms);
          if (probe->frames != frames) throw std::runtime_error("settled trick kept requesting frames");
        }
        ++serial; co_await Delay(120ms); ++serial; co_await Delay(100ms);
        if (!probe->playing || probe->progress > 0.2F)
          throw std::runtime_error("replay did not restart the retained timeline");
        reduced = true; co_await Delay(150ms);
        if (probe->playing) throw std::runtime_error("reduced motion did not stop the trick");
        manual = true; scrub = 0.45F; co_await Delay(100ms);
        if (std::abs(probe->progress - 0.45F) > 0.001F)
          throw std::runtime_error("manual preview did not select the requested pose");
        std::puts("box-cat-demo: 8 tricks, settled frame clocks, rapid replay, reduced motion and scrubbing passed");
        smokePassed = true;
      } catch (const std::exception& error) {
        std::fprintf(stderr, "cat tricks: %s\n", error.what());
      }
      application.Quit();
    });
    return [] {};
  }, 0);
  const auto& current = kCatalog[selected.Get()];
  const auto play = [selected, serial, manual](int index) {
    selected = index; manual = false; ++serial;
  };
  std::vector<View> choices;
  const auto baseButtonStyle = UseEnvironment<ButtonStyle>();
  for (int i = 0; i < 8; ++i) {
    const bool active = selected.Get() == i;
    auto buttonStyle = baseButtonStyle;
    buttonStyle.background = active ? theme.colors.primary_container : theme.colors.surface_container_high;
    buttonStyle.label_style.foreground = active ? theme.colors.on_primary_container : theme.colors.on_surface;
    choices.push_back(ProvideEnvironment(buttonStyle, Button(kCatalog[i].title).OnClick([play, i] { play(i); })
        .With(Frame{.width = 210, .height = 48},
            Border(active ? theme.colors.primary : theme.colors.outline, active ? 1.5F : 0.5F),
            CornerRadius(12))).Key(i));
  }
  const auto muted = TextStyle{Font::System(13), theme.colors.on_surface_variant};
  return ScrollView(Column {
    Text("CLASH FLUX  /  CAT MOTION LAB").Style(TextStyle{Font::System(12), theme.colors.primary}),
    Text("猫咪动作实验室", TextRole::Title)
        .Style(TextStyle{Font::System(30).WithWeight(FontWeight::SemiBold), theme.colors.on_surface}),
    Text("同一只小猫，八种小情绪。选一个动作，看看它怎么回应。 ").Style(muted),
    Row {
      Column {
        CatTrickPlayer(current.trick, serial.Get(), !reduced.Get(), probe, speed.Get(),
            manual.Get() ? std::optional<float>{scrub.Get()} : std::nullopt)
            .With(Frame{.width = 360, .height = 330},
                Semantics{.role = SemanticRole::Button, .label = "重播当前动作"}, Indication{})
            .OnClick([play, selected] { play(selected.Get()); }).Key("trick-lab-preview"),
        Text("点击小猫 · 再播一次").Style(muted),
      }.With(Spacing(8), CrossAlign(CrossAxisAlignment::Center)),
      Column {
        Text(current.title, TextRole::Title),
        Text(current.description).Style(TextStyle{Font::System(15), theme.colors.on_surface_variant}),
        Text(std::to_string(static_cast<int>(current.duration * 1000)) + " ms  ·  单次播放后回到待机").Style(muted),
        Row {
          Button("再播一次").OnClick([play, selected] { play(selected.Get()); }),
          Button("随机动作").OnClick([play, selected, random] {
            std::uniform_int_distribution<int> pick(1, 7);
            play((selected.Get() + pick(*random.Get())) % 8);
          }),
        }.With(Spacing(12)),
        Text("播放速度").Style(muted),
        Row {
          Slider(speed).Range(0.25F, 2).Step(0.25F).OnChanged([speed](float value) { speed = value; })
              .With(Grow()),
          Text(std::to_string(static_cast<int>(speed.Get() * 100)) + "%").Style(muted),
        }.With(Spacing(12), CrossAlign(CrossAxisAlignment::Center)),
        Row {
          Checkbox(reduced).OnChanged([reduced](bool value) { reduced = value; }),
          Text("减少动态效果").Style(muted),
        }.With(Spacing(10), CrossAlign(CrossAxisAlignment::Center)),
      }.With(Grow(), Spacing(18), MainAlign(MainAxisAlignment::Center)),
    }.With(Padding(24), Spacing(32), Background(theme.colors.surface_container_low), CornerRadius(20)),
    Flow(choices).With(Spacing(12)),
    Column {
      Row {
        Text("逐帧检查", TextRole::Title), Spacer(),
        Text(manual.Get() ? std::to_string(static_cast<int>(scrub.Get() * 100)) + "%" : "拖动后暂停播放")
            .Style(muted),
      }.With(CrossAlign(CrossAxisAlignment::Center)),
      Slider(scrub).Range(0, 1).OnChanged([scrub, manual](float value) { scrub = value; manual = true; }),
      Row {
        Text("准备").Style(muted), Spacer(), Text("动作 / 表情").Style(muted), Spacer(), Text("回到待机").Style(muted),
      },
    }.With(Padding(24), Spacing(12), CrossAlign(CrossAxisAlignment::Stretch),
        Background(theme.colors.surface_container_low), CornerRadius(20)),
    Text("独立动画演示 · 不连接内核、代理或订阅。减少动态效果时可用滑杆查看静态姿势。").Style(muted),
  }.With(Padding(32), Spacing(20), CrossAlign(CrossAxisAlignment::Stretch)))
      .With(Background(theme.colors.background));
}
} // namespace boxcat
