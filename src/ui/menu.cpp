// Math operators for ImVec2/ImVec4 must be enabled before the first include of imgui.h.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/internal.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <windows.h>

#include <imgui.h>
#include <imgui_internal.h>

#include "cg_version.h"
#include "core/bootstrap.h"
#include "core/config.h"
#include "core/hotkeys.h"
#include "core/log.h"
#include "core/mp_guard.h"
#include "core/paths.h"
#include "core/tasks.h"
#include "core/util.h"
#include "features/feature.h"
#include "game/bindings.h"
#include "game/game.h"
#include "render/fonts.h"
#include "render/overlay.h"
#include "render/theme.h"
#include "script/engine.h"
#include "ui/notify.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace cg::ui::detail {
namespace {

namespace th = render::theme;
using features::Category;
using features::Feature;
using features::Kind;

constexpr int kNoCategory = -1;

struct PageInfo {
  Page page;
  const char* name;
  const char* key;
  const char* icon;
  int category;
  const char* blurb;
};

constexpr PageInfo kPages[] = {
    {Page::Home, "Home", "home", CG_ICON_HOME, kNoCategory, "Favorites, what's running and the state of the game."},
    {Page::Player, "Player", "player", CG_ICON_PLAYER, static_cast<int>(Category::Player), "Tommy himself: health, movement, abilities."},
    {Page::Vehicle, "Vehicle", "vehicle", CG_ICON_CAR, static_cast<int>(Category::Vehicle), "The car you are driving."},
    {Page::Weapons, "Weapons", "weapons", CG_ICON_WEAPON, static_cast<int>(Category::Weapons), "Ammunition, reloads and firearms."},
    {Page::World, "World", "world", CG_ICON_WORLD, static_cast<int>(Category::World), "Time, weather, traffic and the city of Lost Heaven."},
    {Page::Teleport, "Teleport", "teleport", CG_ICON_TELEPORT, static_cast<int>(Category::Teleport), "Saved places and quick travel."},
    {Page::Camera, "Camera", "camera", CG_ICON_CAMERA, static_cast<int>(Category::Camera), "Field of view, free camera and photo tools."},
    {Page::Visuals, "Visuals", "visuals", CG_ICON_VISUALS, static_cast<int>(Category::Visuals), "Overlays drawn on top of the world."},
    {Page::CheatTable, "Cheat Table", "cheat_table", CG_ICON_TABLE, static_cast<int>(Category::CheatTable),
     "Data-driven cheats declared in the bindings files."},
    {Page::Scripts, "Scripts", "scripts", CG_ICON_SCRIPT, kNoCategory, "Your Lua scripts, their menu entries and panels."},
    {Page::Tools, "Tools", "tools", CG_ICON_DEVTOOLS, kNoCategory, "Reverse-engineering toolkit: scan, inspect, hook and bind."},
    {Page::Settings, "Settings", "settings", CG_ICON_SETTINGS, kNoCategory, "Appearance, hotkeys, HUD, logging and profiles."},
    {Page::About, "About", "about", CG_ICON_INFO, kNoCategory, "Version, licences and the fine print."},
};
static_assert(std::size(kPages) == static_cast<size_t>(Page::Count), "kPages must list every page in order");

const PageInfo& Info(Page p) {
  const auto i = static_cast<size_t>(p);
  return kPages[i < std::size(kPages) ? i : 0];
}

struct MenuState {
  Page page = Page::Home;
  Page shownPage = Page::Count;
  float pageFade = 1.0f;
  float barY = -1.0f;
  std::string focusFeature;
  std::string highlightFeature;
  double highlightUntil = 0.0;
  bool unloadRequested = false;
  std::array<std::array<char, 96>, static_cast<size_t>(Page::Count)> search{};
  std::unordered_set<std::string> expanded;
  std::unordered_map<std::string, float> cardHeights;

  uint64_t bindingsGen = ~0ull;
  double bindingsAt = -100.0;
  size_t bindOk = 0, bindTotal = 0, bindFailed = 0;
  std::string gameBuild;

  std::vector<std::string> profiles;
  bool profilesDirty = true;
  std::array<char, 64> profileName{};
  std::string pendingDelete;

  float scaleEdit = -1.0f;
  bool docsChecked = false;
  std::filesystem::path docsDir;
};
MenuState g_menu;

// ---- small drawing helpers ------------------------------------------------------------------------

float DrawTracked(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, const char* text, float tracking, ImU32 col) {
  float x = pos.x;
  const float extra = size * tracking;
  float width = 0.0f;
  for (const char* c = text; *c != '\0'; ++c) {
    const char glyph[2] = {*c, '\0'};
    dl->AddText(font, size, ImVec2(x, pos.y), col, glyph);
    const float adv = font->CalcTextSizeA(size, FLT_MAX, 0.0f, glyph).x;
    width = x + adv - pos.x;
    x += adv + extra;
  }
  return width;
}

void DrawStar(ImDrawList* dl, ImVec2 c, float r, bool filled, ImU32 col) {
  ImVec2 pts[10];
  for (int i = 0; i < 10; ++i) {
    const float a = -IM_PI * 0.5f + static_cast<float>(i) * IM_PI / 5.0f;
    const float rr = (i % 2 == 0) ? r : r * 0.45f;
    pts[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
  }
  if (filled) dl->AddConcavePolyFilled(pts, 10, col);
  else dl->AddPolyline(pts, 10, col, std::max(1.0f, r * 0.16f), ImDrawFlags_Closed);
}

float ButtonWidth(const std::string& label) {
  return ImGui::CalcTextSize(label.c_str(), nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

float IconButtonWidth(const char* fallback) {
  return HasIcons() ? ImGui::GetFrameHeight() : ButtonWidth(fallback);
}

ImVec4 StatusColor(const char* status) {
  const auto& p = th::Colors();
  if (status == nullptr) return p.textDim;
  const std::string_view s(status);
  if (s == "Ready") return p.success;
  if (util::StartsWith(s, "Blocked") || util::StartsWith(s, "Failed")) return p.danger;
  return p.warning;
}

void RefreshBindingSummary() {
  auto& b = game::Bindings::Get();
  const uint64_t gen = b.Generation();
  const double now = ImGui::GetTime();
  if (gen == g_menu.bindingsGen && now - g_menu.bindingsAt < 2.0) return;
  size_t ok = 0, total = 0, failed = 0;
  for (const auto& s : b.List()) {
    ++total;
    if (s.status == game::SymbolStatus::Resolved || s.status == game::SymbolStatus::Overridden) ++ok;
    else if (s.status == game::SymbolStatus::Failed) ++failed;
  }
  g_menu.bindOk = ok;
  g_menu.bindTotal = total;
  g_menu.bindFailed = failed;
  g_menu.gameBuild = b.GameBuild();
  g_menu.bindingsGen = gen;
  g_menu.bindingsAt = now;
}

bool LinkButton(const char* label) {
  const auto& p = th::Colors();
  ImGui::PushStyleColor(ImGuiCol_TextLink, p.accent);
  const bool clicked = ImGui::TextLink(label);
  ImGui::PopStyleColor();
  return clicked;
}

void PageHeader(const char* icon, const char* title, const std::string& subtitle) {
  const auto& p = th::Colors();
  if (HasIcons()) {
    PushBodySize(1.45f);
    ImGui::PushStyleColor(ImGuiCol_Text, p.accent);
    Icon(icon);
    ImGui::PopStyleColor();
    PopFont();
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 1.2f);
  }
  PushTitle(1.25f);
  ImGui::TextUnformatted(title);
  PopFont();
  if (!subtitle.empty()) TextWrappedColored(p.textDim, subtitle);
  ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f));
}

// ---- feature cards -----------------------------------------------------------------------------------

bool FavoriteButton(Feature& f) {
  const auto& p = th::Colors();
  const float sz = ImGui::GetFrameHeight();
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton("##fav", ImVec2(sz, sz), ImGuiButtonFlags_EnableNav);
  const bool hovered = ImGui::IsItemHovered();
  if (pressed) {
    f.favorite = !f.favorite;
    features::Registry::Get().Save();
  }
  const ImU32 col = Col(f.favorite ? p.accent : (hovered ? p.textDim : p.textFaint));
  DrawStar(ImGui::GetWindowDrawList(), pos + ImVec2(sz * 0.5f, sz * 0.52f), sz * 0.30f, f.favorite, col);
  ImGui::SetItemTooltip("%s", f.favorite ? "Remove from favorites" : "Add to favorites (shown on Home)");
  return pressed;
}

float ControlColumnWidth() {
  const float toggleW = std::round(std::round(ImGui::GetFrameHeight() * 0.70f) * 1.85f);
  return std::max(toggleW, ButtonWidth(IconLabel(CG_ICON_PLAY, "Run")));
}

void DrawFeatureControl(Feature& f, bool available, float width) {
  const auto& p = th::Colors();
  switch (f.GetKind()) {
    case Kind::Toggle: {
      bool on = f.Enabled();
      if (ToggleSwitch("##on", &on, available)) QueueFeatureSet(f.Id(), on);
      if (available) ImGui::SetItemTooltip("%s", on ? "On - click to switch off" : "Off - click to switch on");
      break;
    }
    case Kind::Action: {
      if (!available) ImGui::BeginDisabled();
      if (AccentButton(IconLabel(CG_ICON_PLAY, "Run").c_str(), ImVec2(width, 0.0f))) QueueFeatureTrigger(f.Id());
      if (!available) ImGui::EndDisabled();
      break;
    }
    case Kind::Panel: {
      ImGui::AlignTextToFramePadding();
      ImGui::PushStyleColor(ImGuiCol_Text, available ? p.accent : p.textFaint);
      Icon(features::CategoryIcon(f.GetCategory()), "");
      ImGui::PopStyleColor();
      break;
    }
  }
}

void DrawFeatureCard(Feature& f) {
  const auto& p = th::Colors();
  const ImGuiStyle& style = ImGui::GetStyle();
  const std::string& id = f.Id();
  ImGui::PushID(id.c_str());

  const float width = ImGui::GetContentRegionAvail().x;
  const bool focus = g_menu.focusFeature == id;
  if (const auto it = g_menu.cardHeights.find(id); it != g_menu.cardHeights.end() && !focus) {
    // Off-screen cards are replaced by a spacer of their last measured height.
    if (!ImGui::IsRectVisible(ImVec2(width, it->second))) {
      ImGui::Dummy(ImVec2(width, it->second));
      ImGui::PopID();
      return;
    }
  }

  const bool available = f.Available();
  const Kind kind = f.GetKind();
  const bool hasSettings = kind != Kind::Panel && f.HasSettings();
  const bool expanded = kind == Kind::Panel || (hasSettings && g_menu.expanded.count(id) != 0);
  const float ctrlW = ControlColumnWidth();

  BeginCard("##card");
  if (ImGui::BeginTable("##row", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX)) {
    ImGui::TableSetupColumn("ctrl", ImGuiTableColumnFlags_WidthFixed, ctrlW);
    ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("side", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    DrawFeatureControl(f, available, ctrlW);

    ImGui::TableSetColumnIndex(1);
    ImGui::AlignTextToFramePadding();
    PushBold();
    TextColored(available ? p.text : p.textDim, f.Name());
    PopFont();
    if (hasSettings && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      if (expanded) g_menu.expanded.erase(id);
      else g_menu.expanded.insert(id);
    }
    if (kind == Kind::Toggle && f.Enabled()) {
      ImGui::SameLine();
      Badge("ON", Col(WithAlpha(p.success, 0.22f)), Col(Mix(p.success, p.text, 0.25f)));
    }
    if (!f.Description().empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, available ? p.textDim : p.textFaint);
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextUnformatted(f.Description().c_str());
      ImGui::PopTextWrapPos();
      ImGui::PopStyleColor();
    }

    ImGui::TableSetColumnIndex(2);
    bool first = true;
    const auto next = [&first, &style] {
      if (!first) ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
      first = false;
    };
    if (!available) {
      next();
      ImGui::AlignTextToFramePadding();
      ImGui::PushStyleColor(ImGuiCol_Text, p.warning);
      Icon(CG_ICON_WARNING, "!");
      ImGui::PopStyleColor();
      if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        const std::string reason = f.UnavailableReason();
        ImGui::TextUnformatted(reason.empty() ? "Unavailable" : reason.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
      next();
      if (ImGui::SmallButton("Bindings")) OpenWindow("tools.bindings");
      ImGui::SetItemTooltip("Open the Bindings window to see which symbols are missing");
    }
    next();
    FavoriteButton(f);
    if (kind != Kind::Panel) {
      next();
      if (HotkeyButton("##key", &f.Key())) features::Registry::Get().Save();
    }
    if (hasSettings) {
      next();
      ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(p.panelHover, 0.0f));
      if (ImGui::ArrowButton("##more", expanded ? ImGuiDir_Down : ImGuiDir_Right)) {
        if (expanded) g_menu.expanded.erase(id);
        else g_menu.expanded.insert(id);
      }
      ImGui::PopStyleColor();
      ImGui::SetItemTooltip("%s", expanded ? "Hide settings" : "Settings");
    }
    ImGui::EndTable();
  }

  if (expanded) {
    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y * 0.25f));
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(a, ImVec2(a.x + ImGui::GetContentRegionAvail().x, a.y), Col(p.border));
    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y * 0.5f));
    const float indent = kind == Kind::Panel ? 0.0f : ctrlW + style.CellPadding.x * 2.0f;
    if (indent > 0.0f) ImGui::Indent(indent);
    if (!available) ImGui::BeginDisabled();
    RunGuarded(id.c_str(), [&f] { f.DrawSettings(); });
    if (!available) ImGui::EndDisabled();
    if (indent > 0.0f) ImGui::Unindent(indent);
  }
  EndCard();

  const ImVec2 cardMin = ImGui::GetItemRectMin();
  const ImVec2 cardMax = ImGui::GetItemRectMax();
  g_menu.cardHeights[id] = cardMax.y - cardMin.y;
  if (g_menu.highlightFeature == id) {
    const double now = ImGui::GetTime();
    if (now < g_menu.highlightUntil) {
      const float t = static_cast<float>((g_menu.highlightUntil - now) / 1.6);
      ImGui::GetWindowDrawList()->AddRect(cardMin, cardMax, Col(p.accent, std::clamp(t, 0.0f, 1.0f)), 8.0f * UiScale(), 2.0f);
    } else {
      g_menu.highlightFeature.clear();
    }
  }
  if (focus) {
    ImGui::SetScrollHereY(0.25f);
    g_menu.focusFeature.clear();
  }
  ImGui::PopID();
}

bool MatchesSearch(const Feature& f, const char* query) {
  if (query == nullptr || query[0] == '\0') return true;
  const std::string q = util::Trim(query);
  return q.empty() || util::IContains(f.Name(), q) || util::IContains(f.Description(), q) || util::IContains(f.Id(), q);
}

void DrawFeatureList(const std::vector<Feature*>& list, const char* query) {
  size_t shown = 0;
  for (Feature* f : list) {
    if (f == nullptr || !MatchesSearch(*f, query)) continue;
    DrawFeatureCard(*f);
    ++shown;
  }
  if (shown == 0 && !list.empty()) {
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize()));
    TextWrappedColored(th::Colors().textDim, std::string("No features match \"") + util::Trim(query) + "\".");
  }
}

// ---- pages ---------------------------------------------------------------------------------------------

void DrawEmptyCategory(Page page) {
  const auto& p = th::Colors();
  BeginCard("##empty");
  PushBold();
  ImGui::TextUnformatted("Nothing here yet");
  PopFont();
  if (page == Page::CheatTable) {
    TextWrappedColored(p.textDim,
                       "Cheat-table entries are declared in the \"cheats\" array of the binding files (Consigliere/bindings/*.json). "
                       "They appear here as soon as the bindings load - edit the JSON and use Reload bindings, no restart needed.");
  } else {
    TextWrappedColored(p.textDim,
                       "Features in this category appear once they are registered and their bindings resolve. If the game was "
                       "just updated, the Bindings window shows which symbols need attention.");
  }
  ImGui::Spacing();
  if (ImGui::Button(IconLabel(CG_ICON_LINK, "Open Bindings").c_str())) OpenWindow("tools.bindings");
  ImGui::SameLine();
  if (ImGui::Button(IconLabel(CG_ICON_REFRESH, "Reload bindings").c_str())) CmdReloadBindings();
  EndCard();
}

void DrawCategoryPage(Page page, Category cat) {
  const PageInfo& info = Info(page);
  const auto list = features::Registry::Get().InCategory(cat);
  size_t active = 0, unavailable = 0;
  for (const Feature* f : list) {
    if (f == nullptr) continue;
    if (f->GetKind() == Kind::Toggle && f->Enabled()) ++active;
    if (!f->Available()) ++unavailable;
  }
  std::string sub = info.blurb;
  if (!list.empty()) {
    sub += "  " + std::to_string(list.size()) + (list.size() == 1 ? " feature" : " features") + ", " + std::to_string(active) + " active";
    if (unavailable) sub += ", " + std::to_string(unavailable) + " waiting for bindings";
    sub += ".";
  }
  PageHeader(info.icon, info.name, sub);
  if (list.empty()) {
    DrawEmptyCategory(page);
    return;
  }
  auto& buf = g_menu.search[static_cast<size_t>(page)];
  SearchBox("##search", buf.data(), buf.size(), "Filter by name or description");
  ImGui::Spacing();
  DrawFeatureList(list, buf.data());
}

void DrawHome() {
  const auto& p = th::Colors();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float s = UiScale();

  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  const char* greeting = local.tm_hour < 5 ? "Working late." : local.tm_hour < 12 ? "Good morning." : local.tm_hour < 18 ? "Good afternoon." : "Good evening.";
  PageHeader(CG_ICON_HOME, greeting, "Everything you need, nothing you don't. Single-player only - what happens in Lost Heaven stays in Lost Heaven.");

  // Status + quick actions side by side.
  if (ImGui::BeginTable("##home.top", 2, ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    BeginCard("##status");
    SectionHeader("Status");
    const bool inGame = game::InGame();
    StatusDot(th::U32(inGame ? p.success : p.textFaint));
    ImGui::SameLine();
    ImGui::TextUnformatted(inGame ? "In game" : "Not in a game session (menu or loading)");
    if (inGame) {
      const auto hp = game::player::Health();
      const auto hpMax = game::player::MaxHealth();
      if (hp) {
        char line[64];
        if (hpMax && *hpMax > 0.0f) std::snprintf(line, sizeof(line), "Health  %.0f / %.0f", *hp, *hpMax);
        else std::snprintf(line, sizeof(line), "Health  %.0f", *hp);
        ImGui::PushStyleColor(ImGuiCol_Text, p.textDim);
        Icon(CG_ICON_HEALTH, "");
        ImGui::SameLine();
        ImGui::TextUnformatted(line);
        ImGui::PopStyleColor();
        if (hpMax && *hpMax > 0.0f) {
          const float frac = std::clamp(*hp / *hpMax, 0.0f, 1.0f);
          ImGui::PushStyleColor(ImGuiCol_PlotHistogram, frac > 0.5f ? p.success : frac > 0.2f ? p.warning : p.danger);
          ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 4.0f * s), "");
          ImGui::PopStyleColor();
        }
      }
      if (const auto pos = game::player::Position()) {
        char line[96];
        std::snprintf(line, sizeof(line), "%.1f, %.1f, %.1f", pos->x, pos->y, pos->z);
        ImGui::PushStyleColor(ImGuiCol_Text, p.textDim);
        Icon(CG_ICON_LOCATION, "");
        ImGui::SameLine();
        ImGui::PopStyleColor();
        PushMono();
        CopyableText(line);
        PopFont();
      }
      const bool driving = game::player::Vehicle().has_value();
      TextColored(p.textDim, driving ? "Driving" : "On foot");
    }
    RefreshBindingSummary();
    char bind[96];
    std::snprintf(bind, sizeof(bind), "%zu of %zu bindings resolved", g_menu.bindOk, g_menu.bindTotal);
    StatusDot(th::U32(g_menu.bindTotal == 0 ? p.textFaint : g_menu.bindOk == g_menu.bindTotal ? p.success : p.warning));
    ImGui::SameLine();
    ImGui::TextUnformatted(bind);
    if (!g_menu.gameBuild.empty()) TextColored(p.textFaint, "Bindings for: " + g_menu.gameBuild);
    EndCard();

    ImGui::TableSetColumnIndex(1);
    BeginCard("##actions");
    SectionHeader("Quick actions");
    const float bw = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(IconLabel(CG_ICON_SEARCH, "Command palette").c_str(), ImVec2(bw, 0))) OpenPalette();
    if (ImGui::Button(IconLabel(CG_ICON_REFRESH, "Reload bindings").c_str(), ImVec2(bw, 0))) CmdReloadBindings();
    if (ImGui::Button(IconLabel(CG_ICON_FOLDER, "Open data folder").c_str(), ImVec2(bw, 0))) CmdOpenFolder(paths::DataDir());
    if (DangerButton(IconLabel(CG_ICON_POWER, "Disable all features").c_str(), ImVec2(bw, 0))) CmdDisableAll();
    EndCard();
    ImGui::EndTable();
  }

  // Favorites grid.
  SectionHeader("Favorites");
  std::vector<Feature*> favs;
  std::vector<Feature*> active;
  for (const auto& up : features::Registry::Get().All()) {
    if (!up) continue;
    if (up->favorite) favs.push_back(up.get());
    if (up->GetKind() == Kind::Toggle && up->Enabled()) active.push_back(up.get());
  }
  if (favs.empty()) {
    TextWrappedColored(p.textFaint, "Star a feature on any page to pin it here.");
  } else {
    const float cardW = 232.0f * s;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>((avail + style.ItemSpacing.x) / (cardW + style.ItemSpacing.x)));
    const float w = (avail - style.ItemSpacing.x * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    for (size_t i = 0; i < favs.size(); ++i) {
      Feature& f = *favs[i];
      if (i % static_cast<size_t>(cols) != 0) ImGui::SameLine();
      ImGui::PushID(f.Id().c_str());
      BeginCard("##fav", ImVec2(w, 0.0f));
      const bool available = f.Available();
      ImGui::PushStyleColor(ImGuiCol_Text, p.accent);
      Icon(features::CategoryIcon(f.GetCategory()), "");
      ImGui::PopStyleColor();
      ImGui::SameLine();
      PushBold();
      TextColored(available ? p.text : p.textDim, f.Name());
      PopFont();
      TextColored(p.textFaint, features::CategoryName(f.GetCategory()));
      switch (f.GetKind()) {
        case Kind::Toggle: {
          bool on = f.Enabled();
          if (ToggleSwitch("##on", &on, available)) QueueFeatureSet(f.Id(), on);
          ImGui::SameLine();
          ImGui::AlignTextToFramePadding();
          TextColored(on ? p.success : p.textFaint, on ? "On" : "Off");
          break;
        }
        case Kind::Action:
          if (!available) ImGui::BeginDisabled();
          if (AccentButton(IconLabel(CG_ICON_PLAY, "Run").c_str())) QueueFeatureTrigger(f.Id());
          if (!available) ImGui::EndDisabled();
          break;
        case Kind::Panel:
          if (ImGui::Button("Open")) NavigateTo(PageForCategory(f.GetCategory()), f.Id());
          break;
      }
      if (!available) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, p.warning);
        Icon(CG_ICON_WARNING, "!");
        ImGui::PopStyleColor();
        if (ImGui::BeginItemTooltip()) {
          const std::string reason = f.UnavailableReason();
          ImGui::TextUnformatted(reason.empty() ? "Unavailable" : reason.c_str());
          ImGui::EndTooltip();
        }
      }
      EndCard();
      ImGui::PopID();
    }
  }

  SectionHeader("Active now");
  if (active.empty()) {
    TextColored(p.textFaint, "Nothing switched on.");
  } else {
    for (Feature* f : active) {
      ImGui::PushID(f->Id().c_str());
      bool on = true;
      if (ToggleSwitch("##on", &on)) QueueFeatureSet(f->Id(), on);
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(f->Name().c_str());
      ImGui::SameLine();
      TextColored(p.textFaint, features::CategoryName(f->GetCategory()));
      ImGui::PopID();
    }
  }

  SectionHeader("Tips");
  const auto& st = Settings();
  const auto tip = [&](const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, p.accentDim);
    ImGui::Bullet();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    TextWrappedColored(p.textDim, text);
  };
  tip(st.menuKey.ToString() + " opens and closes this menu.");
  tip("Ctrl+P opens the command palette: fuzzy-search every feature, page, tool and command.");
  tip(st.panicKey.ToString() + " is the panic key: every feature switches off at once.");
  tip("Right-click a tool window's title bar (or use its pin) to keep it on screen while the menu is closed.");
  tip("Every feature can be bound to a key: click the key button on its card and press the key.");
}

void DrawScriptsPage() {
  const auto& p = th::Colors();
  const auto dir = paths::Data(L"scripts");
  PageHeader(CG_ICON_SCRIPT, "Scripts", "Lua 5.4 scripts from " + util::Narrow(dir.wstring()) + ". Each script runs in its own sandbox.");

  if (AccentButton(IconLabel(CG_ICON_REFRESH, "Reload all").c_str())) CmdReloadScripts();
  ImGui::SameLine();
  if (ImGui::Button(IconLabel(CG_ICON_FOLDER, "Open scripts folder").c_str())) CmdOpenFolder(dir);
  ImGui::SameLine();
  if (ImGui::Button(IconLabel(CG_ICON_CONSOLE, "Script console").c_str())) OpenWindow("tools.scriptconsole");

  SectionHeader("Loaded scripts");
  const auto scripts = script::Engine::Get().List();
  if (scripts.empty()) {
    TextWrappedColored(p.textFaint, "No scripts yet. Drop .lua files into the scripts folder and press Reload all.");
  } else if (ImGui::BeginTable("##scripts", 5,
                               ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit |
                                   ImGuiTableFlags_PadOuterX)) {
    ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Script", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Tick", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (const auto& sc : scripts) {
      ImGui::PushID(sc.name.c_str());
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      bool enabled = sc.enabled;
      if (ToggleSwitch("##en", &enabled)) {
        const std::string name = sc.name;
        tasks::PostRender([name, enabled] {
          RunGuarded("script enable", [&] { script::Engine::Get().SetEnabled(name, enabled); });
        });
      }
      ImGui::TableSetColumnIndex(1);
      ImGui::AlignTextToFramePadding();
      PushBold();
      ImGui::TextUnformatted(sc.name.c_str());
      PopFont();
      ImGui::SetItemTooltip("%s", util::Narrow(sc.path.wstring()).c_str());
      ImGui::TableSetColumnIndex(2);
      ImGui::AlignTextToFramePadding();
      if (!sc.error.empty()) TextColored(DangerText(), "Error");
      else if (!sc.enabled) TextColored(p.textFaint, "Disabled");
      else if (sc.loaded) TextColored(p.success, "Running");
      else TextColored(p.warning, "Not loaded");
      ImGui::TableSetColumnIndex(3);
      ImGui::AlignTextToFramePadding();
      char tick[32];
      std::snprintf(tick, sizeof(tick), "%.2f ms", sc.lastTickMs);
      TextColored(sc.lastTickMs > 1.0 ? p.warning : p.textDim, tick);
      ImGui::SetItemTooltip("Time spent in this script's tick handlers last frame");
      ImGui::TableSetColumnIndex(4);
      if (IconButton(CG_ICON_REFRESH, "Reload", "Reload this script")) {
        const std::string name = sc.name;
        tasks::PostRender([name] {
          bool ok = false;
          RunGuarded("script reload", [&] { ok = script::Engine::Get().Reload(name); });
          notify::Push(ok ? notify::Kind::Success : notify::Kind::Error, ok ? "Script reloaded" : "Script failed to load", name);
        });
      }
      if (!sc.error.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        PushMono(0.95f);
        TextWrappedColored(DangerText(), sc.error);
        PopFont();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  const auto scriptFeatures = features::Registry::Get().InCategory(Category::Scripts);
  if (!scriptFeatures.empty()) {
    SectionHeader("Script features");
    DrawFeatureList(scriptFeatures, nullptr);
  }

  SectionHeader("Script panels");
  const ImVec2 before = ImGui::GetCursorScreenPos();
  RunGuarded("script menus", [] { script::Engine::Get().DrawMenu(); });
  if (ImGui::GetCursorScreenPos().y == before.y) TextColored(p.textFaint, "Scripts that handle the \"menu\" event draw their panels here.");
}

void DrawToolsPage() {
  const auto& p = th::Colors();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float s = UiScale();
  PageHeader(CG_ICON_DEVTOOLS, "Tools",
             "The reverse-engineering toolkit. Windows stay open while you browse the menu; pin one to keep it on screen in game.");
  const auto& windows = Windows();
  if (windows.empty()) {
    TextWrappedColored(p.textFaint, "No tool windows are registered.");
    return;
  }
  std::vector<std::string> groups;
  for (const auto& w : windows) {
    const std::string g = w->Group() != nullptr ? w->Group() : "Tools";
    if (std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(g);
  }
  const float cardW = 250.0f * s;
  for (const std::string& group : groups) {
    SectionHeader(group.c_str());
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, static_cast<int>((avail + style.ItemSpacing.x) / (cardW + style.ItemSpacing.x)));
    const float w = (avail - style.ItemSpacing.x * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    int col = 0;
    for (const auto& win : windows) {
      const char* g = win->Group() != nullptr ? win->Group() : "Tools";
      if (group != g) continue;
      if (col++ % cols != 0) ImGui::SameLine();
      ImGui::PushID(win->Id());
      BeginCard("##tool", ImVec2(w, 0.0f));
      if (HasIcons()) {
        PushBodySize(1.3f);
        ImGui::PushStyleColor(ImGuiCol_Text, p.accent);
        Icon(win->Icon());
        ImGui::PopStyleColor();
        PopFont();
        ImGui::SameLine();
      }
      ImGui::BeginGroup();
      PushBold();
      ImGui::TextUnformatted(win->Title() != nullptr ? win->Title() : win->Id());
      PopFont();
      PushMono(0.85f);
      TextColored(p.textFaint, win->Id());
      PopFont();
      ImGui::EndGroup();
      if (win->open) {
        if (ImGui::Button(IconLabel(CG_ICON_OPEN_WINDOW, "Focus").c_str())) OpenWindow(win->Id());
        ImGui::SameLine();
        if (ImGui::Button("Close")) win->open = false;
      } else if (AccentButton(IconLabel(CG_ICON_OPEN_WINDOW, "Open").c_str())) {
        OpenWindow(win->Id());
      }
      ImGui::SameLine();
      bool pinned = win->pinned;
      if (ToggleSwitch("##pin", &pinned)) win->pinned = pinned;
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      TextColored(pinned ? p.accent : p.textFaint, "Pinned");
      ImGui::SetItemTooltip("Pinned windows stay visible (click-through) while the menu is closed");
      EndCard();
      ImGui::PopID();
    }
  }
}

// Label column + control column rows for the settings page.
bool BeginSettingsTable(const char* id) {
  if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
  ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 210.0f * UiScale());
  ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch);
  return true;
}

void SettingsLabel(const char* label, const char* help = nullptr) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (help != nullptr) {
    ImGui::SameLine();
    HelpMarker(help);
  }
  ImGui::TableSetColumnIndex(1);
}

void ConfigToggleRow(const char* label, const char* key, bool current, const char* help = nullptr) {
  SettingsLabel(label, help);
  bool v = current;
  ImGui::PushID(key);
  if (ToggleSwitch("##v", &v)) {
    Config::Get().WriteBool(key, v);
    InvalidateSettings();
  }
  ImGui::PopID();
}

bool ValidProfileName(std::string_view name) {
  if (name.empty() || name.size() > 48 || name.front() == '.' || name.front() == ' ' || name.back() == ' ' || name.back() == '.') return false;
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' || c == '.';
    if (!ok) return false;
  }
  return true;
}

void HotkeyRow(const char* label, const char* key, Hotkey current, bool allowUnbound, const char* help) {
  SettingsLabel(label, help);
  Hotkey hk = current;
  if (HotkeyButton(key, &hk)) {
    if (!hk.Valid() && !allowUnbound) {
      notify::Push(notify::Kind::Warning, "The menu key can't be unbound", "Without it the menu could never be opened again");
    } else {
      Config::Get().WriteString(key, hk.ToString());
      InvalidateSettings();
    }
  }
}

void DrawSettingsPage() {
  const auto& p = th::Colors();
  const float s = UiScale();
  PageHeader(CG_ICON_SETTINGS, "Settings", "Changes are saved automatically.");

  // Appearance.
  SectionHeader("Appearance");
  if (BeginSettingsTable("##appearance")) {
    SettingsLabel("Theme");
    int preset = static_cast<int>(th::Current());
    ImGui::SetNextItemWidth(220.0f * s);
    if (ImGui::BeginCombo("##theme", th::PresetName(th::Current()))) {
      for (int i = 0; i < 4; ++i) {
        const auto pr = static_cast<th::Preset>(i);
        if (ImGui::Selectable(th::PresetName(pr), i == preset)) RequestAppearance(i, UiScale());
        if (i == preset) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    const float sw = ImGui::GetFrameHeight() * 0.6f;
    const ImVec4 swatches[] = {p.accent, p.danger, p.warning, p.success, p.info};
    for (const ImVec4& c : swatches) {
      const ImVec2 at = ImGui::GetCursorScreenPos() + ImVec2(0.0f, (ImGui::GetFrameHeight() - sw) * 0.5f);
      ImGui::GetWindowDrawList()->AddRectFilled(at, at + ImVec2(sw, sw), Col(c), sw * 0.25f);
      ImGui::Dummy(ImVec2(sw, ImGui::GetFrameHeight()));
      ImGui::SameLine(0.0f, 4.0f * s);
    }
    ImGui::NewLine();

    SettingsLabel("UI scale", "Applied when you release the slider. Fonts are re-rasterised at the new size, no restart needed.");
    if (g_menu.scaleEdit < 0.0f) g_menu.scaleEdit = UiScale();
    ImGui::SetNextItemWidth(220.0f * s);
    ImGui::SliderFloat("##scale", &g_menu.scaleEdit, 0.75f, 2.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) RequestAppearance(preset, g_menu.scaleEdit);
    if (!ImGui::IsItemActive() && !ImGui::IsItemDeactivatedAfterEdit()) g_menu.scaleEdit = -1.0f;
    ImGui::SameLine();
    if (ImGui::Button("1.00x")) RequestAppearance(preset, 1.0f);
    ImGui::EndTable();
  }

  // Hotkeys.
  SectionHeader("Hotkeys");
  const ShellSettings& st = Settings();
  if (BeginSettingsTable("##hotkeys")) {
    HotkeyRow("Toggle menu", kCfgMenuKey, st.menuKey, false, "Opens and closes Consigliere. It cannot be unbound.");
    HotkeyRow("Panic", kCfgPanicKey, st.panicKey, true, "Switches every feature off immediately.");
    HotkeyRow("Unload Consigliere", kCfgUnloadKey, st.unloadKey, true, "Removes Consigliere from the game, restoring everything it changed.");
    ImGui::EndTable();
  }
  const Hotkey keys[] = {st.menuKey, st.panicKey, st.unloadKey};
  bool clash = false;
  for (int i = 0; i < 3; ++i)
    for (int j = i + 1; j < 3; ++j) clash |= keys[i].Valid() && keys[i] == keys[j];
  if (clash) TextColored(p.warning, IconLabel(CG_ICON_WARNING, "Two system hotkeys share the same key; only the first action will fire reliably."));

  // HUD.
  SectionHeader("In-game HUD");
  if (BeginSettingsTable("##hud")) {
    ConfigToggleRow("Watermark", kCfgHudWatermark, st.hudWatermark, "Shows CONSIGLIERE and the menu key for a few seconds after loading and after closing the menu.");
    ConfigToggleRow("FPS counter", kCfgHudFps, st.hudFps);
    ConfigToggleRow("Active features list", kCfgHudActive, st.hudActive, "Lists the features that are switched on along the right edge.");
    ConfigToggleRow("Player position", kCfgHudPosition, st.hudPosition);
    ImGui::EndTable();
  }

  // Notifications and logging.
  SectionHeader("Notifications & logging");
  if (BeginSettingsTable("##notify")) {
    ConfigToggleRow("Notifications", kCfgNotifications, st.notifications, "Toasts in the bottom-right corner. Warnings and errors are always shown.");
    SettingsLabel("Log level", "Minimum severity written to consigliere.log and the Log console.");
    const int level = static_cast<int>(log::MinLevel());
    ImGui::SetNextItemWidth(220.0f * s);
    if (ImGui::BeginCombo("##loglevel", log::LevelName(log::MinLevel()))) {
      for (int i = 0; i <= static_cast<int>(log::Level::Error); ++i) {
        const auto lv = static_cast<log::Level>(i);
        if (ImGui::Selectable(log::LevelName(lv), i == level)) {
          log::SetMinLevel(lv);
          Config::Get().WriteInt(kCfgLogLevel, i);
        }
        if (i == level) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(IconLabel(CG_ICON_CONSOLE, "Log console").c_str())) OpenWindow("tools.log");
    ImGui::EndTable();
  }

  // Profiles.
  SectionHeader("Profiles");
  TextWrappedColored(p.textDim, "A profile is a complete snapshot of your settings, features and hotkeys.");
  if (g_menu.profilesDirty) {
    g_menu.profiles = Config::Get().ListProfiles();
    g_menu.profilesDirty = false;
  }
  ImGui::SetNextItemWidth(220.0f * s);
  const bool enter = ImGui::InputTextWithHint("##profile", "new profile name", g_menu.profileName.data(), g_menu.profileName.size(),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  const std::string newName = util::Trim(g_menu.profileName.data());
  const bool validName = ValidProfileName(newName);
  if (!validName) ImGui::BeginDisabled();
  if ((AccentButton(IconLabel(CG_ICON_SAVE, "Save as").c_str()) || enter) && validName) {
    features::Registry::Get().Save();
    if (Config::Get().SaveProfile(newName)) {
      notify::Push(notify::Kind::Success, "Profile saved", newName);
      g_menu.profileName.fill('\0');
    } else {
      notify::Push(notify::Kind::Error, "Saving profile failed", newName);
    }
    g_menu.profilesDirty = true;
  }
  if (!validName) ImGui::EndDisabled();
  if (!newName.empty() && !validName) {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    TextColored(p.warning, "Letters, digits, space, - _ . only (max 48)");
  }
  ImGui::SameLine();
  if (IconButton(CG_ICON_REFRESH, "Refresh", "Refresh the list")) g_menu.profilesDirty = true;

  if (g_menu.profiles.empty()) {
    TextColored(p.textFaint, "No saved profiles.");
  } else if (ImGui::BeginTable("##profiles", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) {
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed);
    for (const std::string& name : g_menu.profiles) {
      ImGui::PushID(name.c_str());
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(name.c_str());
      ImGui::TableSetColumnIndex(1);
      if (ImGui::Button("Load")) {
        const std::string n = name;
        // Loading replaces the whole store and re-enables features: do it between frames.
        tasks::PostRender([n] {
          if (Config::Get().LoadProfile(n)) notify::Push(notify::Kind::Success, "Profile loaded", n);
          else notify::Push(notify::Kind::Error, "Loading profile failed", n);
        });
      }
      ImGui::SameLine();
      if (ImGui::Button("Delete")) {
        g_menu.pendingDelete = name;
        ImGui::OpenPopup("##confirmdelete");
      }
      if (ImGui::BeginPopup("##confirmdelete")) {
        ImGui::Text("Delete profile \"%s\"?", g_menu.pendingDelete.c_str());
        if (DangerButton("Delete")) {
          if (Config::Get().DeleteProfile(g_menu.pendingDelete)) notify::Push(notify::Kind::Info, "Profile deleted", g_menu.pendingDelete);
          else notify::Push(notify::Kind::Error, "Deleting profile failed", g_menu.pendingDelete);
          g_menu.profilesDirty = true;
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  // Data.
  SectionHeader("Data");
  if (ImGui::Button(IconLabel(CG_ICON_FOLDER, "Open data folder").c_str())) CmdOpenFolder(paths::DataDir());
  ImGui::SameLine();
  if (ImGui::Button(IconLabel(CG_ICON_SAVE, "Save configuration now").c_str())) CmdSaveConfig();
  ImGui::SameLine();
  if (ImGui::Button(IconLabel(CG_ICON_REFRESH, "Reload bindings").c_str())) CmdReloadBindings();
  TextColored(p.textFaint, util::Narrow(paths::DataDir().wstring()));

  SectionHeader("Danger zone");
  TextWrappedColored(p.textDim, "Unloading switches every feature off, restores every patched byte, removes all hooks and frees the DLL. "
                                "The game keeps running exactly as it was before Consigliere arrived.");
  if (DangerButton(IconLabel(CG_ICON_POWER, "Unload Consigliere").c_str())) RequestUnloadConfirmation();
}

void DrawAboutPage() {
  const auto& p = th::Colors();
  PageHeader(CG_ICON_INFO, "About", "");
  BeginCard("##about");
  PushTitle(1.3f);
  TextColored(p.accent, CG_NAME);
  PopFont();
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  TextColored(p.textDim, "version " CG_VERSION);
  TextWrappedColored(p.text,
                     "A mod menu and reverse-engineering toolkit for Mafia: Definitive Edition. Game knowledge lives in "
                     "editable binding files, so a game patch is fixed by editing JSON instead of waiting for a rebuild.");
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, p.success);
  Icon(CG_ICON_SHIELD, "");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  TextWrappedColored(p.text,
                     "Single-player only. If a multiplayer client is detected in the process, every gameplay feature, patch "
                     "and script memory write is refused.");
  EndCard();

  SectionHeader("Credits & licences");
  struct Credit {
    const char* name;
    const char* license;
    const char* who;
  };
  static constexpr Credit kCredits[] = {
      {"Dear ImGui " IMGUI_VERSION, "MIT", "Omar Cornut and contributors"},
      {"MinHook", "BSD-2-Clause", "Tsuda Kageyu and contributors"},
      {"nlohmann/json", "MIT", "Niels Lohmann"},
      {"Lua 5.4", "MIT", "Lua.org, PUC-Rio"},
  };
  if (ImGui::BeginTable("##credits", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
    for (const Credit& c : kCredits) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      PushBold();
      ImGui::TextUnformatted(c.name);
      PopFont();
      ImGui::TableSetColumnIndex(1);
      Badge(c.license, Col(WithAlpha(p.accent, 0.18f)), Col(p.accent));
      ImGui::TableSetColumnIndex(2);
      TextColored(p.textDim, c.who);
    }
    ImGui::EndTable();
  }
  TextWrappedColored(p.textFaint,
                     "Fonts (Segoe UI, Consolas, Segoe Fluent Icons / MDL2 Assets) are loaded from your Windows installation and "
                     "are not redistributed.");

  SectionHeader("Documentation");
  if (!g_menu.docsChecked) {
    g_menu.docsChecked = true;
    std::error_code ec;
    for (const auto& candidate : {paths::DataDir() / L"docs", paths::ModuleDir() / L"docs"}) {
      if (std::filesystem::is_directory(candidate, ec)) {
        g_menu.docsDir = candidate;
        break;
      }
    }
  }
  TextWrappedColored(p.textDim,
                     "ARCHITECTURE.md explains how the pieces fit, BINDINGS.md documents the binding file format and every "
                     "canonical symbol, SCRIPTING.md is the Lua API reference.");
  if (!g_menu.docsDir.empty()) {
    if (LinkButton("Open the documentation folder")) CmdOpenFolder(g_menu.docsDir);
  } else {
    TextColored(p.textFaint, "The docs folder ships with the Consigliere source package.");
  }

  SectionHeader("Build");
#if defined(_MSC_VER)
  ImGui::Text("Compiler: MSVC %d", _MSC_VER);
#elif defined(__clang__)
  ImGui::Text("Compiler: Clang %s", __clang_version__);
#elif defined(__GNUC__)
  ImGui::Text("Compiler: GCC %s", __VERSION__);
#endif
  TextColored(p.textDim, "Built " __DATE__);
}

void DrawPage(Page page) {
  switch (page) {
    case Page::Home: DrawHome(); break;
    case Page::Scripts: DrawScriptsPage(); break;
    case Page::Tools: DrawToolsPage(); break;
    case Page::Settings: DrawSettingsPage(); break;
    case Page::About: DrawAboutPage(); break;
    default:
      if (const auto cat = PageCategory(page)) DrawCategoryPage(page, *cat);
      else DrawHome();
      break;
  }
}

// ---- chrome --------------------------------------------------------------------------------------------

void DrawHeader(float height) {
  const auto& p = th::Colors();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float s = UiScale();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const ImVec2 ws = ImGui::GetWindowSize();

  dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + height), Col(p.bgAlt), style.WindowRounding, ImDrawFlags_RoundCornersTop);
  const float mid = wp.x + ws.x * 0.5f;
  const float ry = wp.y + height - 1.0f;
  dl->AddRectFilledMultiColor(ImVec2(wp.x, ry), ImVec2(mid, ry + 1.0f), Col(p.accent, 0.0f), Col(p.accent, 0.85f), Col(p.accent, 0.85f),
                              Col(p.accent, 0.0f));
  dl->AddRectFilledMultiColor(ImVec2(mid, ry), ImVec2(wp.x + ws.x, ry + 1.0f), Col(p.accent, 0.85f), Col(p.accent, 0.0f), Col(p.accent, 0.0f),
                              Col(p.accent, 0.85f));

  // Wordmark.
  float x = wp.x + 22.0f * s;
  PushTitle(1.3f);
  ImFont* titleFont = ImGui::GetFont();
  const float titleSize = ImGui::GetFontSize();
  PopFont();
  const float titleY = wp.y + (height - titleSize) * 0.5f;
  x += DrawTracked(dl, titleFont, titleSize, ImVec2(x, titleY), "CONSIGLIERE", 0.16f, Col(p.accent));
  x += 10.0f * s;
  {
    PushBodySize(0.82f);
    const char* ver = "v" CG_VERSION;
    const float fs = ImGui::GetFontSize();
    dl->AddText(ImGui::GetFont(), fs, ImVec2(x, titleY + titleSize - fs - 1.0f * s), Col(p.textFaint), ver);
    x += ImGui::CalcTextSize(ver).x;
    PopFont();
  }
  x += 20.0f * s;

  const float frameH = ImGui::GetFrameHeight();
  const float rowY = wp.y + (height - frameH) * 0.5f;
  ImGui::SetCursorScreenPos(ImVec2(x, rowY));
  const char* status = StatusText();
  std::string statusText = status != nullptr ? status : "Unknown";
  auto& bindings = game::Bindings::Get();
  if (bindings.Resolving()) {
    char pct[16];
    std::snprintf(pct, sizeof(pct), " %d%%", static_cast<int>(std::clamp(bindings.Progress(), 0.0f, 1.0f) * 100.0f));
    statusText += pct;
  }
  StatusPill(statusText.c_str(), StatusColor(status));

  ImGui::SameLine(0.0f, 10.0f * s);
  RefreshBindingSummary();
  char summary[64];
  std::snprintf(summary, sizeof(summary), "%zu/%zu bindings", g_menu.bindOk, g_menu.bindTotal);
  ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(p.panelHover, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, g_menu.bindTotal > 0 && g_menu.bindOk == g_menu.bindTotal ? p.textDim : p.warning);
  if (ImGui::Button(IconLabel(CG_ICON_LINK, summary).c_str())) OpenWindow("tools.bindings");
  ImGui::PopStyleColor(2);
  if (ImGui::BeginItemTooltip()) {
    ImGui::Text("%zu resolved, %zu failed, %zu pending", g_menu.bindOk, g_menu.bindFailed, g_menu.bindTotal - g_menu.bindOk - g_menu.bindFailed);
    if (!g_menu.gameBuild.empty()) ImGui::Text("Bindings for: %s", g_menu.gameBuild.c_str());
    TextColored(p.textFaint, "Click to open the Bindings window");
    ImGui::EndTooltip();
  }

  // Right cluster: palette, panic, close.
  const std::string paletteLabel = IconLabel(CG_ICON_SEARCH, "Search   Ctrl+P");
  const float paletteW = ButtonWidth(paletteLabel);
  const float panicW = IconButtonWidth("Panic");
  const float closeW = IconButtonWidth("X");
  const float right = wp.x + ws.x - 14.0f * s;
  const float startX = right - closeW - panicW - paletteW - style.ItemSpacing.x * 2.0f;
  if (startX > ImGui::GetItemRectMax().x + style.ItemSpacing.x) {
    ImGui::SetCursorScreenPos(ImVec2(startX, rowY));
    ImGui::PushStyleColor(ImGuiCol_Button, p.panel);
    ImGui::PushStyleColor(ImGuiCol_Text, p.textDim);
    if (ImGui::Button(paletteLabel.c_str())) OpenPalette();
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("Command palette: search features, pages, tools and commands");
    ImGui::SameLine();
  } else {
    ImGui::SetCursorScreenPos(ImVec2(right - closeW - panicW - style.ItemSpacing.x, rowY));
  }
  ImGui::PushStyleColor(ImGuiCol_Text, p.danger);
  if (IconButton(CG_ICON_POWER, "Panic", ("Panic: disable every feature (" + Settings().panicKey.ToString() + ")").c_str())) CmdDisableAll();
  ImGui::PopStyleColor();
  ImGui::SameLine();
  if (IconButton(CG_ICON_CLOSE, "X", ("Close menu (" + Settings().menuKey.ToString() + ")").c_str())) render::SetMenuOpen(false);
}

float DrawMpBanner(float top) {
  if (!mp_guard::Blocked()) return 0.0f;
  const auto& p = th::Colors();
  const float s = UiScale();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const float w = ImGui::GetWindowSize().x;
  const float h = ImGui::GetFrameHeight() + 8.0f * s;
  dl->AddRectFilled(ImVec2(wp.x, top), ImVec2(wp.x + w, top + h), Col(p.danger, 0.92f));
  std::string text = "Multiplayer client detected - all gameplay features are disabled.";
  if (!mp_guard::Reason().empty()) text += "  " + mp_guard::Reason();
  const float fs = ImGui::GetFontSize();
  const ImVec2 at(wp.x + 22.0f * s, top + (h - fs) * 0.5f);
  if (HasIcons()) {
    dl->AddText(render::GetFonts().body, fs, at, Col(p.text), CG_ICON_WARNING);
    dl->AddText(ImGui::GetFont(), fs, at + ImVec2(fs * 1.6f, 0.0f), Col(p.text), text.c_str());
  } else {
    dl->AddText(ImGui::GetFont(), fs, at, Col(p.text), text.c_str());
  }
  return h;
}

void DrawSidebar(const std::array<int, static_cast<size_t>(Category::Count)>& activeCounts) {
  const auto& p = th::Colors();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float s = UiScale();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float rowH = ImGui::GetFrameHeight() * 1.08f;
  const float rowW = ImGui::GetContentRegionAvail().x;
  float selectedY = -1.0f;

  const auto group = [&](const char* label) {
    ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
    PushBold(0.72f);
    TextColored(p.textFaint, label);
    PopFont();
  };
  const auto item = [&](Page page) {
    const PageInfo& info = Info(page);
    ImGui::PushID(info.key);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##page", ImVec2(rowW, rowH), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    const bool selected = g_menu.page == page;
    if (pressed) NavigateTo(page);
    if (selected) selectedY = pos.y;
    if (selected) dl->AddRectFilled(pos, pos + ImVec2(rowW, rowH), Col(Mix(p.bgAlt, p.accentDim, 0.35f)), style.FrameRounding);
    else if (hovered) dl->AddRectFilled(pos, pos + ImVec2(rowW, rowH), Col(p.panelHover, 0.7f), style.FrameRounding);
    const ImU32 col = Col(selected ? p.accent : hovered ? p.text : p.textDim);
    const float fs = ImGui::GetFontSize();
    float tx = pos.x + 12.0f * s;
    const float ty = pos.y + (rowH - fs) * 0.5f;
    if (HasIcons()) {
      dl->AddText(render::GetFonts().body, fs, ImVec2(tx, ty), col, info.icon);
      tx += fs * 1.7f;
    }
    dl->AddText(ImGui::GetFont(), fs, ImVec2(tx, ty), col, info.name);
    if (info.category != kNoCategory) {
      const int n = activeCounts[static_cast<size_t>(info.category)];
      if (n > 0) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", n);
        const float bfs = fs * 0.78f;
        const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(bfs, FLT_MAX, 0.0f, buf);
        const float bh = fs * 0.95f;
        const float bw = std::max(bh, ts.x + bh * 0.6f);
        const ImVec2 b0(pos.x + rowW - 10.0f * s - bw, pos.y + (rowH - bh) * 0.5f);
        dl->AddRectFilled(b0, b0 + ImVec2(bw, bh), Col(WithAlpha(p.success, 0.22f)), bh * 0.5f);
        dl->AddText(ImGui::GetFont(), bfs, b0 + ImVec2((bw - ts.x) * 0.5f, (bh - ts.y) * 0.5f), Col(Mix(p.success, p.text, 0.3f)), buf);
      }
    }
    ImGui::PopID();
  };

  item(Page::Home);
  group("GAMEPLAY");
  for (Page pg : {Page::Player, Page::Vehicle, Page::Weapons, Page::World, Page::Teleport, Page::Camera, Page::Visuals, Page::CheatTable}) item(pg);
  group("EXTEND");
  item(Page::Scripts);
  item(Page::Tools);
  group("CONSIGLIERE");
  item(Page::Settings);
  item(Page::About);

  // Animated selection marker.
  if (selectedY >= 0.0f) {
    g_menu.barY = g_menu.barY < 0.0f ? selectedY : Approach(g_menu.barY, selectedY, 18.0f);
    const float x = ImGui::GetWindowPos().x + 3.0f * s;
    const float inset = rowH * 0.22f;
    dl->AddRectFilled(ImVec2(x, g_menu.barY + inset), ImVec2(x + 3.0f * s, g_menu.barY + rowH - inset), Col(p.accent), 2.0f * s);
  }

  // Footer: game state.
  const float footerH = ImGui::GetTextLineHeightWithSpacing() * 2.0f + style.WindowPadding.y;
  const float y = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - footerH;
  if (y > ImGui::GetCursorScreenPos().y + style.ItemSpacing.y) {
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y));
    const bool inGame = game::InGame();
    StatusDot(th::U32(inGame ? p.success : p.textFaint));
    ImGui::SameLine(0.0f, 4.0f * s);
    TextColored(p.textDim, inGame ? "In game" : "Not in game");
    char fps[32];
    std::snprintf(fps, sizeof(fps), "%.0f FPS", static_cast<double>(render::Fps()));
    TextColored(p.textFaint, fps);
  }
}

void DrawUnloadPopup() {
  const auto& p = th::Colors();
  if (g_menu.unloadRequested) {
    ImGui::OpenPopup("Unload Consigliere?###cg.unload");
    g_menu.unloadRequested = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("Unload Consigliere?###cg.unload", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
    ImGui::TextUnformatted("Every feature is switched off, patches are restored and hooks removed. The game keeps running.");
    TextColored(p.textDim, "Inject Consigliere again to bring it back.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (DangerButton(IconLabel(CG_ICON_POWER, "Unload").c_str())) {
      ImGui::CloseCurrentPopup();
      RequestUnload();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::SetItemDefaultFocus();
    ImGui::EndPopup();
  }
}

}  // namespace

// ---- page registry -----------------------------------------------------------------------------------

const char* PageName(Page p) { return Info(p).name; }
const char* PageKey(Page p) { return Info(p).key; }
const char* PageIcon(Page p) { return Info(p).icon; }

std::optional<Page> PageFromKey(std::string_view key) {
  for (const PageInfo& info : kPages)
    if (util::IEquals(key, info.key)) return info.page;
  return std::nullopt;
}

std::optional<features::Category> PageCategory(Page p) {
  const int c = Info(p).category;
  if (c == kNoCategory) return std::nullopt;
  return static_cast<Category>(c);
}

Page PageForCategory(features::Category c) {
  if (c == Category::Scripts) return Page::Scripts;
  for (const PageInfo& info : kPages)
    if (info.category == static_cast<int>(c)) return info.page;
  return Page::Home;
}

Page CurrentPage() { return g_menu.page; }

void NavigateTo(Page p, std::string_view featureId) {
  if (p >= Page::Count) p = Page::Home;
  if (p != g_menu.page) {
    g_menu.page = p;
    Config::Get().WriteString(kCfgPage, PageKey(p));
  }
  if (!featureId.empty()) {
    g_menu.focusFeature = std::string(featureId);
    g_menu.highlightFeature = g_menu.focusFeature;
    g_menu.highlightUntil = ImGui::GetCurrentContext() != nullptr ? ImGui::GetTime() + 1.6 : 1.6;
    g_menu.search[static_cast<size_t>(p)][0] = '\0';   // a filter could hide the target
  }
}

void RequestUnloadConfirmation() {
  g_menu.unloadRequested = true;
  if (!render::MenuOpen()) render::SetMenuOpen(true);
}

void DrawMainMenu() {
  const auto& p = th::Colors();
  const float s = UiScale();
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(980.0f * s, 640.0f * s), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f * s, 480.0f * s), ImVec2(FLT_MAX, FLT_MAX));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoScrollWithMouse;
  const bool visible = ImGui::Begin("Consigliere###cg.main", nullptr, flags);
  ImGui::PopStyleVar();
  if (visible) {
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    const float headerH = std::round(ImGui::GetFrameHeight() * 1.95f + 8.0f * s);
    DrawHeader(headerH);
    const float bannerH = DrawMpBanner(wp.y + headerH);
    const float top = wp.y + headerH + bannerH;
    const float bottom = wp.y + ws.y;
    const float sidebarW = std::round(196.0f * s);
    const float bodyH = std::max(bottom - top, 1.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(wp.x, top), ImVec2(wp.x + sidebarW, bottom), Col(Mix(p.bg, p.bgAlt, 0.55f)), ImGui::GetStyle().WindowRounding,
                      ImDrawFlags_RoundCornersBottomLeft);
    dl->AddLine(ImVec2(wp.x + sidebarW, top), ImVec2(wp.x + sidebarW, bottom), Col(p.border));

    std::array<int, static_cast<size_t>(Category::Count)> activeCounts{};
    for (const auto& f : features::Registry::Get().All()) {
      if (!f || f->GetKind() != Kind::Toggle || !f->Enabled()) continue;
      const auto c = static_cast<size_t>(f->GetCategory());
      if (c < activeCounts.size()) activeCounts[c]++;
    }

    ImGui::SetCursorScreenPos(ImVec2(wp.x, top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * s, 10.0f * s));
    ImGui::BeginChild("##cg.sidebar", ImVec2(sidebarW, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    DrawSidebar(activeCounts);
    ImGui::EndChild();

    if (g_menu.shownPage != g_menu.page) {
      g_menu.shownPage = g_menu.page;
      g_menu.pageFade = 0.0f;
      g_menu.scaleEdit = -1.0f;
      if (g_menu.page == Page::Settings) g_menu.profilesDirty = true;
    }
    g_menu.pageFade = Approach(g_menu.pageFade, 1.0f, 12.0f);

    ImGui::SetCursorScreenPos(ImVec2(wp.x + sidebarW + 1.0f, top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f * s, 18.0f * s));
    const std::string contentId = std::string("##cg.content.") + PageKey(g_menu.page);
    ImGui::BeginChild(contentId.c_str(), ImVec2(0.0f, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (0.35f + 0.65f * g_menu.pageFade));
    const Page page = g_menu.page;
    RunGuarded(PageName(page), [page] { DrawPage(page); });
    ImGui::PopStyleVar();
    ImGui::EndChild();

    DrawUnloadPopup();
  }
  ImGui::End();
}

}  // namespace cg::ui::detail
