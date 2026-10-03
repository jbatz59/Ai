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

enum class Preset { Noir, Midnight, Bordeaux, Light };

void Apply(Preset preset, float uiScale);
const Palette& Colors();
Preset Current();
const char* PresetName(Preset p);

ImU32 U32(const ImVec4& c, float alphaMul = 1.0f);

}  // namespace cg::render::theme
