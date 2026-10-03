#pragma once
// World-space overlay drawing (ESP, waypoints, debug markers). Uses the camera view-projection
// from game::Camera; every call is a no-op when the camera binding is unavailable.
#include <cstdint>

#include <imgui.h>

#include "game/types.h"

namespace cg::render::draw {

bool WorldToScreen(const game::Vec3& world, ImVec2& out);   // false if behind camera / no camera

void Line3D(const game::Vec3& a, const game::Vec3& b, ImU32 color, float thickness = 1.5f);
void Text3D(const game::Vec3& at, ImU32 color, const char* text, bool centered = true);
void Box3D(const game::Vec3& center, const game::Vec3& halfExtents, float yawRadians, ImU32 color, float thickness = 1.5f);
void Marker3D(const game::Vec3& at, ImU32 color, float radiusPx = 6.0f);

// Screen-space helpers on the background draw list (always available).
void TextShadow(ImVec2 pos, ImU32 color, const char* text);

}  // namespace cg::render::draw
