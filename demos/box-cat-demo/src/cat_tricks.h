#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <huxerui/huxerui.h>

namespace boxcat {

enum class CatTrick { Tilt, Hop, Stretch, Roll, TailWag, Startle, Doze, Rub };

struct CatTrickSpec {
  CatTrick trick;
  const char* title;
  double duration;
  const char* description;
};

std::span<const CatTrickSpec> CatTrickCatalog();

struct CatTrickProbe {
  std::uint64_t frames = 0;
  std::uint64_t plays = 0;
  bool playing = false;
  float progress = 0;
};

// One-shot playful motion on the full-body demo cat. Every trick is evaluated
// from transforms and asset hinges only: no snapshot capture, no per-frame
// State writes, and the frame clock stops when the trick finishes.
huxerui::View CatTrickPlayer(CatTrick trick, std::uint64_t serial,
    bool motionEnabled = true, std::shared_ptr<CatTrickProbe> probe = {},
    double speed = 1, std::optional<float> progress = {});

huxerui::View CatTrickLab();

} // namespace boxcat
