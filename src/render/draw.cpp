#include "render/draw.h"

#include <cmath>

#include "game/bindings.h"
#include "game/game.h"
#include "render/overlay.h"

namespace cg::render::draw {
namespace {

constexpr float kNearW = 0.01f;

struct Clip {
  float x, y, z, w;
};

// Camera state, cached once per frame (per thread) so ESP with many calls stays cheap.
struct CameraCache {
  uint64_t frame = ~0ull;
  bool ok = false;
  game::Mat4 m{};
  float width = 0, height = 0;
};

const CameraCache& Camera() {
  thread_local CameraCache c;
  const uint64_t frame = render::FrameCount();
  if (c.frame == frame) return c;
  c.frame = frame;
  c.ok = false;
  c.width = render::ScreenWidth();
  c.height = render::ScreenHeight();
  if (!(c.width > 0) || !(c.height > 0)) return c;
  auto vp = game::camera::ViewProjection();
  if (!vp) return c;
  c.m = *vp;
  if (game::Bindings::Get().Offset("Camera.MatrixTransposed").value_or(0) == 1) {
    game::Mat4 t;
    for (int r = 0; r < 4; ++r)
      for (int col = 0; col < 4; ++col) t.m[r][col] = vp->m[col][r];
    c.m = t;
  }
  c.ok = true;
  return c;
}

// clip = [x y z 1] * M (row-vector convention).
Clip ToClip(const game::Mat4& m, const game::Vec3& p) {
  Clip c;
  c.x = p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0];
  c.y = p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1];
  c.z = p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2];
  c.w = p.x * m.m[0][3] + p.y * m.m[1][3] + p.z * m.m[2][3] + m.m[3][3];
  return c;
}

bool ClipToScreen(const CameraCache& cam, const Clip& c, ImVec2& out) {
  if (!(c.w >= kNearW)) return false;   // also rejects NaN
  const float nx = c.x / c.w, ny = c.y / c.w;
  const float sx = (nx * 0.5f + 0.5f) * cam.width;
  const float sy = (0.5f - ny * 0.5f) * cam.height;
  if (!std::isfinite(sx) || !std::isfinite(sy)) return false;
  // Keep far-off-screen points finite and sane for ImGui's float math.
  constexpr float kLimit = 1.0e5f;
  if (std::fabs(sx) > kLimit || std::fabs(sy) > kLimit) return false;
  out = ImVec2(sx, sy);
  return true;
}

ImDrawList* List() { return ImGui::GetCurrentContext() ? ImGui::GetBackgroundDrawList() : nullptr; }

}  // namespace

bool WorldToScreen(const game::Vec3& world, ImVec2& out) {
  const CameraCache& cam = Camera();
  if (!cam.ok) return false;
  return ClipToScreen(cam, ToClip(cam.m, world), out);
}

void Line3D(const game::Vec3& a, const game::Vec3& b, ImU32 color, float thickness) {
  ImDrawList* dl = List();
  const CameraCache& cam = Camera();
  if (!dl || !cam.ok) return;
  Clip ca = ToClip(cam.m, a), cb = ToClip(cam.m, b);
  if (!(ca.w >= kNearW) && !(cb.w >= kNearW)) return;
  // Clip the segment against the near plane (w = kNearW) so lines partly behind the camera still draw.
  auto clipTo = [](Clip& behind, const Clip& front) {
    const float t = (kNearW - behind.w) / (front.w - behind.w);
    behind.x += (front.x - behind.x) * t;
    behind.y += (front.y - behind.y) * t;
    behind.z += (front.z - behind.z) * t;
    behind.w = kNearW;
  };
  if (!(ca.w >= kNearW)) clipTo(ca, cb);
  else if (!(cb.w >= kNearW)) clipTo(cb, ca);
  ImVec2 sa, sb;
  if (!ClipToScreen(cam, ca, sa) || !ClipToScreen(cam, cb, sb)) return;
  dl->AddLine(sa, sb, color, thickness);
}

void Text3D(const game::Vec3& at, ImU32 color, const char* text, bool centered) {
  if (!text || !*text || !List()) return;
  ImVec2 p;
  if (!WorldToScreen(at, p)) return;
  if (centered) {
    const ImVec2 sz = ImGui::CalcTextSize(text);
    p.x -= sz.x * 0.5f;
    p.y -= sz.y * 0.5f;
  }
  TextShadow(ImVec2(std::floor(p.x), std::floor(p.y)), color, text);
}

void Box3D(const game::Vec3& center, const game::Vec3& halfExtents, float yawRadians, ImU32 color, float thickness) {
  if (!List() || !Camera().ok) return;
  const float c = std::cos(yawRadians), s = std::sin(yawRadians);
  game::Vec3 corners[8];
  for (int i = 0; i < 8; ++i) {
    const float lx = (i & 1) ? halfExtents.x : -halfExtents.x;
    const float ly = (i & 2) ? halfExtents.y : -halfExtents.y;
    const float lz = (i & 4) ? halfExtents.z : -halfExtents.z;
    // Yaw rotates around the Z (up) axis.
    corners[i] = game::Vec3{center.x + lx * c - ly * s, center.y + lx * s + ly * c, center.z + lz};
  }
  static constexpr int kEdges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0},   // bottom
                                        {4, 5}, {5, 7}, {7, 6}, {6, 4},   // top
                                        {0, 4}, {1, 5}, {2, 6}, {3, 7}};  // verticals
  for (const auto& e : kEdges) Line3D(corners[e[0]], corners[e[1]], color, thickness);
}

void Marker3D(const game::Vec3& at, ImU32 color, float radiusPx) {
  ImDrawList* dl = List();
  if (!dl) return;
  ImVec2 p;
  if (!WorldToScreen(at, p)) return;
  const float r = radiusPx > 1.0f ? radiusPx : 1.0f;
  dl->AddCircleFilled(p, r, color, 16);
  dl->AddCircle(p, r + 1.0f, IM_COL32(0, 0, 0, (color >> IM_COL32_A_SHIFT) & 0xFF), 16, 1.5f);
}

void TextShadow(ImVec2 pos, ImU32 color, const char* text) {
  ImDrawList* dl = List();
  if (!dl || !text || !*text) return;
  const ImU32 shadow = IM_COL32(0, 0, 0, ((color >> IM_COL32_A_SHIFT) & 0xFF) * 200 / 255);
  dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), shadow, text);
  dl->AddText(pos, color, text);
}

}  // namespace cg::render::draw
