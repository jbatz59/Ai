#include "render/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "core/config.h"

namespace cg::render::theme {
namespace {

constexpr ImVec4 Rgb(uint32_t rgb, float a = 1.0f) {
  return ImVec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                static_cast<float>(rgb & 0xFF) / 255.0f, a);
}

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
  return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

ImVec4 Alpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

struct PresetDef {
  Palette pal;
  bool dark;
};

PresetDef MakeNoir() {
  Palette p{};
  p.bg = Rgb(0x121214);
  p.bgAlt = Rgb(0x0C0C0E);
  p.panel = Rgb(0x1A1A1E);
  p.panelHover = Rgb(0x25242A);
  p.border = Rgb(0x2F2C29);
  p.text = Rgb(0xE9E3D5);
  p.textDim = Rgb(0xA49E92);
  p.textFaint = Rgb(0x6C675F);
  p.accent = Rgb(0xC8A15A);
  p.accentHover = Rgb(0xDBB774);
  p.accentActive = Rgb(0xAE8742);
  p.accentDim = Rgb(0x5E4B2B);
  p.danger = Rgb(0xA8383A);
  p.warning = Rgb(0xD39236);
  p.success = Rgb(0x6FA46A);
  p.info = Rgb(0x6A93B8);
  return {p, true};
}

PresetDef MakeMidnight() {
  Palette p{};
  p.bg = Rgb(0x0E1118);
  p.bgAlt = Rgb(0x090B11);
  p.panel = Rgb(0x151A25);
  p.panelHover = Rgb(0x1F2635);
  p.border = Rgb(0x283146);
  p.text = Rgb(0xDDE4EF);
  p.textDim = Rgb(0x96A2B6);
  p.textFaint = Rgb(0x5D677B);
  p.accent = Rgb(0x86A9CC);
  p.accentHover = Rgb(0xA0BEDD);
  p.accentActive = Rgb(0x6C91B7);
  p.accentDim = Rgb(0x2E425A);
  p.danger = Rgb(0xB5485A);
  p.warning = Rgb(0xD5A043);
  p.success = Rgb(0x52A57E);
  p.info = Rgb(0x6DB5DA);
  return {p, true};
}

PresetDef MakeBordeaux() {
  Palette p{};
  p.bg = Rgb(0x170D11);
  p.bgAlt = Rgb(0x10080B);
  p.panel = Rgb(0x221318);
  p.panelHover = Rgb(0x2E1A21);
  p.border = Rgb(0x42242C);
  p.text = Rgb(0xF1E4D7);
  p.textDim = Rgb(0xB79F93);
  p.textFaint = Rgb(0x7C625A);
  p.accent = Rgb(0xD6B04A);
  p.accentHover = Rgb(0xE6C566);
  p.accentActive = Rgb(0xB8932F);
  p.accentDim = Rgb(0x5E4920);
  p.danger = Rgb(0xC9483C);
  p.warning = Rgb(0xE3A247);
  p.success = Rgb(0x83A85F);
  p.info = Rgb(0x86A2C4);
  return {p, true};
}

PresetDef MakeLight() {
  Palette p{};
  p.bg = Rgb(0xF1EADB);
  p.bgAlt = Rgb(0xE6DCC7);
  p.panel = Rgb(0xF9F5EB);
  p.panelHover = Rgb(0xEAE0CB);
  p.border = Rgb(0xCDBEA0);
  p.text = Rgb(0x28221D);
  p.textDim = Rgb(0x5C5248);
  p.textFaint = Rgb(0x8E8272);
  p.accent = Rgb(0x8A6420);
  p.accentHover = Rgb(0xA0772C);
  p.accentActive = Rgb(0x6D4E17);
  p.accentDim = Rgb(0xDCC9A0);
  p.danger = Rgb(0x8E2B2B);
  p.warning = Rgb(0xA4621A);
  p.success = Rgb(0x3D7543);
  p.info = Rgb(0x2E5E86);
  return {p, false};
}

// Modern dark glass with an accent that the Chroma tick re-tints every frame.
PresetDef MakeChroma() {
  Palette p{};
  p.bg = Rgb(0x0D0D12, 0.94f);
  p.bgAlt = Rgb(0x08080C, 0.96f);
  p.panel = Rgb(0x15151D);
  p.panelHover = Rgb(0x1E1E2A);
  p.border = Rgb(0x2A2A38);
  p.text = Rgb(0xEEEEF5);
  p.textDim = Rgb(0x9C9CB2);
  p.textFaint = Rgb(0x5F5F75);
  p.accent = Rgb(0xB45CFF);
  p.accentHover = Rgb(0xC983FF);
  p.accentActive = Rgb(0x9B3FEA);
  p.accentDim = Rgb(0x3D2259);
  p.danger = Rgb(0xFF4D6A);
  p.warning = Rgb(0xFFB547);
  p.success = Rgb(0x3DDC97);
  p.info = Rgb(0x4DA3FF);
  return {p, true};
}

// iOS dark mode: system colours from Apple's Human Interface Guidelines (dark appearance).
PresetDef MakeIOS() {
  Palette p{};
  p.bg = Rgb(0x000000, 0.94f);       // systemBackground
  p.bgAlt = Rgb(0x000000);
  p.panel = Rgb(0x1C1C1E);           // secondarySystemGroupedBackground (cells)
  p.panelHover = Rgb(0x2C2C2E);      // tertiary fill
  p.border = Rgb(0x38383A);          // separator
  p.text = Rgb(0xFFFFFF);            // label
  p.textDim = Rgb(0x98989F);         // secondaryLabel
  p.textFaint = Rgb(0x5C5C62);       // tertiaryLabel
  p.accent = Rgb(0x0A84FF);          // systemBlue
  p.accentHover = Rgb(0x409CFF);
  p.accentActive = Rgb(0x0070E0);
  p.accentDim = Rgb(0x0A3A6B);
  p.danger = Rgb(0xFF453A);          // systemRed
  p.warning = Rgb(0xFF9F0A);         // systemOrange
  p.success = Rgb(0x30D158);         // systemGreen
  p.info = Rgb(0x64D2FF);            // systemTeal
  return {p, true};
}

// Neon Glass: deep navy glass, violet accent; gradients run violet -> cyan -> pink.
PresetDef MakeNeon() {
  Palette p{};
  p.bg = Rgb(0x0A0C18, 0.92f);
  p.bgAlt = Rgb(0x0E1124, 0.94f);
  p.panel = Rgb(0x151933, 0.78f);
  p.panelHover = Rgb(0x1E2346);
  p.border = Rgb(0x2A3062);
  p.text = Rgb(0xF2F3FF);
  p.textDim = Rgb(0xA7ACD9);
  p.textFaint = Rgb(0x666D9C);
  p.accent = Rgb(0x8B5CF6);
  p.accentHover = Rgb(0xA78BFA);
  p.accentActive = Rgb(0x7C3AED);
  p.accentDim = Rgb(0x3B2C7A);
  p.danger = Rgb(0xFB7185);
  p.warning = Rgb(0xFBBF24);
  p.success = Rgb(0x34D399);
  p.info = Rgb(0x22D3EE);
  return {p, true};
}

PresetDef Make(Preset p) {
  switch (p) {
    case Preset::Neon: return MakeNeon();
    case Preset::Midnight: return MakeMidnight();
    case Preset::Bordeaux: return MakeBordeaux();
    case Preset::Light: return MakeLight();
    case Preset::Chroma: return MakeChroma();
    case Preset::iOS: return MakeIOS();
    case Preset::Noir:
    default: return MakeNoir();
  }
}

Preset Sanitize(Preset p) {
  switch (p) {
    case Preset::Noir:
    case Preset::Midnight:
    case Preset::Bordeaux:
    case Preset::Light:
    case Preset::Chroma:
    case Preset::iOS:
    case Preset::Neon: return p;
  }
  return kDefaultPreset;
}

Palette g_palette = MakeNeon().pal;
Preset g_current = Preset::Neon;
float g_hue = 0.75f;
float g_saturation = 0.85f;
float g_speed = 0.12f;
int g_configRefresh = 0;

ImVec4 Hsv(float h, float s, float v, float a = 1.0f) {
  float r, g, b;
  ImGui::ColorConvertHSVtoRGB(h - std::floor(h), std::clamp(s, 0.0f, 1.0f), std::clamp(v, 0.0f, 1.0f), r, g, b);
  return ImVec4(r, g, b, a);
}

// Bump when Dear ImGui adds style colors: SetColors() must assign every entry.
static_assert(ImGuiCol_COUNT == 61, "Dear ImGui color list changed - update theme.cpp SetColors()");

void SetColors(ImVec4* c, const Palette& p, bool dark) {
  const ImVec4 clear = Alpha(p.bg, 0.0f);
  const ImVec4 dim = dark ? Alpha(p.bgAlt, 0.62f) : Alpha(p.text, 0.28f);

  c[ImGuiCol_Text] = p.text;
  c[ImGuiCol_TextDisabled] = p.textFaint;
  c[ImGuiCol_WindowBg] = p.bg;
  c[ImGuiCol_ChildBg] = clear;
  c[ImGuiCol_PopupBg] = Alpha(Mix(p.bg, p.panel, 0.6f), 0.98f);
  c[ImGuiCol_Border] = p.border;
  c[ImGuiCol_BorderShadow] = clear;
  c[ImGuiCol_FrameBg] = p.bgAlt;
  c[ImGuiCol_FrameBgHovered] = Mix(p.bgAlt, p.panelHover, 0.7f);
  c[ImGuiCol_FrameBgActive] = Mix(p.panelHover, p.accentDim, 0.45f);
  c[ImGuiCol_TitleBg] = p.bgAlt;
  c[ImGuiCol_TitleBgActive] = Mix(p.bgAlt, p.accentDim, 0.40f);
  c[ImGuiCol_TitleBgCollapsed] = Alpha(p.bgAlt, 0.80f);
  c[ImGuiCol_MenuBarBg] = p.bgAlt;
  c[ImGuiCol_ScrollbarBg] = Alpha(p.bgAlt, 0.35f);
  c[ImGuiCol_ScrollbarGrab] = p.border;
  c[ImGuiCol_ScrollbarGrabHovered] = Mix(p.border, p.accentDim, 0.6f);
  c[ImGuiCol_ScrollbarGrabActive] = p.accentActive;
  c[ImGuiCol_CheckMark] = p.accent;
  c[ImGuiCol_CheckboxSelectedBg] = Mix(p.bgAlt, p.accentDim, 0.55f);
  c[ImGuiCol_SliderGrab] = p.accentActive;
  c[ImGuiCol_SliderGrabActive] = p.accentHover;
  c[ImGuiCol_Button] = p.panelHover;
  c[ImGuiCol_ButtonHovered] = Mix(p.panelHover, p.accent, 0.28f);
  c[ImGuiCol_ButtonActive] = Mix(p.panelHover, p.accent, 0.48f);
  c[ImGuiCol_Header] = Alpha(p.accent, 0.20f);
  c[ImGuiCol_HeaderHovered] = Alpha(p.accent, 0.30f);
  c[ImGuiCol_HeaderActive] = Alpha(p.accent, 0.42f);
  c[ImGuiCol_Separator] = p.border;
  c[ImGuiCol_SeparatorHovered] = p.accentDim;
  c[ImGuiCol_SeparatorActive] = p.accent;
  c[ImGuiCol_ResizeGrip] = Alpha(p.accent, 0.14f);
  c[ImGuiCol_ResizeGripHovered] = Alpha(p.accent, 0.45f);
  c[ImGuiCol_ResizeGripActive] = Alpha(p.accent, 0.80f);
  c[ImGuiCol_InputTextCursor] = p.accentHover;
  c[ImGuiCol_TabHovered] = Mix(p.panel, p.accent, 0.32f);
  c[ImGuiCol_Tab] = p.panel;
  c[ImGuiCol_TabSelected] = Mix(p.panel, p.accent, 0.20f);
  c[ImGuiCol_TabSelectedOverline] = p.accent;
  c[ImGuiCol_TabDimmed] = p.bgAlt;
  c[ImGuiCol_TabDimmedSelected] = p.panel;
  c[ImGuiCol_TabDimmedSelectedOverline] = p.accentDim;
  c[ImGuiCol_PlotLines] = p.accent;
  c[ImGuiCol_PlotLinesHovered] = p.accentHover;
  c[ImGuiCol_PlotHistogram] = p.accent;
  c[ImGuiCol_PlotHistogramHovered] = p.accentHover;
  c[ImGuiCol_TableHeaderBg] = p.panelHover;
  c[ImGuiCol_TableBorderStrong] = p.border;
  c[ImGuiCol_TableBorderLight] = Mix(p.border, p.panel, 0.5f);
  c[ImGuiCol_TableRowBg] = clear;
  c[ImGuiCol_TableRowBgAlt] = Alpha(p.text, dark ? 0.025f : 0.04f);
  c[ImGuiCol_TextLink] = dark ? p.accentHover : p.accent;
  c[ImGuiCol_TextSelectedBg] = Alpha(p.accent, 0.35f);
  c[ImGuiCol_TreeLines] = p.border;
  c[ImGuiCol_DragDropTarget] = p.accent;
  c[ImGuiCol_DragDropTargetBg] = Alpha(p.accent, 0.12f);
  c[ImGuiCol_UnsavedMarker] = p.accent;
  c[ImGuiCol_NavCursor] = p.accentHover;
  c[ImGuiCol_NavWindowingHighlight] = Alpha(p.accent, 0.70f);
  c[ImGuiCol_NavWindowingDimBg] = dim;
  c[ImGuiCol_ModalWindowDimBg] = dim;
}

}  // namespace

void Apply(Preset preset, float uiScale) {
  preset = Sanitize(preset);
  const PresetDef def = Make(preset);
  g_palette = def.pal;
  g_current = preset;
  if (ImGui::GetCurrentContext() == nullptr) return;

  const float scale = std::clamp(uiScale > 0.0f ? uiScale : 1.0f, 0.5f, 3.0f);
  ImGuiStyle& style = ImGui::GetStyle();
  // Keep the values owned by the font system / platform layer across the reset.
  const float fontSizeBase = style.FontSizeBase;
  const float fontScaleDpi = style.FontScaleDpi;
  style = ImGuiStyle();
  style.FontSizeBase = fontSizeBase;
  style.FontScaleDpi = fontScaleDpi;

  style.WindowPadding = ImVec2(12, 12);
  style.FramePadding = ImVec2(10, 6);
  style.CellPadding = ImVec2(8, 5);
  style.ItemSpacing = ImVec2(10, 8);
  style.ItemInnerSpacing = ImVec2(8, 6);
  style.TouchExtraPadding = ImVec2(0, 0);
  style.IndentSpacing = 20;
  style.ScrollbarSize = 13;
  style.ScrollbarPadding = 2;
  style.GrabMinSize = 12;

  style.WindowBorderSize = 1;
  style.ChildBorderSize = 1;
  style.PopupBorderSize = 1;
  style.FrameBorderSize = 0;
  style.TabBorderSize = 0;
  style.TabBarBorderSize = 1;
  style.TabBarOverlineSize = 2;
  style.SeparatorTextBorderSize = 1;

  style.WindowRounding = 10;
  style.ChildRounding = 8;
  style.FrameRounding = 6;
  style.PopupRounding = 8;
  style.ScrollbarRounding = 9;
  style.GrabRounding = 6;
  style.TabRounding = 6;
  style.ImageRounding = 6;

  style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
  style.WindowMenuButtonPosition = ImGuiDir_None;
  style.WindowMinSize = ImVec2(160, 96);
  style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
  style.SeparatorTextPadding = ImVec2(12, 4);
  style.DisabledAlpha = 0.45f;
  style.DisplaySafeAreaPadding = ImVec2(4, 4);

  if (preset == Preset::Chroma) {
    style.WindowRounding = 14;
    style.ChildRounding = 10;
    style.FrameRounding = 8;
    style.PopupRounding = 10;
    style.GrabRounding = 8;
    style.TabRounding = 8;
    style.FramePadding = ImVec2(12, 7);
    style.ItemSpacing = ImVec2(10, 9);
    style.WindowBorderSize = 0;   // the animated chroma border replaces it
    style.PopupBorderSize = 1;
  }

  if (preset == Preset::iOS) {
    style.WindowPadding = ImVec2(16, 16);
    style.FramePadding = ImVec2(12, 8);
    style.ItemSpacing = ImVec2(10, 10);
    style.WindowRounding = 20;
    style.ChildRounding = 14;
    style.FrameRounding = 10;
    style.PopupRounding = 14;
    style.GrabRounding = 20;
    style.TabRounding = 10;
    style.ScrollbarSize = 6;
    style.ScrollbarRounding = 6;
    style.GrabMinSize = 18;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 0;
    style.PopupBorderSize = 0;
  }

  if (preset == Preset::Neon) {
    style.WindowPadding = ImVec2(16, 16);
    style.FramePadding = ImVec2(12, 8);
    style.ItemSpacing = ImVec2(10, 10);
    style.WindowRounding = 18;
    style.ChildRounding = 14;
    style.FrameRounding = 10;
    style.PopupRounding = 14;
    style.GrabRounding = 10;
    style.TabRounding = 10;
    style.ScrollbarSize = 8;
    style.ScrollbarRounding = 8;
    style.GrabMinSize = 14;
    style.WindowBorderSize = 0;   // the animated gradient border replaces it
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
  }

  SetColors(style.Colors, def.pal, def.dark);
  if (preset == Preset::Neon) {
    ImVec4* c = style.Colors;
    c[ImGuiCol_FrameBg] = Rgb(0x1A1F3D);
    c[ImGuiCol_FrameBgHovered] = Rgb(0x242A52);
    c[ImGuiCol_FrameBgActive] = Rgb(0x2E2766);
    c[ImGuiCol_Button] = Rgb(0x1F2448);
    c[ImGuiCol_ButtonHovered] = Rgb(0x352C7A);
    c[ImGuiCol_ButtonActive] = Rgb(0x4C36A8);
    c[ImGuiCol_SliderGrab] = Rgb(0xA78BFA);
    c[ImGuiCol_SliderGrabActive] = Rgb(0x22D3EE);
    c[ImGuiCol_CheckMark] = Rgb(0x22D3EE);
    c[ImGuiCol_ScrollbarBg] = Rgb(0x000000, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = Rgb(0x2A3062);
    c[ImGuiCol_PopupBg] = Rgb(0x10132A, 0.97f);
    c[ImGuiCol_Border] = Rgb(0x2A3062, 0.8f);
  }
  if (preset == Preset::iOS) {
    // Fills and controls follow UIKit: grey fills inside cells, white slider knobs.
    ImVec4* c = style.Colors;
    c[ImGuiCol_FrameBg] = Rgb(0x2C2C2E);
    c[ImGuiCol_FrameBgHovered] = Rgb(0x3A3A3C);
    c[ImGuiCol_FrameBgActive] = Rgb(0x48484A);
    c[ImGuiCol_Button] = Rgb(0x2C2C2E);
    c[ImGuiCol_ButtonHovered] = Rgb(0x3A3A3C);
    c[ImGuiCol_ButtonActive] = Rgb(0x48484A);
    c[ImGuiCol_SliderGrab] = Rgb(0xFFFFFF);
    c[ImGuiCol_SliderGrabActive] = Rgb(0xE5E5EA);
    c[ImGuiCol_ScrollbarGrab] = Rgb(0x48484A);
    c[ImGuiCol_ScrollbarBg] = Rgb(0x000000, 0.0f);
    c[ImGuiCol_PopupBg] = Rgb(0x1C1C1E, 0.98f);
    c[ImGuiCol_TitleBg] = Rgb(0x1C1C1E);
    c[ImGuiCol_TitleBgActive] = Rgb(0x1C1C1E);
    c[ImGuiCol_Header] = Rgb(0x0A84FF, 0.22f);
    c[ImGuiCol_HeaderHovered] = Rgb(0x0A84FF, 0.32f);
    c[ImGuiCol_HeaderActive] = Rgb(0x0A84FF, 0.45f);
    c[ImGuiCol_TableHeaderBg] = Rgb(0x2C2C2E);
    c[ImGuiCol_ResizeGrip] = Rgb(0x000000, 0.0f);
    c[ImGuiCol_ResizeGripHovered] = Rgb(0x8E8E93, 0.35f);
    c[ImGuiCol_ResizeGripActive] = Rgb(0x8E8E93, 0.6f);
  }

  style.ScaleAllSizes(scale);
  style.FontScaleMain = scale;
}

const Palette& Colors() { return g_palette; }

Preset Current() { return g_current; }

const char* PresetName(Preset p) {
  switch (p) {
    case Preset::Noir: return "Noir";
    case Preset::Midnight: return "Midnight";
    case Preset::Bordeaux: return "Bordeaux";
    case Preset::Light: return "Light";
    case Preset::Chroma: return "Chroma RGB";
    case Preset::iOS: return "iOS";
    case Preset::Neon: return "Neon Glass";
  }
  return "Neon Glass";
}

ImU32 U32(const ImVec4& c, float alphaMul) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, std::clamp(c.w * alphaMul, 0.0f, 1.0f)));
}

}  // namespace cg::render::theme

namespace cg::render::theme {

void Tick(float dt) {
  if (g_current != Preset::Chroma || ImGui::GetCurrentContext() == nullptr) return;
  if (--g_configRefresh <= 0) {
    g_configRefresh = 30;
    g_speed = std::clamp(Config::Get().ReadFloat("ui.chroma.speed", 0.12f), 0.0f, 2.0f);
    g_saturation = std::clamp(Config::Get().ReadFloat("ui.chroma.saturation", 0.85f), 0.0f, 1.0f);
  }
  g_hue = std::fmod(g_hue + g_speed * std::max(dt, 0.0f), 1.0f);
  g_palette.accent = Hsv(g_hue, g_saturation, 1.0f);
  g_palette.accentHover = Hsv(g_hue, g_saturation * 0.75f, 1.0f);
  g_palette.accentActive = Hsv(g_hue, g_saturation, 0.82f);
  g_palette.accentDim = Hsv(g_hue, g_saturation * 0.9f, 0.36f);
  SetColors(ImGui::GetStyle().Colors, g_palette, true);
}

bool ChromaActive() { return g_current == Preset::Chroma; }

ImU32 Chroma(float offset, float alpha) {
  const float s = g_current == Preset::Chroma ? g_saturation : 0.0f;
  return ImGui::ColorConvertFloat4ToU32(Hsv(g_hue + offset, s > 0 ? s : 0.85f, 1.0f, std::clamp(alpha, 0.0f, 1.0f)));
}

void DrawChromaBorder(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float thickness) {
  if (!dl) return;
  dl->PathRect(min, max, rounding);
  std::vector<ImVec2> pts(dl->_Path.Data, dl->_Path.Data + dl->_Path.Size);
  dl->PathClear();
  if (pts.size() < 2) return;
  pts.push_back(pts.front());
  // Subdivide so straight edges also carry the gradient.
  std::vector<ImVec2> fine;
  fine.reserve(pts.size() * 4);
  const float step = 10.0f;
  for (size_t i = 0; i + 1 < pts.size(); ++i) {
    const ImVec2 a = pts[i], b = pts[i + 1];
    const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    const int n = std::max(1, static_cast<int>(len / step));
    for (int k = 0; k < n; ++k) {
      const float t = static_cast<float>(k) / static_cast<float>(n);
      fine.emplace_back(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
    }
  }
  fine.push_back(pts.back());
  const float total = static_cast<float>(fine.size());
  for (size_t i = 0; i + 1 < fine.size(); ++i) {
    const float off = static_cast<float>(i) / total;
    dl->AddLine(fine[i], fine[i + 1], Chroma(off, 0.16f), thickness * 4.0f);   // glow
  }
  for (size_t i = 0; i + 1 < fine.size(); ++i) {
    const float off = static_cast<float>(i) / total;
    dl->AddLine(fine[i], fine[i + 1], Chroma(off, 1.0f), thickness);
  }
}

void DrawChromaBar(ImDrawList* dl, ImVec2 min, ImVec2 max, float alpha) {
  if (!dl || max.x <= min.x) return;
  constexpr int kSegments = 32;
  const float w = (max.x - min.x) / kSegments;
  for (int i = 0; i < kSegments; ++i) {
    const float a = static_cast<float>(i) / kSegments, b = static_cast<float>(i + 1) / kSegments;
    const ImU32 ca = Chroma(a * 0.5f, alpha), cb = Chroma(b * 0.5f, alpha);
    dl->AddRectFilledMultiColor(ImVec2(min.x + w * i, min.y), ImVec2(min.x + w * (i + 1), max.y), ca, cb, cb, ca);
  }
}

}  // namespace cg::render::theme

namespace cg::render::theme {
namespace {

float Smooth(float x) { return x * x * (3.0f - 2.0f * x); }

// Outline of a rounded rect, subdivided so straight edges carry the gradient too.
std::vector<ImVec2> LoopPoints(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding) {
  dl->PathRect(min, max, rounding);
  std::vector<ImVec2> pts(dl->_Path.Data, dl->_Path.Data + dl->_Path.Size);
  dl->PathClear();
  if (pts.size() < 2) return {};
  pts.push_back(pts.front());
  std::vector<ImVec2> fine;
  fine.reserve(pts.size() * 4);
  for (size_t i = 0; i + 1 < pts.size(); ++i) {
    const ImVec2 a = pts[i], b = pts[i + 1];
    const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    const int n = std::max(1, static_cast<int>(len / 10.0f));
    for (int k = 0; k < n; ++k) {
      const float t = static_cast<float>(k) / static_cast<float>(n);
      fine.emplace_back(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
    }
  }
  fine.push_back(pts.back());
  return fine;
}

}  // namespace

bool NeonActive() { return g_current == Preset::Neon; }

ImU32 Neon(float offset, float alpha) {
  static const ImVec4 kStops[3] = {Rgb(0x8B5CF6), Rgb(0x22D3EE), Rgb(0xF472B6)};
  float x = offset + static_cast<float>(ImGui::GetTime()) * 0.06f;
  x = (x - std::floor(x)) * 3.0f;
  const int i = std::min(static_cast<int>(x), 2);
  ImVec4 c = Mix(kStops[i], kStops[(i + 1) % 3], Smooth(x - static_cast<float>(i)));
  c.w = std::clamp(alpha, 0.0f, 1.0f);
  return ImGui::ColorConvertFloat4ToU32(c);
}

void DrawNeonBorder(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float thickness, float alpha) {
  if (!dl || alpha <= 0.002f) return;
  const auto pts = LoopPoints(dl, min, max, rounding);
  if (pts.size() < 2) return;
  const float total = static_cast<float>(pts.size());
  for (size_t i = 0; i + 1 < pts.size(); ++i)   // soft glow under the line
    dl->AddLine(pts[i], pts[i + 1], Neon(static_cast<float>(i) / total, 0.14f * alpha), thickness * 5.0f);
  for (size_t i = 0; i + 1 < pts.size(); ++i)
    dl->AddLine(pts[i], pts[i + 1], Neon(static_cast<float>(i) / total, alpha), thickness);
}

void DrawNeonBar(ImDrawList* dl, ImVec2 min, ImVec2 max, float alpha) {
  if (!dl || max.x <= min.x) return;
  constexpr int kSegments = 32;
  const float w = (max.x - min.x) / kSegments;
  for (int i = 0; i < kSegments; ++i) {
    const float a = static_cast<float>(i) / kSegments, b = static_cast<float>(i + 1) / kSegments;
    const ImU32 ca = Neon(a * 0.6f, alpha), cb = Neon(b * 0.6f, alpha);
    dl->AddRectFilledMultiColor(ImVec2(min.x + w * i, min.y), ImVec2(min.x + w * (i + 1), max.y), ca, cb, cb, ca);
  }
}

void DrawAurora(ImDrawList* dl, ImVec2 min, ImVec2 max, float alpha) {
  if (!dl || max.x <= min.x || max.y <= min.y) return;
  const float t = static_cast<float>(ImGui::GetTime());
  const float w = max.x - min.x, h = max.y - min.y;
  struct Blob { float fx, fy, sx, sy, phase, radius, color; };
  static const Blob kBlobs[] = {
      {0.25f, 0.30f, 0.11f, 0.07f, 0.0f, 0.42f, 0.00f},
      {0.78f, 0.25f, 0.08f, 0.10f, 2.1f, 0.36f, 0.34f},
      {0.60f, 0.85f, 0.13f, 0.06f, 4.2f, 0.40f, 0.67f},
  };
  dl->PushClipRect(min, max, true);
  for (const Blob& b : kBlobs) {
    const ImVec2 c(min.x + w * (b.fx + 0.12f * std::sin(t * b.sx + b.phase)), min.y + h * (b.fy + 0.10f * std::cos(t * b.sy + b.phase)));
    const float r = std::min(w, h) * b.radius;
    // Smooth radial gradient: a triangle fan, coloured centre fading to transparent at the rim.
    constexpr int kSegs = 64;
    const ImU32 inner = Neon(b.color, 0.16f * alpha), outer = Neon(b.color, 0.0f);
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;
    dl->PrimReserve(kSegs * 3, kSegs + 1);
    const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    dl->PrimWriteVtx(c, uv, inner);
    for (int k = 0; k < kSegs; ++k) {
      const float ang = static_cast<float>(k) / kSegs * 6.2831853f;
      dl->PrimWriteVtx(ImVec2(c.x + std::cos(ang) * r, c.y + std::sin(ang) * r), uv, outer);
    }
    for (int k = 0; k < kSegs; ++k) {
      dl->PrimWriteIdx(base);
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + k));
      dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + (k + 1) % kSegs));
    }
  }
  dl->PopClipRect();
}

}  // namespace cg::render::theme
