#pragma once
// Visual identity: 1930s noir — charcoal panels, warm brass/gold accent, oxblood highlights,
// soft rounding. All colors live here so widgets and pages never hard-code them.
#include <imgui.h>

namespace cg::render::theme {

struct Palette {
  ImVec4 bg, bgAlt, panel, panelHover, border;
  ImVec4 text, textDim, textFaint;
  ImVec4 accent, accentHover, accentActive, accentDim;   // brass
  ImVec4 danger, warning, success, info;                 // oxblood, amber, green, steel blue
};

enum class Preset { Noir, Midnight, Bordeaux, Light, Chroma, iOS, Neon };
inline constexpr int kPresetCount = 7;
inline constexpr Preset kDefaultPreset = Preset::Neon;

void Apply(Preset preset, float uiScale);
const Palette& Colors();
Preset Current();
const char* PresetName(Preset p);

ImU32 U32(const ImVec4& c, float alphaMul = 1.0f);

// ---- Chroma (animated RGB accents; active when the Chroma preset is selected) ------------------
// Tick advances the hue (render thread, once per frame before ImGui::NewFrame) and re-tints every
// accent-derived ImGui colour. Speed/saturation come from Config "ui.chroma.speed" (hue cycles per
// second, default 0.12) and "ui.chroma.saturation" (default 0.85).
void Tick(float dt);
bool ChromaActive();
ImU32 Chroma(float offset, float alpha = 1.0f);   // colour at `offset` (0..1) around the wheel from the current hue
// Animated rainbow outline with a soft glow (subdivided so long edges carry a gradient too).
void DrawChromaBorder(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float thickness);
// Horizontal rainbow gradient bar.
void DrawChromaBar(ImDrawList* dl, ImVec2 min, ImVec2 max, float alpha = 1.0f);

// ---- Neon Glass (default): animated violet -> cyan -> pink gradient accents ---------------------
bool NeonActive();
ImU32 Neon(float offset, float alpha = 1.0f);   // gradient colour at `offset` (0..1), drifting over time
// Animated gradient outline; `alpha` scales it (cards fade it in on hover).
void DrawNeonBorder(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, float thickness, float alpha = 1.0f);
void DrawNeonBar(ImDrawList* dl, ImVec2 min, ImVec2 max, float alpha = 1.0f);
// Soft drifting light blobs behind the menu content (clipped to the rect).
void DrawAurora(ImDrawList* dl, ImVec2 min, ImVec2 max, float alpha = 1.0f);

}  // namespace cg::render::theme
