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

enum class Preset { Noir, Midnight, Bordeaux, Light, Chroma };
inline constexpr int kPresetCount = 5;

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

}  // namespace cg::render::theme
