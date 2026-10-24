#pragma once

#include <huxerui/geometry.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace boxcat {

inline constexpr huxerui::Point kLogoBoxJoint{64, 78.2F};

// Rear leaves fold away past flat; their projected faces become narrow
// and sit behind the rim. Foreground leaves keep the shared center joint.
inline std::array<huxerui::Point, 4> LogoBoxFlap(int side, float opening) {
  using Quad = std::array<huxerui::Point, 4>;
  constexpr std::array<Quad, 4> closed{{
      {{{64, 49.8F}, {25.5F, 64}, {44.75F, 71.1F}, {83.25F, 56.9F}}},
      {{{64, 49.8F}, {102.5F, 64}, {83.25F, 71.1F}, {44.75F, 56.9F}}},
      {{{25.5F, 64}, kLogoBoxJoint, {83.25F, 71.1F}, {44.75F, 56.9F}}},
      {{{102.5F, 64}, kLogoBoxJoint, {44.75F, 71.1F}, {83.25F, 56.9F}}}}};
  if (side < 2) {
    const float angle = opening * (220.0F * 3.14159265F / 180.0F);
    const float inwardX = side == 0 ? 19.25F : -19.25F;
    const huxerui::Point offset{inwardX * std::cos(angle),
        7.1F * std::cos(angle) - 12.0F * std::sin(angle)};
    const auto a = closed[side][0];
    const auto b = closed[side][1];
    return {a, b, {b.x + offset.x, b.y + offset.y}, {a.x + offset.x, a.y + offset.y}};
  }
  constexpr std::array<Quad, 2> openedFront{{
      {{{23.7F, 63.3F}, kLogoBoxJoint, {44.7F, 89.3F}, {12.2F, 72.1F}}},
      {{{104.5F, 63.3F}, kLogoBoxJoint, {83.5F, 89.3F}, {116, 72.1F}}}}};
  const float angle = opening * 3.14159265F;
  const float fold = (1 - std::cos(angle)) / 2;
  const float lift = 16 * std::sin(angle);
  Quad result;
  for (std::size_t i = 0; i < result.size(); ++i) {
    const auto a = closed[side][i];
    const auto b = openedFront[side - 2][i];
    result[i] = {a.x + (b.x - a.x) * fold,
                 a.y + (b.y - a.y) * fold - (i >= 2 ? lift : 0)};
  }
  // Keep the free edge parallel to its hinge through the edge-on phase.
  // Restore the illustration's small asymmetries only after unfolding.
  const huxerui::Point offset{
      ((result[2].x - result[1].x) + (result[3].x - result[0].x)) / 2,
      ((result[2].y - result[1].y) + (result[3].y - result[0].y)) / 2};
  const float t = std::clamp((opening - 0.86F) / 0.14F, 0.0F, 1.0F);
  const float restore = t * t * (3 - 2 * t);
  for (std::size_t i = 2; i < result.size(); ++i) {
    const auto hinge = result[i == 2 ? 1 : 0];
    const huxerui::Point aligned{hinge.x + offset.x, hinge.y + offset.y};
    result[i] = {aligned.x + (result[i].x - aligned.x) * restore,
                 aligned.y + (result[i].y - aligned.y) * restore};
  }
  return result;
}

} // namespace boxcat
