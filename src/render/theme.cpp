#include "render/theme.h"

#include <algorithm>
#include <cstdint>

#include <imgui.h>

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

PresetDef Make(Preset p) {
  switch (p) {
    case Preset::Midnight: return MakeMidnight();
    case Preset::Bordeaux: return MakeBordeaux();
    case Preset::Light: return MakeLight();
    case Preset::Noir:
    default: return MakeNoir();
  }
}

Preset Sanitize(Preset p) {
  switch (p) {
    case Preset::Noir:
    case Preset::Midnight:
    case Preset::Bordeaux:
    case Preset::Light: return p;
  }
  return Preset::Noir;
}

Palette g_palette = MakeNoir().pal;
Preset g_current = Preset::Noir;

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

  SetColors(style.Colors, def.pal, def.dark);

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
  }
  return "Noir";
}

ImU32 U32(const ImVec4& c, float alphaMul) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, std::clamp(c.w * alphaMul, 0.0f, 1.0f)));
}

}  // namespace cg::render::theme
