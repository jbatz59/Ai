#pragma once
// House widgets. Pages and tools use these instead of raw ImGui where a consistent look matters.
#include <cstdint>
#include <string>

#include <imgui.h>

#include "core/hotkeys.h"

namespace cg::ui {

// Animated pill switch. Returns true when toggled.
bool ToggleSwitch(const char* id, bool* v, bool enabled = true);
// Shows "Insert" / "Unbound"; click -> "Press a key..." capture mode. Returns true when changed.
bool HotkeyButton(const char* id, Hotkey* hk);
void HelpMarker(const char* text);             // (?) with tooltip
void SectionHeader(const char* text);          // gold small-caps style header with rule
void StatusDot(ImU32 color, const char* tooltip = nullptr);
bool SearchBox(const char* id, char* buf, size_t bufSize, const char* hint = "Search...");
// Address entry that accepts mem::EvalAddress expressions; shows the resolved value or the error.
bool AddressInput(const char* id, std::string* expr, uintptr_t* out);
void CopyableText(const char* text);           // click to copy to clipboard (with toast)
bool IconButton(const char* icon, const char* fallbackText, const char* tooltip, ImVec2 size = {0, 0});
void Badge(const char* text, ImU32 bg, ImU32 fg);
// Text in the icon font when available, otherwise the fallback.
void Icon(const char* icon, const char* fallback = "");
// Rounded card container: BeginCard("id"); ...; EndCard();
bool BeginCard(const char* id, ImVec2 size = {0, 0});
void EndCard();

}  // namespace cg::ui
