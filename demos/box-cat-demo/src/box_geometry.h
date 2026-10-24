#pragma once

#include <huxerui/geometry.h>

#include <array>
#include <cmath>

namespace boxcat {

enum class BoxView { Perspective, Front };

struct BoxPoint {
  float x, y, z;
};

// Shared pinhole camera parameters with tools/generate_box_views.py.
// All dimensions belong to one unit cube; each flap is half a face deep.
inline huxerui::Point ProjectBox(BoxPoint point, BoxView view) {
  constexpr float pi = 3.14159265F;
  const float yaw = view == BoxView::Front ? 0 : -28 * pi / 180;
  constexpr float pitch = 26 * pi / 180;
  const float horizontal = point.x * std::cos(yaw) + point.y * std::sin(yaw);
  const float depth = -point.x * std::sin(yaw) + point.y * std::cos(yaw);
  const float height = point.z + 0.45F;
  const float distance = depth * std::cos(pitch) + height * std::sin(pitch);
  const float perspective = 4.5F / (4.5F - distance);
  return {64 + 52 * horizontal * perspective,
          78 + 52 * (depth * std::sin(pitch) - height * std::cos(pitch)) * perspective};
}

// Back, front, left, right. Hinges remain fixed during the entire rotation.
inline std::array<huxerui::Point, 4> BoxFlap(BoxView view, int side, float opening) {
  constexpr std::array<std::array<BoxPoint, 3>, 4> hinges{{
      {{{-0.5F, -0.5F, 0}, {0.5F, -0.5F, 0}, {0, 0.5F, 0}}},
      {{{-0.5F, 0.5F, 0}, {0.5F, 0.5F, 0}, {0, -0.5F, 0}}},
      {{{-0.5F, -0.5F, 0}, {-0.5F, 0.5F, 0}, {0.5F, 0, 0}}},
      {{{0.5F, -0.5F, 0}, {0.5F, 0.5F, 0}, {-0.5F, 0, 0}}}}};
  const auto [a, b, inward] = hinges[side];
  const float angle = opening * 3.14159265F;
  const auto free = [&](BoxPoint hinge) {
    return ProjectBox({hinge.x + inward.x * std::cos(angle),
                       hinge.y + inward.y * std::cos(angle),
                       0.5F * std::sin(angle)}, view);
  };
  return {ProjectBox(a, view), ProjectBox(b, view), free(b), free(a)};
}

} // namespace boxcat
