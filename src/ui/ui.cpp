// Math operators for ImVec2/ImVec4 must be enabled before the first include of imgui.h.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/ui.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <imgui.h>
#include <imgui_internal.h>

#include "core/config.h"
#include "core/hotkeys.h"
#include "core/log.h"
#include "core/tasks.h"
#include "core/util.h"
#include "features/feature.h"
#include "game/bindings.h"
#include "render/fonts.h"
#include "render/overlay.h"
#include "render/theme.h"
#include "script/engine.h"
#include "ui/internal.h"
#include "ui/notify.h"
#include "ui/widgets.h"

// This file defines ui::FindWindow itself (see the note in ui.h).
#undef FindWindow

namespace cg::ui {

const char* Window::Icon() const { return CG_ICON_TOOLS; }
const char* Window::Group() const { return "Tools"; }

namespace {

using Clock = std::chrono::steady_clock;

struct Persisted {
  bool open = false;
  bool pinned = false;
};

std::vector<std::unique_ptr<Window>> g_windows;
std::unordered_map<std::string, Persisted> g_persisted;
std::string g_focusRequest;
bool g_initialized = false;
Clock::time_point g_initTime = Clock::now();

detail::ShellSettings g_settings;
bool g_settingsValid = false;
Clock::time_point g_settingsAt;

struct FailureRecord {
  Clock::time_point lastLogged;
  uint64_t count = 0;
};
std::unordered_map<std::string, FailureRecord> g_failures;

std::string WindowKey(const char* id, const char* field) {
  std::string k = "ui.windows.";
  k += id;
  k += '.';
  k += field;
  return k;
}

void LoadWindowState(Window& w) {
  const auto& cfg = Config::Get();
  w.open = cfg.ReadBool(WindowKey(w.Id(), "open"), w.open);
  w.pinned = cfg.ReadBool(WindowKey(w.Id(), "pinned"), w.pinned);
  g_persisted[w.Id()] = {w.open, w.pinned};
}

void PersistWindowState() {
  auto& cfg = Config::Get();
  for (const auto& w : g_windows) {
    Persisted& p = g_persisted[w->Id()];
    if (p.open != w->open) {
      cfg.WriteBool(WindowKey(w->Id(), "open"), w->open);
      p.open = w->open;
    }
    if (p.pinned != w->pinned) {
      cfg.WriteBool(WindowKey(w->Id(), "pinned"), w->pinned);
      p.pinned = w->pinned;
    }
  }
}

void ReportFailure(const char* what, const char* message) {
  const std::string key = what != nullptr ? what : "ui";
  FailureRecord& rec = g_failures[key];
  const auto now = Clock::now();
  const bool first = rec.count++ == 0;
  if (first || now - rec.lastLogged > std::chrono::seconds(5)) {
    rec.lastLogged = now;
    log::Error("ui", "Exception in {}: {} (occurrence {})", key, message, rec.count);
  }
  if (first) notify::Push(notify::Kind::Error, "UI error in " + key, message, 6.0f);
}

void EnsureHotkeyDefaults() {
  auto& cfg = Config::Get();
  const auto ensure = [&](const char* key, const Hotkey& def) {
    if (cfg.ReadJson(key).is_null()) cfg.WriteString(key, def.ToString());
  };
  ensure(detail::kCfgMenuKey, detail::DefaultMenuKey());
  ensure(detail::kCfgPanicKey, detail::DefaultPanicKey());
  ensure(detail::kCfgUnloadKey, detail::DefaultUnloadKey());
}

void ReloadFromConfig() {
  for (const auto& w : g_windows) LoadWindowState(*w);
  detail::InvalidateSettings();
  const auto& cfg = Config::Get();
  const int preset = static_cast<int>(cfg.ReadInt(detail::kCfgTheme, static_cast<int>(render::theme::kDefaultPreset)));
  const float scale = cfg.ReadFloat(detail::kCfgScale, detail::UiScale());
  render::theme::Apply(static_cast<render::theme::Preset>(preset), scale);
  log::SetMinLevel(static_cast<log::Level>(
      std::clamp<int64_t>(cfg.ReadInt(detail::kCfgLogLevel, static_cast<int64_t>(log::MinLevel())), 0, 4)));
  if (const auto page = detail::PageFromKey(cfg.ReadString(detail::kCfgPage, "home"))) detail::NavigateTo(*page);
}

// Profile loads may come from any thread; the UI state is re-read between frames.
void OnConfigReloaded() {
  tasks::PostRender([] {
    if (g_initialized) ReloadFromConfig();
  });
}

void TitleBarControls(Window& w) {
  ImGuiWindow* win = ImGui::GetCurrentWindow();
  if (win == nullptr || (win->Flags & ImGuiWindowFlags_NoTitleBar)) return;

  // Right after Begin() the "last item" is the title bar: right-click it for window options.
  if (ImGui::BeginPopupContextItem("##cg.title.ctx")) {
    ImGui::MenuItem("Pin (stays visible while the menu is closed)", nullptr, &w.pinned);
    if (ImGui::MenuItem("Close")) w.open = false;
    ImGui::EndPopup();
  }

  ImGuiContext& g = *GImGui;
  const ImGuiStyle& style = g.Style;
  const ImRect tb = win->TitleBarRect();
  const float sz = g.FontSize;
  // ImGui places the close button at Max.x - FramePadding.x - sz; the pin sits just left of it.
  const ImVec2 pos(tb.Max.x - style.FramePadding.x - sz * 2.0f - style.ItemInnerSpacing.x, tb.Min.y + style.FramePadding.y);
  const ImRect bb(pos, pos + ImVec2(sz, sz));
  const ImGuiID id = win->GetID("#cg.pin");
  ImGui::PushClipRect(tb.Min, tb.Max, false);
  bool hovered = false, held = false;
  if (ImGui::ItemAdd(bb, id, nullptr, ImGuiItemFlags_NoNav)) {
    if (ImGui::ButtonBehavior(bb, id, &hovered, &held)) w.pinned = !w.pinned;
    const auto& p = render::theme::Colors();
    ImDrawList* dl = win->DrawList;
    if (hovered) dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered), style.FrameRounding);
    const ImU32 col = detail::Col(w.pinned ? p.accent : (hovered ? p.text : p.textFaint));
    if (detail::HasIcons()) {
      ImFont* font = render::GetFonts().body;
      const char* glyph = w.pinned ? CG_ICON_PINNED : CG_ICON_PIN;
      const float fs = sz * 0.85f;
      const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, glyph);
      dl->AddText(font, fs, bb.GetCenter() - ts * 0.5f, col, glyph);
    } else {
      // Drawn push-pin: head + needle.
      const ImVec2 c = bb.GetCenter();
      dl->AddCircleFilled(ImVec2(c.x, c.y - sz * 0.14f), sz * 0.2f, col);
      dl->AddLine(ImVec2(c.x, c.y), ImVec2(c.x, c.y + sz * 0.34f), col, std::max(1.0f, sz * 0.08f));
      if (!w.pinned) dl->AddCircle(ImVec2(c.x, c.y - sz * 0.14f), sz * 0.2f, col);
    }
  }
  ImGui::PopClipRect();
  if (hovered) ImGui::SetTooltip("%s", w.pinned ? "Pinned: stays visible (click-through) while the menu is closed" : "Pin window");
}

void DrawToolWindow(Window& w, size_t index, bool passive) {
  const float s = detail::UiScale();
  const char* id = w.Id();
  std::string title = detail::IconLabel(w.Icon(), w.Title() != nullptr ? w.Title() : id);
  title += "###";
  title += id;

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float cascade = static_cast<float>(index % 8) * 26.0f * s;
  ImGui::SetNextWindowPos(vp->GetCenter() + ImVec2(cascade, cascade), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(w.DefaultWidth() * s, w.DefaultHeight() * s), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(280.0f * s, 160.0f * s), ImVec2(FLT_MAX, FLT_MAX));
  ImGuiWindowFlags flags = 0;
  if (passive) {
    flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
             ImGuiWindowFlags_NoNav;
    ImGui::SetNextWindowBgAlpha(0.82f);
  } else if (g_focusRequest == id) {
    ImGui::SetNextWindowFocus();
    ImGui::SetNextWindowCollapsed(false);
    g_focusRequest.clear();
  }

  bool open = true;
  const bool visible = ImGui::Begin(title.c_str(), passive ? nullptr : &open, flags);
  if (!passive) TitleBarControls(w);
  if (visible) detail::RunGuarded(id, [&w] { w.Draw(); });
  ImGui::End();
  if (!passive && !open) w.open = false;
}

void DrawToolWindows(bool passive) {
  // Index loop: a window's Draw() may register further windows.
  for (size_t i = 0; i < g_windows.size(); ++i) {
    Window* w = g_windows[i].get();
    if (w == nullptr || !w->open || (passive && !w->pinned)) continue;
    DrawToolWindow(*w, i, passive);
  }
}

template <class T> T* NavigationTarget(const char* id, const char* what) {
  Window* w = FindWindow(id);
  if (w == nullptr) {
    notify::Push(notify::Kind::Warning, std::string(what) + " unavailable", std::string("Tool window '") + id + "' is not registered");
    return nullptr;
  }
  OpenWindow(id);
  T* t = dynamic_cast<T*>(w);
  if (t == nullptr) log::Warn("ui", "Tool window '{}' does not accept {} requests", id, what);
  return t;
}

}  // namespace

// ---- registry ------------------------------------------------------------------------------------

void RegisterWindow(std::unique_ptr<Window> w) {
  if (!w || w->Id() == nullptr || *w->Id() == '\0') return;
  if (FindWindow(w->Id()) != nullptr) {
    log::Warn("ui", "Duplicate tool window id '{}' ignored", w->Id());
    return;
  }
  LoadWindowState(*w);
  g_windows.push_back(std::move(w));
}

Window* FindWindow(const std::string& id) {
  for (const auto& w : g_windows)
    if (id == w->Id()) return w.get();
  return nullptr;
}

const std::vector<std::unique_ptr<Window>>& Windows() { return g_windows; }

void OpenWindow(const std::string& id) {
  Window* w = FindWindow(id);
  if (w == nullptr) {
    log::Warn("ui", "OpenWindow: unknown tool window '{}'", id);
    notify::Push(notify::Kind::Warning, "Tool unavailable", "No tool window named '" + id + "'");
    return;
  }
  w->open = true;
  g_focusRequest = id;
}

void GotoHexView(uintptr_t address) {
  if (auto* t = NavigationTarget<HexViewTarget>("tools.hexview", "Hex view"))
    detail::RunGuarded("hex view navigation", [&] { t->Goto(address); });
}

void AddToAddressTable(uintptr_t address, const std::string& typeName, const std::string& description) {
  if (auto* t = NavigationTarget<AddressTableTarget>("tools.addresstable", "Address table"))
    detail::RunGuarded("address table", [&] { t->AddEntry(address, typeName, description); });
}

void SetBreakpointOn(uintptr_t address, int kind, int size) {
  if (auto* t = NavigationTarget<BreakpointTarget>("tools.breakpoints", "Breakpoints"))
    detail::RunGuarded("breakpoints", [&] { t->SetOn(address, kind, size); });
}

void InspectStruct(uintptr_t address) {
  if (auto* t = NavigationTarget<StructTarget>("tools.struct", "Struct dissector"))
    detail::RunGuarded("struct dissector", [&] { t->Inspect(address); });
}

// ---- lifecycle -----------------------------------------------------------------------------------

void Init() {
  if (g_initialized) return;
  g_initialized = true;
  g_initTime = Clock::now();
  EnsureHotkeyDefaults();
  detail::InvalidateSettings();
  Config::Get().AddReloadListener(&OnConfigReloaded);

  using Factory = std::unique_ptr<Window> (*)();
  static constexpr std::pair<const char*, Factory> kFactories[] = {
      {"tools.scanner", &MakeScannerWindow},           {"tools.hexview", &MakeHexViewWindow},
      {"tools.addresstable", &MakeAddressTableWindow},   {"tools.bindings", &MakeBindingsWindow},
      {"tools.signature", &MakeSignatureWindow},       {"tools.log", &MakeLogConsoleWindow},
      {"tools.gamelua", &MakeGameLuaConsoleWindow},    {"tools.modules", &MakeModulesWindow},
  };
  for (const auto& [name, make] : kFactories) {
    try {
      std::unique_ptr<Window> w = make();
      if (!w) {
        log::Warn("ui", "Tool window factory for '{}' returned nothing", name);
        continue;
      }
      if (std::string_view(w->Id()) != name) log::Warn("ui", "Tool window '{}' registered under id '{}'", name, w->Id());
      RegisterWindow(std::move(w));
    } catch (const std::exception& e) {
      log::Error("ui", "Creating tool window '{}' failed: {}", name, e.what());
    } catch (...) {
      log::Error("ui", "Creating tool window '{}' failed", name);
    }
  }
  if (const auto page = detail::PageFromKey(Config::Get().ReadString(detail::kCfgPage, "home"))) detail::NavigateTo(*page);
  log::Info("ui", "UI ready: {} tool windows", g_windows.size());
}

void DrawFrame() {
  if (ImGui::GetCurrentContext() == nullptr) return;
  if (!g_initialized) Init();
  detail::WidgetsNewFrame();

  detail::RunGuarded("feature overlays", [] { features::Registry::Get().DrawOverlays(); });
  detail::RunGuarded("script overlay", [] { script::Engine::Get().DrawOverlay(); });
  detail::RunGuarded("HUD", [] { detail::DrawHud(); });

  if (render::MenuOpen()) {
    detail::RunGuarded("main menu", [] { detail::DrawMainMenu(); });
    DrawToolWindows(false);
    detail::RunGuarded("command palette", [] { detail::UpdatePalette(); });
  } else {
    if (detail::PaletteOpen()) detail::ClosePalette();
    DrawToolWindows(true);
  }
  PersistWindowState();
  detail::RunGuarded("notifications", [] { notify::Draw(); });
}

void Shutdown() {
  if (!g_initialized) return;
  if (hotkeys::Capturing()) hotkeys::CancelCapture();
  PersistWindowState();
  detail::ClosePalette();
  g_windows.clear();
  g_persisted.clear();
  g_focusRequest.clear();
  g_failures.clear();
  g_initialized = false;
}

// ---- shell internals -----------------------------------------------------------------------------
namespace detail {

void RunGuardedImpl(const char* what, void (*thunk)(void*), void* ctx) {
  ImGuiContext* g = ImGui::GetCurrentContext();
  const bool canRecover = g != nullptr && g->CurrentWindow != nullptr && g->WithinFrameScope;
  ImGuiErrorRecoveryState state;
  if (canRecover) ImGui::ErrorRecoveryStoreState(&state);
  try {
    thunk(ctx);
    return;
  } catch (const std::exception& e) {
    ReportFailure(what, e.what());
  } catch (...) {
    ReportFailure(what, "unknown exception");
  }
  if (canRecover) {
    // Unwind whatever Begin/Push the failing code left open, without tripping ImGui's asserts.
    ImGuiIO& io = ImGui::GetIO();
    const bool assertBackup = io.ConfigErrorRecoveryEnableAssert;
    const bool tooltipBackup = io.ConfigErrorRecoveryEnableTooltip;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    ImGui::ErrorRecoveryTryToRecoverState(&state);
    io.ConfigErrorRecoveryEnableAssert = assertBackup;
    io.ConfigErrorRecoveryEnableTooltip = tooltipBackup;
  }
}

Hotkey DefaultMenuKey() { return Hotkey{VK_INSERT, false, false, false}; }
Hotkey DefaultPanicKey() { return Hotkey{'P', true, true, false}; }
Hotkey DefaultUnloadKey() { return Hotkey{VK_END, true, true, false}; }

const ShellSettings& Settings() {
  const auto now = Clock::now();
  if (!g_settingsValid || now - g_settingsAt > std::chrono::milliseconds(500)) {
    const auto& cfg = Config::Get();
    ShellSettings s;
    s.hudWatermark = cfg.ReadBool(kCfgHudWatermark, true);
    s.hudFps = cfg.ReadBool(kCfgHudFps, false);
    s.hudActive = cfg.ReadBool(kCfgHudActive, true);
    s.hudPosition = cfg.ReadBool(kCfgHudPosition, false);
    s.notifications = cfg.ReadBool(kCfgNotifications, true);
    s.menuKey = Hotkey::Parse(cfg.ReadString(kCfgMenuKey, DefaultMenuKey().ToString()));
    s.panicKey = Hotkey::Parse(cfg.ReadString(kCfgPanicKey, DefaultPanicKey().ToString()));
    s.unloadKey = Hotkey::Parse(cfg.ReadString(kCfgUnloadKey, DefaultUnloadKey().ToString()));
    g_settings = s;
    g_settingsValid = true;
    g_settingsAt = now;
  }
  return g_settings;
}

void InvalidateSettings() { g_settingsValid = false; }

void RequestAppearance(int preset, float scale) {
  scale = std::clamp(scale, 0.75f, 2.0f);
  auto& cfg = Config::Get();
  cfg.WriteInt(kCfgTheme, preset);
  cfg.WriteDouble(kCfgScale, scale);
  tasks::PostRender([preset, scale] { render::theme::Apply(static_cast<render::theme::Preset>(preset), scale); });
}

float UiScale() {
  if (ImGui::GetCurrentContext() == nullptr) return 1.0f;
  const float s = ImGui::GetStyle().FontScaleMain;
  return s > 0.0f ? s : 1.0f;
}

double SecondsSinceInit() { return std::chrono::duration<double>(Clock::now() - g_initTime).count(); }

void CmdDisableAll() {
  features::Registry::Get().DisableAll();
  notify::Push(notify::Kind::Warning, "All features disabled");
}

void CmdReloadBindings() {
  auto& b = game::Bindings::Get();
  if (b.Resolving()) {
    notify::Push(notify::Kind::Info, "Bindings are already resolving", "Wait for the current pass to finish");
    return;
  }
  notify::Push(notify::Kind::Info, "Reloading bindings...");
  tasks::RunAsync([] {
    bool ok = false;
    try {
      ok = game::Bindings::Get().Reload();
    } catch (const std::exception& e) {
      log::Error("ui", "Bindings reload threw: {}", e.what());
    } catch (...) {
      log::Error("ui", "Bindings reload threw");
    }
    if (!ok) {
      notify::Push(notify::Kind::Error, "Bindings reload failed", "No binding file could be parsed - see the log");
      return;
    }
    size_t resolved = 0, total = 0;
    for (const auto& s : game::Bindings::Get().List()) {
      ++total;
      if (s.status == game::SymbolStatus::Resolved || s.status == game::SymbolStatus::Overridden) ++resolved;
    }
    notify::Push(resolved == total ? notify::Kind::Success : notify::Kind::Warning, "Bindings reloaded",
                 std::to_string(resolved) + " of " + std::to_string(total) + " symbols resolved");
    // Same as the initial pass: rebuild data-driven cheats on the render thread, then restore state.
    tasks::PostRender([] {
      auto& registry = features::Registry::Get();
      features::SyncCheatTableFeatures(registry);
      registry.Load();
    });
  });
}

void CmdReloadScripts() {
  tasks::PostRender([] {
    RunGuarded("script reload", [] { script::Engine::Get().ReloadAll(); });
    size_t loaded = 0, failed = 0;
    for (const auto& s : script::Engine::Get().List()) {
      if (!s.error.empty()) ++failed;
      else if (s.loaded) ++loaded;
    }
    notify::Push(failed ? notify::Kind::Warning : notify::Kind::Success, "Scripts reloaded",
                 std::to_string(loaded) + " loaded" + (failed ? ", " + std::to_string(failed) + " with errors" : std::string()));
  });
}

void QueueFeatureSet(const std::string& id, bool on) {
  tasks::PostRender([id, on] {
    if (features::Feature* f = features::Registry::Get().Find(id)) RunGuarded(id.c_str(), [f, on] { f->SetEnabled(on); });
  });
}

void QueueFeatureTrigger(const std::string& id) {
  tasks::PostRender([id] {
    if (features::Feature* f = features::Registry::Get().Find(id)) RunGuarded(id.c_str(), [f] { f->Trigger(); });
  });
}

void CmdSaveConfig() {
  features::Registry::Get().Save();
  if (Config::Get().Save()) notify::Push(notify::Kind::Success, "Configuration saved");
  else notify::Push(notify::Kind::Error, "Saving configuration failed", "See the log for details");
}

void CmdOpenFolder(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  // ShellExecute may block on shell extensions; never on the render thread.
  tasks::RunAsync([dir] {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const auto rc = reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"explore", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (SUCCEEDED(hr)) CoUninitialize();
    if (rc <= 32) {
      log::Warn("ui", "Opening folder {} failed (code {})", util::Narrow(dir.wstring()), rc);
      notify::Push(notify::Kind::Error, "Could not open folder", util::Narrow(dir.wstring()));
    }
  });
}

}  // namespace detail
}  // namespace cg::ui
