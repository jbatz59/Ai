// Math operators for ImVec2/ImVec4 must be enabled before the first include of imgui.h.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/mp_guard.h"
#include "features/feature.h"
#include "game/bindings.h"
#include "game/game.h"
#include "render/fonts.h"
#include "render/overlay.h"
#include "render/theme.h"

namespace cg::ui::detail {
namespace {

namespace th = render::theme;

constexpr double kWatermarkAfterInject = 8.0;   // seconds
constexpr double kWatermarkAfterClose = 3.0;
constexpr double kFadeSeconds = 1.2;
constexpr size_t kMaxActiveLines = 14;

bool g_lastMenuOpen = false;
double g_watermarkUntil = kWatermarkAfterInject;
double g_positionAt = -1.0;
std::optional<game::Vec3> g_position;

struct Line {
  std::string text;
  ImU32 color;
  ImFont* font;
  float size;
};

ImFont* FontOr(ImFont* f) { return f != nullptr ? f : ImGui::GetFont(); }

ImVec2 Measure(const Line& l) { return l.font->CalcTextSizeA(l.size, FLT_MAX, 0.0f, l.text.c_str()); }

// Translucent plate behind HUD text so it stays readable over any scene.
void Plate(ImDrawList* dl, ImVec2 min, ImVec2 max, float alpha, float s) {
  const auto& p = th::Colors();
  dl->AddRectFilled(min, max, th::U32(p.bgAlt, 0.62f * alpha), 6.0f * s);
  dl->AddRect(min, max, th::U32(p.border, 0.55f * alpha), 6.0f * s);
}

// Draws a vertical block of lines. `alignRight` anchors the block's right edge at `anchor.x`.
ImVec2 DrawBlock(ImDrawList* dl, ImVec2 anchor, const std::vector<Line>& lines, float alpha, bool alignRight, float s) {
  if (lines.empty() || alpha <= 0.0f) return ImVec2(0, 0);
  const ImVec2 pad(9.0f * s, 6.0f * s);
  const float gap = 2.0f * s;
  float w = 0.0f, h = 0.0f;
  std::vector<ImVec2> sizes;
  sizes.reserve(lines.size());
  for (const Line& l : lines) {
    const ImVec2 sz = Measure(l);
    sizes.push_back(sz);
    w = std::max(w, sz.x);
    h += sz.y;
  }
  h += gap * static_cast<float>(lines.size() - 1);
  const ImVec2 size(w + pad.x * 2.0f, h + pad.y * 2.0f);
  const ImVec2 min = alignRight ? ImVec2(anchor.x - size.x, anchor.y) : anchor;
  Plate(dl, min, min + size, alpha, s);
  float y = min.y + pad.y;
  for (size_t i = 0; i < lines.size(); ++i) {
    const Line& l = lines[i];
    const float x = alignRight ? min.x + size.x - pad.x - sizes[i].x : min.x + pad.x;
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(l.color);
    c.w *= alpha;
    dl->AddText(l.font, l.size, ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(c), l.text.c_str());
    y += sizes[i].y + gap;
  }
  return size;
}

}  // namespace

void DrawHud() {
  const bool menuOpen = render::MenuOpen();
  const double now = SecondsSinceInit();
  if (g_lastMenuOpen && !menuOpen) g_watermarkUntil = std::max(g_watermarkUntil, now + kWatermarkAfterClose);
  g_lastMenuOpen = menuOpen;
  if (menuOpen) return;

  const auto& p = th::Colors();
  const ShellSettings& st = Settings();
  const auto& fonts = render::GetFonts();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float s = UiScale();
  const float fs = ImGui::GetFontSize();
  ImFont* body = FontOr(fonts.body);
  ImFont* bold = FontOr(fonts.bold);
  ImFont* mono = FontOr(fonts.mono);
  const float monoSize = fs * (render::fonts::kMonoSize / render::fonts::kBodySize);

  ImVec2 cursor = vp->WorkPos + ImVec2(14.0f * s, 12.0f * s);
  const float blockGap = 6.0f * s;

  if (st.hudWatermark) {
    const float alpha = static_cast<float>(std::clamp((g_watermarkUntil - now) / kFadeSeconds, 0.0, 1.0));
    if (alpha > 0.0f) {
      std::vector<Line> lines;
      // U+00B7 middle dot when the font has it (the embedded fallback font may not).
      const char* sep = bold->IsGlyphInFont(0x00B7) ? "  \xC2\xB7  " : "  |  ";
      lines.push_back({"CHROMA" + std::string(sep) + st.menuKey.ToString(), th::U32(p.accent), bold, fs});
      auto& b = game::Bindings::Get();
      if (mp_guard::Blocked()) {
        lines.push_back({"Multiplayer detected - inactive", th::U32(p.danger), body, fs * 0.85f});
      } else if (b.Resolving()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Resolving bindings %d%%", static_cast<int>(std::clamp(b.Progress(), 0.0f, 1.0f) * 100.0f));
        lines.push_back({buf, th::U32(p.warning), body, fs * 0.85f});
      }
      const ImVec2 size = DrawBlock(dl, cursor, lines, alpha, false, s);
      cursor.y += size.y + blockGap;
    }
  }

  if (st.hudFps) {
    const float fps = render::Fps();
    const ImVec4 c = fps >= 55.0f ? p.success : fps >= 30.0f ? p.warning : p.danger;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f FPS", static_cast<double>(fps));
    const ImVec2 size = DrawBlock(dl, cursor, {{buf, th::U32(c), mono, monoSize}}, 1.0f, false, s);
    cursor.y += size.y + blockGap;
  }

  if (st.hudPosition) {
    if (now - g_positionAt > 0.1) {   // 10 Hz is plenty for a readout
      g_position = game::player::Position();
      g_positionAt = now;
    }
    if (g_position) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "X %.1f  Y %.1f  Z %.1f", static_cast<double>(g_position->x), static_cast<double>(g_position->y),
                    static_cast<double>(g_position->z));
      const ImVec2 size = DrawBlock(dl, cursor, {{buf, th::U32(p.text), mono, monoSize}}, 1.0f, false, s);
      cursor.y += size.y + blockGap;
    }
  }

  if (st.hudActive) {
    std::vector<const features::Feature*> active;
    for (const auto& f : features::Registry::Get().All())
      if (f && f->GetKind() == features::Kind::Toggle && f->Enabled()) active.push_back(f.get());
    if (!active.empty()) {
      std::sort(active.begin(), active.end(), [](const features::Feature* a, const features::Feature* b) { return a->Name() < b->Name(); });
      std::vector<Line> lines;
      lines.push_back({"ACTIVE", th::U32(p.accent), bold, fs * 0.78f});
      for (size_t i = 0; i < active.size() && i < kMaxActiveLines; ++i) lines.push_back({active[i]->Name(), th::U32(p.text), body, fs * 0.92f});
      if (active.size() > kMaxActiveLines) {
        lines.push_back({"+" + std::to_string(active.size() - kMaxActiveLines) + " more", th::U32(p.textFaint), body, fs * 0.85f});
      }
      DrawBlock(dl, ImVec2(vp->WorkPos.x + vp->WorkSize.x - 14.0f * s, vp->WorkPos.y + 12.0f * s), lines, 1.0f, true, s);
    }
  }
}

}  // namespace cg::ui::detail
