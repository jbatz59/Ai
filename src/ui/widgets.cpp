// Math operators for ImVec2/ImVec4 must be enabled before the first include of imgui.h.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/widgets.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "core/util.h"
#include "game/bindings.h"
#include "mem/expr.h"
#include "mem/safe.h"
#include "render/fonts.h"
#include "render/overlay.h"
#include "render/theme.h"
#include "ui/internal.h"
#include "ui/notify.h"

namespace cg::ui {
namespace th = render::theme;

namespace {

// Hotkey capture ownership: only one HotkeyButton can listen at a time.
ImGuiID g_captureId = 0;
int g_captureFrame = -1;

// AddressInput evaluation cache (per widget id). Expressions may dereference memory and touch the
// module list, so they are re-evaluated on edit and at most twice a second otherwise.
struct EvalEntry {
  std::string expr;
  mem::ExprResult result;
  bool readable = false;
  double evaluatedAt = -1.0;
  int lastUsedFrame = 0;
};
std::unordered_map<ImGuiID, EvalEntry> g_evalCache;

std::vector<ImGuiID> g_cardHoverKeys;

float Luminance(const ImVec4& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
bool DarkTheme() { return Luminance(th::Colors().bg) < 0.5f; }

ImU32 WithStyleAlpha(ImU32 c) {
  ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
  v.w *= ImGui::GetStyle().Alpha;
  return ImGui::ColorConvertFloat4ToU32(v);
}

ImFont* BodyFont() {
  ImFont* f = render::GetFonts().body;
  return f != nullptr ? f : ImGui::GetFont();
}

std::string UpperAscii(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  return out;
}

const char* LabelEnd(const char* label) { return ImGui::FindRenderedTextEnd(label); }

mem::ExprResult Evaluate(const std::string& expr) {
  try {
    return mem::EvalAddress(expr, [](std::string_view name) -> std::optional<uintptr_t> {
      const auto& b = game::Bindings::Get();
      if (auto a = b.Addr(name)) return a;
      if (auto o = b.Offset(name)) return static_cast<uintptr_t>(*o);
      return std::nullopt;
    });
  } catch (const std::exception& e) {
    mem::ExprResult r;
    r.error = e.what();
    return r;
  } catch (...) {
    mem::ExprResult r;
    r.error = "evaluation failed";
    return r;
  }
}

// Text-like item placement (aligned to the current line's text baseline like ImGui::Text).
bool TextLikeItem(const ImVec2& size, ImRect* outBb) {
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems) return false;
  const ImVec2 pos(window->DC.CursorPos.x, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
  const ImRect bb(pos, pos + size);
  ImGui::ItemSize(size, 0.0f);
  if (!ImGui::ItemAdd(bb, 0)) return false;
  *outBb = bb;
  return true;
}

}  // namespace

// ---- detail helpers ------------------------------------------------------------------------------
namespace detail {

void WidgetsNewFrame() {
  const int frame = ImGui::GetFrameCount();
  if (g_captureId != 0 && frame - g_captureFrame > 1) {
    // The capturing button was not drawn last frame (page switched, menu closed): release the
    // capture so global hotkeys work again.
    hotkeys::CancelCapture();
    g_captureId = 0;
  }
  if ((frame % 300) == 0) {
    for (auto it = g_evalCache.begin(); it != g_evalCache.end();) {
      if (frame - it->second.lastUsedFrame > 600) it = g_evalCache.erase(it);
      else ++it;
    }
  }
}

bool HasIcons() { return render::GetFonts().hasIcons; }

ImU32 Col(const ImVec4& c, float alphaMul) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alphaMul)); }

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
  return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

ImVec4 WithAlpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

ImVec4 DangerText() {
  const auto& p = th::Colors();
  return DarkTheme() ? Mix(p.danger, p.text, 0.32f) : p.danger;
}

float Approach(float current, float target, float speed) {
  const float dt = ImGui::GetIO().DeltaTime;
  const float k = 1.0f - std::exp(-speed * std::max(dt, 0.0f));
  const float v = current + (target - current) * k;
  return std::fabs(v - target) < 0.001f ? target : v;
}

std::string IconLabel(const char* icon, std::string_view text) {
  if (!HasIcons() || icon == nullptr || *icon == '\0') return std::string(text);
  std::string s(icon);
  s += "  ";
  s += text;
  return s;
}

void PushBold(float sizeMul) { ImGui::PushFont(render::GetFonts().bold, ImGui::GetStyle().FontSizeBase * sizeMul); }

void PushTitle(float sizeMul) { ImGui::PushFont(render::GetFonts().title, ImGui::GetStyle().FontSizeBase * sizeMul); }

void PushMono(float sizeMul) {
  ImGui::PushFont(render::GetFonts().mono,
                  ImGui::GetStyle().FontSizeBase * sizeMul * (render::fonts::kMonoSize / render::fonts::kBodySize));
}

void PushBodySize(float sizeMul) { ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * sizeMul); }

void TextColored(const ImVec4& c, std::string_view text) {
  ImGui::PushStyleColor(ImGuiCol_Text, c);
  ImGui::TextUnformatted(text.data(), text.data() + text.size());
  ImGui::PopStyleColor();
}

void TextWrappedColored(const ImVec4& c, std::string_view text) {
  ImGui::PushStyleColor(ImGuiCol_Text, c);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(text.data(), text.data() + text.size());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

bool LabeledToggle(const char* label, bool* v, const char* help, bool enabled) {
  if (v == nullptr) return false;
  ImGui::PushID(label);
  bool changed = ToggleSwitch("##toggle", v, enabled);
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  if (!enabled) ImGui::BeginDisabled();
  ImGui::TextUnformatted(label, LabelEnd(label));
  if (enabled && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    *v = !*v;
    changed = true;
  }
  if (!enabled) ImGui::EndDisabled();
  if (help != nullptr && *help != '\0') {
    ImGui::SameLine();
    HelpMarker(help);
  }
  ImGui::PopID();
  return changed;
}

bool AccentButton(const char* label, ImVec2 size) {
  const auto& p = th::Colors();
  ImGui::PushStyleColor(ImGuiCol_Button, p.accent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.accentHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, p.accentActive);
  ImGui::PushStyleColor(ImGuiCol_Text, DarkTheme() ? p.bg : p.panel);
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(4);
  return pressed;
}

bool DangerButton(const char* label, ImVec2 size) {
  const auto& p = th::Colors();
  ImGui::PushStyleColor(ImGuiCol_Button, p.danger);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Mix(p.danger, p.text, DarkTheme() ? 0.15f : 0.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, Mix(p.danger, p.bg, 0.25f));
  ImGui::PushStyleColor(ImGuiCol_Text, DarkTheme() ? p.text : p.panel);
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(4);
  return pressed;
}

void StatusPill(const char* text, const ImVec4& color) {
  if (text == nullptr) return;
  const auto& p = th::Colors();
  ImFont* font = ImGui::GetFont();
  const float fs = ImGui::GetFontSize() * 0.86f;
  const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text);
  const float h = ImGui::GetFrameHeight() * 0.82f;
  const float dot = h * 0.16f;
  const float padX = h * 0.45f;
  const ImVec2 size(padX + dot * 2 + h * 0.3f + ts.x + padX, ImGui::GetFrameHeight());
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems) return;
  const ImRect bb(window->DC.CursorPos, window->DC.CursorPos + size);
  ImGui::ItemSize(size, ImGui::GetStyle().FramePadding.y);
  if (!ImGui::ItemAdd(bb, 0)) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float y0 = bb.Min.y + (size.y - h) * 0.5f;
  const ImVec2 a(bb.Min.x, y0), b(bb.Max.x, y0 + h);
  dl->AddRectFilled(a, b, Col(color, 0.16f), h * 0.5f);
  dl->AddRect(a, b, Col(color, 0.45f), h * 0.5f);
  const ImVec2 c(a.x + padX + dot, y0 + h * 0.5f);
  dl->AddCircleFilled(c, dot * 2.0f, Col(color, 0.25f));
  dl->AddCircleFilled(c, dot, Col(color));
  dl->AddText(font, fs, ImVec2(c.x + dot + h * 0.3f, y0 + (h - ts.y) * 0.5f), Col(Mix(color, p.text, 0.35f)), text);
}

}  // namespace detail

using detail::Col;
using detail::Mix;
using detail::WithAlpha;

// ---- public widgets ------------------------------------------------------------------------------

bool ToggleSwitch(const char* id, bool* v, bool enabled) {
  if (v == nullptr) return false;
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (window->SkipItems) return false;
  const auto& p = th::Colors();
  const float frameH = ImGui::GetFrameHeight();
  const bool ios = th::Current() == th::Preset::iOS;
  const float h = std::round(frameH * (ios ? 0.82f : 0.70f));
  const float w = std::round(h * (ios ? 1.65f : 1.85f));   // UISwitch proportions: 51 x 31

  if (!enabled) ImGui::BeginDisabled();
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, frameH), ImGuiButtonFlags_EnableNav);
  const ImGuiID itemId = ImGui::GetItemID();
  const bool toggled = pressed && enabled;
  if (toggled) {
    *v = !*v;
    ImGui::MarkItemEdited(itemId);
  }
  const bool hovered = ImGui::IsItemHovered();
  const bool visible = ImGui::IsItemVisible();

  ImGuiStorage* st = ImGui::GetStateStorage();
  const ImGuiID animKey = ImHashStr("cg.toggle", 0, itemId);
  const float target = *v ? 1.0f : 0.0f;
  float t = st->GetFloat(animKey, target);
  t = visible ? detail::Approach(t, target, 16.0f) : target;
  st->SetFloat(animKey, t);

  if (visible) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = pos.y + (frameH - h) * 0.5f;
    const ImVec2 a(pos.x, y), b(pos.x + w, y + h);
    if (ios) {
      // UISwitch: grey track, green when on, white knob with a soft shadow.
      const ImVec4 off(0.224f, 0.224f, 0.239f, 1.0f);   // #39393D
      dl->AddRectFilled(a, b, Col(Mix(off, p.success, t)), h * 0.5f);
      const float r = h * 0.5f - std::max(2.0f, h * 0.07f);
      const ImVec2 c(a.x + h * 0.5f + t * (w - h), y + h * 0.5f);
      dl->AddCircleFilled(c + ImVec2(0, 1.5f), r + 0.5f, Col(ImVec4(0, 0, 0, 0.30f)));
      dl->AddCircleFilled(c, r, Col(ImVec4(1, 1, 1, hovered ? 0.92f : 1.0f)));
    } else {
      const ImVec4 off = hovered ? Mix(p.border, p.textFaint, 0.35f) : p.border;
      const ImVec4 on = hovered ? p.accentHover : p.accent;
      dl->AddRectFilled(a, b, Col(Mix(off, on, t)), h * 0.5f);
      const float r = h * 0.5f - std::max(2.0f, h * 0.13f);
      const ImVec2 c(a.x + h * 0.5f + t * (w - h), y + h * 0.5f);
      dl->AddCircleFilled(c + ImVec2(0, 1), r, Col(WithAlpha(p.bgAlt, 0.35f)));
      dl->AddCircleFilled(c, r, Col(Mix(p.textDim, p.bg, t)));
    }
  }
  if (!enabled) ImGui::EndDisabled();
  return toggled;
}

bool HotkeyButton(const char* id, Hotkey* hk) {
  if (hk == nullptr) return false;
  const auto& p = th::Colors();
  ImGui::PushID(id);
  const ImGuiID myId = ImGui::GetID("##hotkey");
  const int frame = ImGui::GetFrameCount();
  bool changed = false;

  if (g_captureId == myId) {
    g_captureFrame = frame;
    Hotkey out = *hk;
    if (hotkeys::PollCapture(out)) {
      changed = !(out == *hk);
      *hk = out;
      g_captureId = 0;
    } else if (!hotkeys::Capturing()) {
      g_captureId = 0;   // cancelled by someone else
    }
  }
  const bool capturing = g_captureId == myId;

  std::string label;
  if (capturing) label = "Press a key... (Esc cancel, Del unbind)";
  else if (hk->Valid()) label = detail::IconLabel(CG_ICON_KEYBOARD, hk->ToString());
  else label = "Unbound";
  const float textW = ImGui::CalcTextSize(label.c_str()).x;
  label += "###hotkey";
  const float width = std::max(118.0f * detail::UiScale(), textW + ImGui::GetStyle().FramePadding.x * 2.0f);

  int colors = 0;
  if (capturing) {
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, Mix(p.accentDim, p.accentActive, pulse * 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    colors = 2;
  } else if (!hk->Valid()) {
    ImGui::PushStyleColor(ImGuiCol_Text, p.textFaint);
    colors = 1;
  }
  ImGui::PushFont(render::GetFonts().body, 0.0f);
  const bool clicked = ImGui::Button(label.c_str(), ImVec2(width, 0));
  ImGui::PopFont();
  ImGui::PopStyleColor(colors);

  if (clicked) {
    if (capturing) {
      hotkeys::CancelCapture();
      g_captureId = 0;
    } else {
      if (hotkeys::Capturing()) hotkeys::CancelCapture();
      hotkeys::BeginCapture();
      g_captureId = myId;
      g_captureFrame = frame;
    }
  }
  if (!capturing) ImGui::SetItemTooltip("Click to rebind. Right-click to clear.");
  if (ImGui::BeginPopupContextItem("##hotkeyctx")) {
    if (ImGui::MenuItem("Clear binding", nullptr, false, hk->Valid())) {
      *hk = Hotkey{};
      changed = true;
    }
    if (ImGui::MenuItem("Rebind...")) {
      if (hotkeys::Capturing()) hotkeys::CancelCapture();
      hotkeys::BeginCapture();
      g_captureId = myId;
      g_captureFrame = frame;
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}

void HelpMarker(const char* text) {
  if (text == nullptr) return;
  ImGui::PushStyleColor(ImGuiCol_Text, th::Colors().textFaint);
  Icon(CG_ICON_HELP, "(?)");
  ImGui::PopStyleColor();
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

void SectionHeader(const char* text) {
  if (text == nullptr) return;
  const auto& p = th::Colors();
  ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
  const std::string upper = UpperAscii(text);
  detail::PushBold(0.80f);
  detail::TextColored(p.accent, upper);
  detail::PopFont();
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  const float x0 = max.x + ImGui::GetStyle().ItemSpacing.x;
  const float x1 = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
  if (x1 > x0) {
    const float y = std::floor((min.y + max.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(x1, y + 1.0f), Col(p.accentDim),
                                                        Col(p.accentDim, 0.0f), Col(p.accentDim, 0.0f), Col(p.accentDim));
  }
}

void StatusDot(ImU32 color, const char* tooltip) {
  const float h = ImGui::GetTextLineHeight();
  ImRect bb;
  if (!TextLikeItem(ImVec2(h * 0.75f, h), &bb)) return;
  const ImVec2 c = bb.GetCenter();
  const float r = std::max(2.5f, h * 0.2f);
  ImVec4 glow = ImGui::ColorConvertU32ToFloat4(color);
  glow.w *= 0.22f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddCircleFilled(c, r * 1.9f, WithStyleAlpha(ImGui::ColorConvertFloat4ToU32(glow)));
  dl->AddCircleFilled(c, r, WithStyleAlpha(color));
  if (tooltip != nullptr && *tooltip != '\0' && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", tooltip);
}

bool SearchBox(const char* id, char* buf, size_t bufSize, const char* hint) {
  if (buf == nullptr || bufSize == 0) return false;
  const auto& p = th::Colors();
  const ImGuiStyle& style = ImGui::GetStyle();
  ImGuiContext& g = *GImGui;
  const float width = (g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasWidth) ? ImGui::CalcItemWidth()
                                                                                    : ImGui::GetContentRegionAvail().x;
  ImGui::PushID(id);
  const float iconW = detail::HasIcons() ? ImGui::GetFontSize() * 1.45f : 0.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x + iconW, style.FramePadding.y));
  ImGui::SetNextItemWidth(std::max(width, ImGui::GetFontSize() * 6.0f));
  ImGui::SetNextItemAllowOverlap();
  bool changed = ImGui::InputTextWithHint("##search", hint != nullptr ? hint : "", buf, bufSize, ImGuiInputTextFlags_EscapeClearsAll);
  ImGui::PopStyleVar();
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  const ImVec2 after = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (detail::HasIcons()) {
    dl->AddText(BodyFont(), ImGui::GetFontSize(), ImVec2(min.x + style.FramePadding.x, min.y + style.FramePadding.y),
                Col(p.textFaint), CG_ICON_SEARCH);
  }
  if (buf[0] != '\0') {
    const float sz = max.y - min.y;
    ImGui::SetCursorScreenPos(ImVec2(max.x - sz, min.y));
    if (ImGui::InvisibleButton("##clear", ImVec2(sz, sz))) {
      buf[0] = '\0';
      changed = true;
    }
    const bool hov = ImGui::IsItemHovered();
    ImGui::SetItemTooltip("Clear");
    const ImVec2 c(max.x - sz * 0.5f, min.y + sz * 0.5f);
    const float e = sz * 0.16f;
    const ImU32 col = Col(hov ? p.text : p.textFaint);
    dl->AddLine(c - ImVec2(e, e), c + ImVec2(e, e), col, 1.5f);
    dl->AddLine(c + ImVec2(-e, e), c + ImVec2(e, -e), col, 1.5f);
    ImGui::SetCursorScreenPos(after);
  }
  ImGui::PopID();
  return changed;
}

bool AddressInput(const char* id, std::string* expr, uintptr_t* out) {
  if (expr == nullptr) return false;
  const auto& p = th::Colors();
  const ImGuiStyle& style = ImGui::GetStyle();
  ImGuiContext& g = *GImGui;
  const float fullW = (g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasWidth) ? ImGui::CalcItemWidth()
                                                                                   : ImGui::GetContentRegionAvail().x;
  ImGui::PushID(id);
  const ImGuiID key = ImGui::GetID("##addr");

  char buf[512];
  const size_t n = std::min(expr->size(), sizeof(buf) - 1);
  std::memcpy(buf, expr->data(), n);
  buf[n] = '\0';

  const float resultW = std::clamp(fullW * 0.40f, 130.0f * detail::UiScale(), 240.0f * detail::UiScale());
  ImGui::SetNextItemWidth(std::max(fullW - resultW - style.ItemSpacing.x, ImGui::GetFontSize() * 6.0f));
  detail::PushMono();
  const bool edited = ImGui::InputTextWithHint("##addr", "address, module+offset, [ptr]+off, Symbol", buf, sizeof(buf),
                                               ImGuiInputTextFlags_AutoSelectAll);
  detail::PopFont();
  const bool submitted = ImGui::IsItemDeactivated() &&
                         (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
  if (edited) *expr = buf;

  EvalEntry& e = g_evalCache[key];
  e.lastUsedFrame = ImGui::GetFrameCount();
  const double now = ImGui::GetTime();
  if (edited || e.expr != *expr || e.evaluatedAt < 0.0 || now - e.evaluatedAt > 0.5) {
    e.expr = *expr;
    const std::string trimmed = util::Trim(*expr);
    if (trimmed.empty()) {
      e.result = {};
    } else {
      e.result = Evaluate(trimmed);
      e.readable = e.result.ok && mem::IsReadable(e.result.value, 1);
    }
    e.evaluatedAt = now;
  }

  ImGui::SameLine();
  const bool empty = util::Trim(*expr).empty();
  if (empty) {
    ImGui::AlignTextToFramePadding();
    detail::TextColored(p.textFaint, "enter an address");
  } else if (e.result.ok) {
    ImGui::AlignTextToFramePadding();
    StatusDot(th::U32(e.readable ? p.success : p.warning), e.readable ? "Readable memory" : "Not readable (unmapped or guarded)");
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    detail::PushMono();
    const std::string hex = util::Hex(e.result.value);
    CopyableText(hex.c_str());
    detail::PopFont();
  } else {
    ImGui::AlignTextToFramePadding();
    StatusDot(th::U32(p.danger), nullptr);
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    const std::string& err = e.result.error.empty() ? std::string("invalid expression") : e.result.error;
    ImGui::PushStyleColor(ImGuiCol_Text, detail::DangerText());
    const float avail = ImGui::GetContentRegionAvail().x;
    const char* end = err.c_str() + err.size();
    // Single line, clipped: the full message is in the tooltip.
    const ImVec2 at = ImGui::GetCursorScreenPos() + ImVec2(0.0f, ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), at, at + ImVec2(avail, ImGui::GetTextLineHeight()), at.x + avail,
                              err.c_str(), end, nullptr);
    ImGui::Dummy(ImVec2(avail, ImGui::GetTextLineHeight()));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", err.c_str());
  }
  ImGui::PopID();

  const bool userCommitted = (edited || submitted) && e.result.ok;
  if (userCommitted && out != nullptr) *out = e.result.value;
  return userCommitted;
}

void CopyableText(const char* text) {
  if (text == nullptr) return;
  ImGui::TextUnformatted(text);
  if (ImGui::IsItemHovered()) {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, max.y), ImVec2(max.x, max.y), Col(th::Colors().accent, 0.8f), 1.0f);
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::SetItemTooltip("Click to copy");
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    ImGui::SetClipboardText(text);
    std::string shown(text);
    if (shown.size() > 96) shown = shown.substr(0, 93) + "...";
    notify::Push(notify::Kind::Success, "Copied to clipboard", std::move(shown), 1.8f);
  }
}

bool IconButton(const char* icon, const char* fallbackText, const char* tooltip, ImVec2 size) {
  const bool icons = detail::HasIcons() && icon != nullptr && *icon != '\0';
  std::string label = icons ? std::string(icon) : std::string(fallbackText != nullptr ? fallbackText : "?");
  label += "##";
  label += tooltip != nullptr ? tooltip : (fallbackText != nullptr ? fallbackText : "");
  if (icons && size.x == 0.0f && size.y == 0.0f) {
    const float f = ImGui::GetFrameHeight();
    size = ImVec2(f, f);
  }
  const auto& p = th::Colors();
  ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(p.panelHover, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Mix(p.panelHover, p.accent, 0.22f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, Mix(p.panelHover, p.accent, 0.42f));
  if (icons) ImGui::PushFont(render::GetFonts().body, 0.0f);
  const bool pressed = ImGui::Button(label.c_str(), size);
  if (icons) ImGui::PopFont();
  ImGui::PopStyleColor(3);
  if (tooltip != nullptr && *tooltip != '\0') ImGui::SetItemTooltip("%s", tooltip);
  return pressed;
}

void Badge(const char* text, ImU32 bg, ImU32 fg) {
  if (text == nullptr) return;
  ImFont* font = render::GetFonts().bold != nullptr ? render::GetFonts().bold : ImGui::GetFont();
  const float fs = ImGui::GetFontSize() * 0.78f;
  const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text);
  const float h = ImGui::GetTextLineHeight();
  ImRect bb;
  if (!TextLikeItem(ImVec2(ts.x + h * 0.8f, h), &bb)) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float inset = std::max(1.0f, h * 0.06f);
  dl->AddRectFilled(ImVec2(bb.Min.x, bb.Min.y + inset), ImVec2(bb.Max.x, bb.Max.y - inset), WithStyleAlpha(bg), h * 0.5f);
  dl->AddText(font, fs, ImVec2(bb.Min.x + (bb.GetWidth() - ts.x) * 0.5f, bb.Min.y + (h - ts.y) * 0.5f), WithStyleAlpha(fg), text);
}

void Icon(const char* icon, const char* fallback) {
  if (detail::HasIcons() && icon != nullptr && *icon != '\0') {
    ImGui::PushFont(render::GetFonts().body, 0.0f);
    ImGui::TextUnformatted(icon);
    ImGui::PopFont();
  } else if (fallback != nullptr && *fallback != '\0') {
    ImGui::TextUnformatted(fallback);
  } else {
    // Keep layout stable for callers that do Icon(); SameLine(); Text().
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
  }
}

bool BeginCard(const char* id, ImVec2 size) {
  const auto& p = th::Colors();
  const float s = detail::UiScale();
  const ImGuiID childId = ImGui::GetID(id);
  const ImGuiID hoverKey = ImHashStr("cg.card.hover", 0, childId);
  const float hover = ImGui::GetStateStorage()->GetFloat(hoverKey, 0.0f);
  g_cardHoverKeys.push_back(hoverKey);

  const bool ios = th::Current() == th::Preset::iOS;
  // iOS grouped cells: no outline, a slightly lighter fill on hover instead.
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ios ? Mix(p.panel, p.panelHover, hover * 0.6f) : p.panel);
  ImGui::PushStyleColor(ImGuiCol_Border, Mix(p.border, p.accentDim, hover));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, (ios ? 12.0f : 8.0f) * s);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ios ? ImVec2(14.0f * s, 11.0f * s) : ImVec2(12.0f * s, 10.0f * s));
  ImGuiChildFlags flags = ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding | (ios ? 0 : ImGuiChildFlags_Borders);
  ImGuiWindowFlags wflags = 0;
  if (size.y == 0.0f) {
    flags |= ImGuiChildFlags_AutoResizeY;
    wflags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  }
  const bool visible = ImGui::BeginChild(id, size, flags, wflags);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
  return visible;
}

void EndCard() {
  if (g_cardHoverKeys.empty()) return;   // unbalanced call; nothing sensible to close
  const ImGuiID hoverKey = g_cardHoverKeys.back();
  g_cardHoverKeys.pop_back();
  const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
  ImGui::EndChild();
  ImGuiStorage* st = ImGui::GetStateStorage();
  st->SetFloat(hoverKey, detail::Approach(st->GetFloat(hoverKey, 0.0f), hovered ? 1.0f : 0.0f, 14.0f));
}

}  // namespace cg::ui
