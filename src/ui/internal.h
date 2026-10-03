#pragma once
// Private glue between the ui shell translation units (registry, menu, HUD, palette, widgets).
// Not for tool windows or features: those use ui/ui.h, ui/widgets.h and ui/notify.h.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include <imgui.h>

#include "core/hotkeys.h"
#include "features/feature.h"

namespace cg::ui::detail {

// ---- Pages of the main menu -----------------------------------------------------------------
enum class Page : uint8_t { Home, Player, Vehicle, Weapons, World, Teleport, Camera, Visuals, Fun, CheatTable, Scripts, Tools, Settings, About, Count };

const char* PageName(Page p);    // "Cheat Table"
const char* PageKey(Page p);     // stable config value: "cheat_table"
const char* PageIcon(Page p);    // CG_ICON_*
std::optional<Page> PageFromKey(std::string_view key);
std::optional<features::Category> PageCategory(Page p);   // category pages only
Page PageForCategory(features::Category c);                // Scripts category -> Scripts page

// ---- Main menu (menu.cpp) -------------------------------------------------------------------
void DrawMainMenu();
// Switches page; when `featureId` is set the page scrolls to that feature card and highlights it.
void NavigateTo(Page p, std::string_view featureId = {});
Page CurrentPage();
void RequestUnloadConfirmation();   // opens the confirmation popup next time the menu draws

// ---- HUD (hud.cpp) --------------------------------------------------------------------------
void DrawHud();   // call every frame; draws only while the menu is closed

// ---- Command palette (palette.cpp) ----------------------------------------------------------
void UpdatePalette();   // call every frame while the menu is open: Ctrl+P handling + drawing
void OpenPalette();
void ClosePalette();
bool PaletteOpen();

// ---- Registry helpers (ui.cpp) ---------------------------------------------------------------
// Runs `fn` so that a C++ exception thrown from module code (tool window, feature settings, script
// menu) can't unbalance ImGui's stacks or escape into the Present hook. Failures are logged and
// toasted once per `what`.
void RunGuardedImpl(const char* what, void (*thunk)(void*), void* ctx);
template <class F> void RunGuarded(const char* what, F&& fn) {
  using Fn = std::remove_reference_t<F>;
  RunGuardedImpl(what, [](void* p) { (*static_cast<Fn*>(p))(); }, static_cast<void*>(&fn));
}

// Shell settings, cached from Config (refreshed twice a second; setters write through).
struct ShellSettings {
  bool hudWatermark = true;
  bool hudFps = false;
  bool hudActive = true;
  bool hudPosition = false;
  bool notifications = true;
  Hotkey menuKey, panicKey, unloadKey;
};
const ShellSettings& Settings();
void InvalidateSettings();

// Config keys + defaults shared by the overlay (it reads the hotkeys every frame via Hotkey::Parse).
inline constexpr const char* kCfgTheme = "ui.theme";             // int: render::theme::Preset
inline constexpr const char* kCfgScale = "ui.scale";             // double
inline constexpr const char* kCfgPage = "ui.page";               // string: PageKey()
inline constexpr const char* kCfgNotifications = "ui.notifications";
inline constexpr const char* kCfgHudWatermark = "hud.watermark";
inline constexpr const char* kCfgHudFps = "hud.fps";
inline constexpr const char* kCfgHudActive = "hud.active";
inline constexpr const char* kCfgHudPosition = "hud.position";
inline constexpr const char* kCfgMenuKey = "hotkeys.menu";
inline constexpr const char* kCfgPanicKey = "hotkeys.panic";
inline constexpr const char* kCfgUnloadKey = "hotkeys.unload";
inline constexpr const char* kCfgLogLevel = "log.level";         // string: log::LevelName()
Hotkey DefaultMenuKey();     // Insert
Hotkey DefaultPanicKey();    // Ctrl+Shift+P
Hotkey DefaultUnloadKey();   // Ctrl+Shift+End

// Theme/scale changes are applied between frames (never while style vars are pushed).
void RequestAppearance(int preset, float scale);
float UiScale();   // current style.FontScaleMain

// Feature interactions from the UI are applied between frames, looked up by id, so a feature (or
// the script behind it) can never invalidate the list that is being drawn.
void QueueFeatureSet(const std::string& id, bool on);
void QueueFeatureTrigger(const std::string& id);

// Shared commands (menu, palette, HUD).
void CmdDisableAll();
void CmdReloadBindings();
void CmdReloadScripts();   // deferred to the start of the next frame
void CmdSaveConfig();
void CmdOpenFolder(const std::filesystem::path& dir);

// Time since ui::Init in seconds (HUD watermark, animations).
double SecondsSinceInit();

// ---- Drawing helpers (widgets.cpp) -----------------------------------------------------------
void WidgetsNewFrame();   // cancels a hotkey capture whose button stopped being drawn
bool HasIcons();
ImU32 Col(const ImVec4& c, float alphaMul = 1.0f);   // palette colour honouring style.Alpha (BeginDisabled)
ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t);
ImVec4 WithAlpha(const ImVec4& c, float a);
ImVec4 DangerText();   // danger hue lifted toward the text colour for readable error text
float Approach(float current, float target, float speed);   // frame-rate independent exponential smoothing
// "<icon>  text" when the icon font is loaded, otherwise just `text`.
std::string IconLabel(const char* icon, std::string_view text);

// Font scopes. The font pointers may be null (fallback) — the current font is kept then.
void PushBold(float sizeMul = 1.0f);
void PushTitle(float sizeMul = 1.55f);
void PushMono(float sizeMul = 1.0f);
void PushBodySize(float sizeMul);   // current font, scaled size
inline void PopFont() { ImGui::PopFont(); }

void TextColored(const ImVec4& c, std::string_view text);
void TextWrappedColored(const ImVec4& c, std::string_view text);
// Toggle with a clickable label to its right. Returns true when toggled.
bool LabeledToggle(const char* label, bool* v, const char* help = nullptr, bool enabled = true);
// Button styled with the accent (primary) or danger colour.
bool AccentButton(const char* label, ImVec2 size = {0, 0});
bool DangerButton(const char* label, ImVec2 size = {0, 0});
// Small rounded status pill (dot + text) on the current line.
void StatusPill(const char* text, const ImVec4& color);

}  // namespace cg::ui::detail
