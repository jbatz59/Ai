// Math operators for ImVec2/ImVec4 must be enabled before the first include of imgui.h.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/notify.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "render/fonts.h"
#include "render/overlay.h"
#include "render/theme.h"
#include "ui/internal.h"
#include "ui/widgets.h"

namespace cg::ui::notify {
namespace {

namespace th = render::theme;

constexpr size_t kMaxPending = 64;     // producers flooding faster than we draw: oldest dropped
constexpr size_t kMaxQueued = 32;      // accepted but not yet on screen
constexpr int kMaxVisible = 6;
constexpr int kSlots = 12;             // ImGui windows are reused so a long session doesn't accumulate them
constexpr float kAppearSeconds = 0.22f;
constexpr float kExitSeconds = 0.28f;

struct Toast {
  Kind kind = Kind::Info;
  std::string title;
  std::string text;
  float duration = 3.5f;
  float age = 0.0f;      // seconds on screen, paused while hovered
  float appear = 0.0f;   // 0..1
  float exit = 0.0f;     // 0..1 once dismissed
  bool dismissed = false;
  float y = -1.0f;       // animated distance from the bottom of the stack
  float height = 0.0f;   // measured last frame
  int count = 1;
  int slot = -1;
};

std::mutex g_mutex;
std::vector<Toast> g_pending;   // guarded by g_mutex

std::vector<Toast> g_active;    // render thread only
std::array<bool, kSlots> g_slotUsed{};

float EaseOut(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  const float u = 1.0f - t;
  return 1.0f - u * u * u;
}

ImVec4 KindColor(Kind k) {
  const auto& p = th::Colors();
  switch (k) {
    case Kind::Success: return p.success;
    case Kind::Warning: return p.warning;
    case Kind::Error: return p.danger;
    case Kind::Info:
    default: return p.info;
  }
}

const char* KindIcon(Kind k) {
  switch (k) {
    case Kind::Success: return CG_ICON_CHECK;
    case Kind::Warning: return CG_ICON_WARNING;
    case Kind::Error: return CG_ICON_ERROR;
    case Kind::Info:
    default: return CG_ICON_INFO;
  }
}

int AcquireSlot() {
  for (int i = 0; i < kSlots; ++i) {
    if (!g_slotUsed[static_cast<size_t>(i)]) {
      g_slotUsed[static_cast<size_t>(i)] = true;
      return i;
    }
  }
  return -1;
}

void ReleaseSlot(int slot) {
  if (slot >= 0 && slot < kSlots) g_slotUsed[static_cast<size_t>(slot)] = false;
}

void Accept(std::vector<Toast>& incoming) {
  const bool enabled = detail::Settings().notifications;
  for (Toast& t : incoming) {
    // With notifications off only warnings and errors get through.
    if (!enabled && (t.kind == Kind::Info || t.kind == Kind::Success)) continue;
    auto dup = std::find_if(g_active.begin(), g_active.end(), [&](const Toast& a) {
      return !a.dismissed && a.kind == t.kind && a.title == t.title && a.text == t.text;
    });
    if (dup != g_active.end()) {
      dup->count++;
      dup->age = 0.0f;
      dup->duration = std::max(dup->duration, t.duration);
      continue;
    }
    g_active.push_back(std::move(t));
  }
  // Too many waiting for a slot: drop the oldest that never made it on screen.
  size_t waiting = 0;
  for (const Toast& t : g_active) waiting += t.slot < 0 ? 1 : 0;
  for (auto it = g_active.begin(); waiting > kMaxQueued && it != g_active.end();) {
    if (it->slot < 0) {
      it = g_active.erase(it);
      --waiting;
    } else {
      ++it;
    }
  }
}

void DrawToast(Toast& t, const ImVec2& anchor, float alpha, bool interactive) {
  const auto& p = th::Colors();
  const float s = detail::UiScale();
  const float width = 340.0f * s;
  const float barW = 4.0f * s;
  const ImVec4 kc = KindColor(t.kind);

  char name[32];
  std::snprintf(name, sizeof(name), "##cg.toast.%d", t.slot);
  ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (!interactive) flags |= ImGuiWindowFlags_NoInputs;

  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::clamp(alpha, 0.0f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * s);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * s + barW, 10.0f * s));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, detail::WithAlpha(p.panel, 0.97f));
  ImGui::PushStyleColor(ImGuiCol_Border, detail::Mix(p.border, kc, 0.35f));

  if (ImGui::Begin(name, nullptr, flags)) {
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    dl->AddRectFilled(wp, ImVec2(wp.x + barW, wp.y + ws.y), detail::Col(kc), 8.0f * s, ImDrawFlags_RoundCornersLeft);

    ImGui::PushStyleColor(ImGuiCol_Text, kc);
    if (detail::HasIcons()) Icon(KindIcon(t.kind));
    else StatusDot(th::U32(kc));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    const float wrapX = width - ImGui::GetStyle().WindowPadding.x;
    ImGui::PushTextWrapPos(wrapX);
    detail::PushBold();
    ImGui::TextUnformatted(t.title.c_str());
    detail::PopFont();
    if (t.count > 1) {
      ImGui::SameLine();
      char buf[24];
      std::snprintf(buf, sizeof(buf), "x%d", t.count);
      detail::TextColored(p.textFaint, buf);
    }
    if (!t.text.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, p.textDim);
      ImGui::TextUnformatted(t.text.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();

    const float remaining = t.dismissed ? 0.0f : std::clamp(1.0f - t.age / std::max(t.duration, 0.1f), 0.0f, 1.0f);
    if (remaining > 0.0f) {
      const float x0 = wp.x + barW + 6.0f * s;
      const float x1 = wp.x + ws.x - 8.0f * s;
      const float y = wp.y + ws.y - 3.0f * s;
      dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * remaining, y + 1.5f * s), detail::Col(kc, 0.55f), 1.0f);
    }

    const bool hovered = interactive && ImGui::IsWindowHovered();
    if (hovered) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) t.dismissed = true;
    } else if (!t.dismissed) {
      t.age += ImGui::GetIO().DeltaTime;
    }
    t.height = ws.y;
  }
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(4);
}

}  // namespace

void Push(Kind kind, std::string title, std::string text, float seconds) {
  try {
    Toast t;
    t.kind = kind;
    t.title = std::move(title);
    t.text = std::move(text);
    t.duration = std::isfinite(seconds) ? std::clamp(seconds, 1.0f, 60.0f) : 3.5f;
    std::lock_guard lock(g_mutex);
    if (g_pending.size() >= kMaxPending) g_pending.erase(g_pending.begin());
    g_pending.push_back(std::move(t));
  } catch (...) {
    // Out of memory while toasting: nothing useful left to report.
  }
}

void Draw() {
  if (ImGui::GetCurrentContext() == nullptr) return;
  std::vector<Toast> incoming;
  {
    std::lock_guard lock(g_mutex);
    incoming.swap(g_pending);
  }
  if (!incoming.empty()) Accept(incoming);
  if (g_active.empty()) return;

  const float dt = ImGui::GetIO().DeltaTime;
  const float s = detail::UiScale();
  const float margin = 18.0f * s;
  const float spacing = 8.0f * s;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const ImVec2 corner(vp->WorkPos.x + vp->WorkSize.x - margin, vp->WorkPos.y + vp->WorkSize.y - margin);
  const bool interactive = render::MenuOpen();

  // Newest at the bottom. Beyond kMaxVisible the oldest live toasts are retired early.
  int live = 0;
  for (int i = static_cast<int>(g_active.size()) - 1; i >= 0; --i) {
    Toast& t = g_active[static_cast<size_t>(i)];
    if (t.dismissed) continue;
    if (t.slot < 0) {
      if (live >= kMaxVisible) {
        t.dismissed = true;   // older than everything on screen: it would only ever flash by
        continue;
      }
      t.slot = AcquireSlot();
      if (t.slot < 0) continue;   // all windows busy with exit animations; retry next frame
    }
    if (++live > kMaxVisible) t.dismissed = true;
    else if (t.age >= t.duration) t.dismissed = true;
  }

  float stack = 0.0f;
  for (int i = static_cast<int>(g_active.size()) - 1; i >= 0; --i) {
    Toast& t = g_active[static_cast<size_t>(i)];
    if (t.slot < 0) continue;
    if (t.dismissed) t.exit = std::min(1.0f, t.exit + dt / kExitSeconds);
    else t.appear = std::min(1.0f, t.appear + dt / kAppearSeconds);

    const float height = t.height > 0.0f ? t.height : ImGui::GetFrameHeightWithSpacing() * 1.6f;
    const float target = stack;
    t.y = t.y < 0.0f ? target : detail::Approach(t.y, target, 14.0f);
    const float in = EaseOut(t.appear);
    const float out = EaseOut(t.exit);
    const float slide = (1.0f - in) * 120.0f * s + out * 60.0f * s;
    DrawToast(t, ImVec2(corner.x + slide, corner.y - t.y), in * (1.0f - out), interactive);
    stack += (height + spacing) * (1.0f - out);
  }

  for (auto it = g_active.begin(); it != g_active.end();) {
    if (it->dismissed && (it->exit >= 1.0f || it->slot < 0)) {
      ReleaseSlot(it->slot);
      it = g_active.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace cg::ui::notify
