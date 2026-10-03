// Math operators for ImVec2/ImVec4 must be enabled before the first include of imgui.h.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/internal.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "core/bootstrap.h"
#include "core/paths.h"
#include "core/util.h"
#include "features/feature.h"
#include "render/fonts.h"
#include "render/overlay.h"
#include "render/theme.h"
#include "ui/notify.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace cg::ui::detail {
namespace {

namespace th = render::theme;

enum class EntryKind : uint8_t { Command, Page, Tool, Toggle, Action, Panel };

enum class Command : uint8_t {
  ReloadBindings,
  ReloadScripts,
  DisableAll,
  SaveConfig,
  OpenDataFolder,
  OpenScriptsFolder,
  CloseMenu,
  ThemeNoir,
  ThemeMidnight,
  ThemeBordeaux,
  ThemeLight,
  Unload,
};

struct CommandInfo {
  Command cmd;
  const char* label;
  const char* detail;
  const char* icon;
};

constexpr CommandInfo kCommands[] = {
    {Command::ReloadBindings, "Reload bindings", "Re-read Chroma/bindings/*.json and resolve every symbol", CG_ICON_REFRESH},
    {Command::ReloadScripts, "Reload scripts", "Reload every Lua script", CG_ICON_REFRESH},
    {Command::DisableAll, "Disable all features", "Panic: switch everything off", CG_ICON_POWER},
    {Command::SaveConfig, "Save config", "Write the configuration to disk now", CG_ICON_SAVE},
    {Command::OpenDataFolder, "Open data folder", "Show the Chroma folder in Explorer", CG_ICON_FOLDER},
    {Command::OpenScriptsFolder, "Open scripts folder", "Show the scripts folder in Explorer", CG_ICON_FOLDER},
    {Command::CloseMenu, "Close menu", "Hide Chroma", CG_ICON_CLOSE},
    {Command::ThemeNoir, "Theme: Noir", "Charcoal and brass", CG_ICON_SETTINGS},
    {Command::ThemeMidnight, "Theme: Midnight", "Blue-black and steel", CG_ICON_SETTINGS},
    {Command::ThemeBordeaux, "Theme: Bordeaux", "Deep wine and gold", CG_ICON_SETTINGS},
    {Command::ThemeLight, "Theme: Light", "Parchment and ink", CG_ICON_SETTINGS},
    {Command::Unload, "Unload Chroma", "Restore the game and remove Chroma", CG_ICON_POWER},
};

struct Entry {
  EntryKind kind = EntryKind::Command;
  std::string key;      // feature id, tool id, page key or command index
  std::string label;
  std::string detail;
  const char* icon = "";
  std::string hotkey;
  bool on = false;
  bool available = true;
  int score = 0;
  int order = 0;
};

struct PaletteState {
  bool open = false;
  int openedFrame = -1;
  std::array<char, 128> query{};
  int selected = 0;
  bool scrollToSelected = false;
  std::vector<Entry> results;
};
PaletteState g_pal;

int KindRank(EntryKind k) {
  switch (k) {
    case EntryKind::Toggle:
    case EntryKind::Action:
    case EntryKind::Panel: return 0;
    case EntryKind::Page: return 1;
    case EntryKind::Tool: return 2;
    case EntryKind::Command: return 3;
  }
  return 4;
}

void Collect(std::vector<Entry>& out) {
  const ShellSettings& st = Settings();
  int order = 0;
  for (const auto& f : features::Registry::Get().All()) {
    if (!f) continue;
    Entry e;
    switch (f->GetKind()) {
      case features::Kind::Toggle: e.kind = EntryKind::Toggle; break;
      case features::Kind::Action: e.kind = EntryKind::Action; break;
      case features::Kind::Panel: e.kind = EntryKind::Panel; break;
    }
    e.key = f->Id();
    e.label = f->Name();
    e.detail = features::CategoryName(f->GetCategory());
    if (!f->Description().empty()) e.detail += " - " + f->Description();
    e.icon = features::CategoryIcon(f->GetCategory());
    if (f->Key().Valid()) e.hotkey = f->Key().ToString();
    e.on = f->Enabled();
    e.available = f->Available();
    e.order = order++;
    out.push_back(std::move(e));
  }
  for (int i = 0; i < static_cast<int>(Page::Count); ++i) {
    const auto pg = static_cast<Page>(i);
    Entry e;
    e.kind = EntryKind::Page;
    e.key = PageKey(pg);
    e.label = PageName(pg);
    e.detail = "Go to page";
    e.icon = PageIcon(pg);
    e.order = order++;
    out.push_back(std::move(e));
  }
  for (const auto& w : Windows()) {
    Entry e;
    e.kind = EntryKind::Tool;
    e.key = w->Id();
    e.label = w->Title() != nullptr ? w->Title() : w->Id();
    e.detail = std::string(w->Group() != nullptr ? w->Group() : "Tools") + (w->open ? " - open" : "");
    e.icon = w->Icon();
    e.order = order++;
    out.push_back(std::move(e));
  }
  for (const CommandInfo& c : kCommands) {
    Entry e;
    e.kind = EntryKind::Command;
    e.key = std::to_string(static_cast<int>(c.cmd));
    e.label = c.label;
    e.detail = c.detail;
    e.icon = c.icon;
    if (c.cmd == Command::DisableAll && st.panicKey.Valid()) e.hotkey = st.panicKey.ToString();
    if (c.cmd == Command::Unload && st.unloadKey.Valid()) e.hotkey = st.unloadKey.ToString();
    if (c.cmd == Command::CloseMenu && st.menuKey.Valid()) e.hotkey = st.menuKey.ToString();
    e.order = order++;
    out.push_back(std::move(e));
  }
}

void Rebuild() {
  std::vector<Entry> all;
  all.reserve(256);
  Collect(all);
  const std::string q = util::Trim(g_pal.query.data());
  g_pal.results.clear();
  if (q.empty()) {
    for (Entry& e : all) g_pal.results.push_back(std::move(e));
    std::stable_sort(g_pal.results.begin(), g_pal.results.end(), [](const Entry& a, const Entry& b) {
      if (KindRank(a.kind) != KindRank(b.kind)) return KindRank(a.kind) < KindRank(b.kind);
      return a.order < b.order;
    });
    return;
  }
  for (Entry& e : all) {
    // Matches on the label always outrank matches found only in the secondary text.
    const int onLabel = util::FuzzyScore(q, e.label);
    int score = -1;
    if (onLabel >= 0) score = 100000 + onLabel;
    else if (const int onDetail = util::FuzzyScore(q, e.detail); onDetail >= 0) score = onDetail;
    if (score < 0) continue;
    e.score = score;
    g_pal.results.push_back(std::move(e));
  }
  std::stable_sort(g_pal.results.begin(), g_pal.results.end(), [](const Entry& a, const Entry& b) {
    if (a.score != b.score) return a.score > b.score;
    if (KindRank(a.kind) != KindRank(b.kind)) return KindRank(a.kind) < KindRank(b.kind);
    return a.label < b.label;
  });
}

void RunCommand(Command c) {
  switch (c) {
    case Command::ReloadBindings: CmdReloadBindings(); break;
    case Command::ReloadScripts: CmdReloadScripts(); break;
    case Command::DisableAll: CmdDisableAll(); break;
    case Command::SaveConfig: CmdSaveConfig(); break;
    case Command::OpenDataFolder: CmdOpenFolder(paths::DataDir()); break;
    case Command::OpenScriptsFolder: CmdOpenFolder(paths::Data(L"scripts")); break;
    case Command::CloseMenu: render::SetMenuOpen(false); break;
    case Command::ThemeNoir: RequestAppearance(static_cast<int>(th::Preset::Noir), UiScale()); break;
    case Command::ThemeMidnight: RequestAppearance(static_cast<int>(th::Preset::Midnight), UiScale()); break;
    case Command::ThemeBordeaux: RequestAppearance(static_cast<int>(th::Preset::Bordeaux), UiScale()); break;
    case Command::ThemeLight: RequestAppearance(static_cast<int>(th::Preset::Light), UiScale()); break;
    case Command::Unload: RequestUnloadConfirmation(); break;
  }
}

void Execute(const Entry& e) {
  switch (e.kind) {
    case EntryKind::Command: {
      const auto idx = util::ParseInt(e.key);
      if (idx && *idx >= 0 && *idx < static_cast<int64_t>(std::size(kCommands))) RunCommand(static_cast<Command>(*idx));
      break;
    }
    case EntryKind::Page:
      if (const auto pg = PageFromKey(e.key)) NavigateTo(*pg);
      break;
    case EntryKind::Tool: OpenWindow(e.key); break;
    case EntryKind::Toggle:
    case EntryKind::Action: {
      const features::Feature* f = features::Registry::Get().Find(e.key);
      if (f == nullptr) break;
      if (!f->Available()) {
        const std::string reason = f->UnavailableReason();
        notify::Push(notify::Kind::Warning, f->Name() + " is unavailable", reason.empty() ? "Missing bindings" : reason);
        break;
      }
      if (e.kind == EntryKind::Toggle) QueueFeatureSet(e.key, !f->Enabled());
      else QueueFeatureTrigger(e.key);
      break;
    }
    case EntryKind::Panel:
      if (const features::Feature* f = features::Registry::Get().Find(e.key)) NavigateTo(PageForCategory(f->GetCategory()), e.key);
      break;
  }
  ClosePalette();
}

int HistoryCallback(ImGuiInputTextCallbackData* data) {
  if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory) return 0;
  const int n = static_cast<int>(g_pal.results.size());
  if (n == 0) return 0;
  if (data->EventKey == ImGuiKey_UpArrow) g_pal.selected = (g_pal.selected - 1 + n) % n;
  else if (data->EventKey == ImGuiKey_DownArrow) g_pal.selected = (g_pal.selected + 1) % n;
  g_pal.scrollToSelected = true;
  return 0;
}

struct BadgeStyle {
  const char* text;
  ImVec4 color;
};

BadgeStyle BadgeFor(const Entry& e) {
  const auto& p = th::Colors();
  switch (e.kind) {
    case EntryKind::Toggle: return {e.on ? "ON" : "OFF", e.on ? p.success : p.textFaint};
    case EntryKind::Action: return {"ACTION", p.accent};
    case EntryKind::Panel: return {"PANEL", p.info};
    case EntryKind::Page: return {"PAGE", p.textDim};
    case EntryKind::Tool: return {"TOOL", p.accent};
    case EntryKind::Command: return {"COMMAND", p.info};
  }
  return {"", p.textDim};
}

void DrawRow(int index, float rowH) {
  const auto& p = th::Colors();
  const float s = UiScale();
  Entry& e = g_pal.results[static_cast<size_t>(index)];
  ImGui::PushID(index);
  const bool selected = index == g_pal.selected;
  const bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None, ImVec2(0.0f, rowH));
  if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f)) g_pal.selected = index;
  if (selected && g_pal.scrollToSelected) {
    ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
    g_pal.scrollToSelected = false;
  }
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const float ty = min.y + (rowH - fs) * 0.5f;
  float x = min.x + 8.0f * s;

  if (selected) dl->AddRectFilled(ImVec2(min.x, min.y + rowH * 0.2f), ImVec2(min.x + 3.0f * s, max.y - rowH * 0.2f), Col(p.accent), 2.0f * s);
  if (HasIcons() && e.icon != nullptr && *e.icon != '\0') {
    dl->AddText(render::GetFonts().body, fs, ImVec2(x, ty), Col(selected ? p.accent : p.textDim), e.icon);
  }
  x += HasIcons() ? fs * 1.8f : 0.0f;

  // Right side: badge, then hotkey.
  const BadgeStyle badge = BadgeFor(e);
  ImFont* bold = render::GetFonts().bold != nullptr ? render::GetFonts().bold : ImGui::GetFont();
  const float bfs = fs * 0.72f;
  const ImVec2 bts = bold->CalcTextSizeA(bfs, FLT_MAX, 0.0f, badge.text);
  const float bh = fs * 1.05f;
  const float bw = bts.x + bh * 0.9f;
  const ImVec2 b0(max.x - 8.0f * s - bw, min.y + (rowH - bh) * 0.5f);
  dl->AddRectFilled(b0, b0 + ImVec2(bw, bh), Col(WithAlpha(badge.color, 0.18f)), bh * 0.5f);
  dl->AddText(bold, bfs, b0 + ImVec2((bw - bts.x) * 0.5f, (bh - bts.y) * 0.5f), Col(badge.color), badge.text);
  float rightEdge = b0.x - 10.0f * s;
  if (!e.hotkey.empty()) {
    ImFont* mono = render::GetFonts().mono != nullptr ? render::GetFonts().mono : ImGui::GetFont();
    const float mfs = fs * 0.85f;
    const ImVec2 hs = mono->CalcTextSizeA(mfs, FLT_MAX, 0.0f, e.hotkey.c_str());
    const ImVec2 h0(rightEdge - hs.x - 10.0f * s, min.y + (rowH - hs.y - 4.0f * s) * 0.5f);
    dl->AddRect(h0, h0 + ImVec2(hs.x + 10.0f * s, hs.y + 4.0f * s), Col(p.border), 4.0f * s);
    dl->AddText(mono, mfs, h0 + ImVec2(5.0f * s, 2.0f * s), Col(p.textDim), e.hotkey.c_str());
    rightEdge = h0.x - 10.0f * s;
  }

  // Label (bold) then the detail, clipped before the right-hand decorations.
  const ImVec4 clip(min.x, min.y, std::max(x, rightEdge), max.y);
  const ImU32 labelCol = Col(e.available ? p.text : p.textFaint);
  dl->AddText(bold, fs, ImVec2(x, ty), labelCol, e.label.c_str(), nullptr, 0.0f, &clip);
  const float labelW = bold->CalcTextSizeA(fs, FLT_MAX, 0.0f, e.label.c_str()).x;
  if (!e.detail.empty() && x + labelW + 12.0f * s < rightEdge) {
    dl->AddText(ImGui::GetFont(), fs * 0.9f, ImVec2(x + labelW + 12.0f * s, ty + fs * 0.06f), Col(p.textFaint), e.detail.c_str(), nullptr,
                0.0f, &clip);
  }
  if (!e.available) ImGui::SetItemTooltip("Unavailable - see the Bindings window");
  ImGui::PopID();
  if (clicked) {
    const Entry chosen = e;   // Execute may rebuild the list
    Execute(chosen);
  }
}

void DrawShadow(const ImVec2& min, const ImVec2& max, float s) {
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const auto& p = th::Colors();
  for (int i = 1; i <= 6; ++i) {
    const float grow = static_cast<float>(i) * 3.0f * s;
    dl->AddRect(min - ImVec2(grow, grow) + ImVec2(0, 4.0f * s), max + ImVec2(grow, grow) + ImVec2(0, 4.0f * s), th::U32(p.bgAlt, 0.10f),
                (10.0f + static_cast<float>(i) * 3.0f) * s, 3.0f * s);
  }
}

}  // namespace

void OpenPalette() {
  g_pal.open = true;
  g_pal.openedFrame = ImGui::GetCurrentContext() != nullptr ? ImGui::GetFrameCount() : 0;
  g_pal.query.fill('\0');
  g_pal.selected = 0;
  g_pal.scrollToSelected = true;
}

void ClosePalette() {
  g_pal.open = false;
  g_pal.results.clear();
}

bool PaletteOpen() { return g_pal.open; }

void UpdatePalette() {
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P)) {
    if (g_pal.open) ClosePalette();
    else OpenPalette();
  }
  if (!g_pal.open) return;

  const auto& p = th::Colors();
  const float s = UiScale();
  const int frame = ImGui::GetFrameCount();
  const bool firstFrame = frame - g_pal.openedFrame <= 1;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float width = std::min(640.0f * s, vp->WorkSize.x - 40.0f * s);

  Rebuild();
  const int n = static_cast<int>(g_pal.results.size());
  g_pal.selected = n == 0 ? 0 : std::clamp(g_pal.selected, 0, n - 1);

  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.16f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
  if (firstFrame) ImGui::SetNextWindowFocus();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * s, 10.0f * s));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * s);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, Mix(p.bg, p.panel, 0.5f));
  ImGui::PushStyleColor(ImGuiCol_Border, p.accentDim);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoScrollbar;
  bool executeSelected = false;
  bool close = false;
  if (ImGui::Begin("##cg.palette", nullptr, flags)) {
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    DrawShadow(ImGui::GetWindowPos(), ImGui::GetWindowPos() + ImGui::GetWindowSize(), s);

    // Query field with a search glyph inside the frame.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float iconW = HasIcons() ? ImGui::GetFontSize() * 1.6f : 0.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x + iconW, style.FramePadding.y * 1.5f));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (firstFrame) ImGui::SetKeyboardFocusHere();
    const std::array<char, 128> before = g_pal.query;
    const bool enter = ImGui::InputTextWithHint("##query", "Type a feature, page, tool or command...", g_pal.query.data(),
                                                g_pal.query.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
                                                HistoryCallback);
    ImGui::PopStyleVar();
    if (HasIcons()) {
      const ImVec2 qmin = ImGui::GetItemRectMin();
      const float fs = ImGui::GetFontSize();
      ImGui::GetWindowDrawList()->AddText(render::GetFonts().body, fs,
                                          ImVec2(qmin.x + style.FramePadding.x, qmin.y + (ImGui::GetItemRectSize().y - fs) * 0.5f),
                                          Col(p.accent), CG_ICON_SEARCH);
    }
    if (before != g_pal.query) {
      g_pal.selected = 0;
      g_pal.scrollToSelected = true;
    }
    if (enter) executeSelected = true;

    ImGui::Spacing();
    const float rowH = ImGui::GetFrameHeight() * 1.1f;
    const int visibleRows = std::clamp(n, 1, 9);
    const float listH = rowH * static_cast<float>(visibleRows) + style.ItemSpacing.y * static_cast<float>(visibleRows);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 2.0f * s));
    if (ImGui::BeginChild("##results", ImVec2(0.0f, listH), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav)) {
      if (n == 0) {
        TextColored(p.textFaint, "No matches.");
      } else {
        ImGuiListClipper clipper;
        clipper.Begin(n, rowH + 2.0f * s);
        if (g_pal.scrollToSelected) clipper.IncludeItemByIndex(g_pal.selected);
        while (clipper.Step()) {
          for (int i = clipper.DisplayStart; i < clipper.DisplayEnd && i < static_cast<int>(g_pal.results.size()); ++i) {
            DrawRow(i, rowH);
            if (!g_pal.open) break;   // a row was executed
          }
          if (!g_pal.open) break;
        }
        clipper.End();
      }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (g_pal.open) {
      ImGui::PushStyleColor(ImGuiCol_Text, p.textFaint);
      ImGui::TextUnformatted("Up/Down to choose   Enter to run   Esc to close");
      ImGui::PopStyleColor();
      char count[32];
      std::snprintf(count, sizeof(count), "%d results", n);
      ImGui::SameLine();
      const float countW = ImGui::CalcTextSize(count).x;
      const float room = ImGui::GetContentRegionAvail().x;
      if (room > countW) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room - countW);
      TextColored(p.textFaint, count);

      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
      if (!firstFrame && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) close = true;
    }
  }
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(2);

  if (g_pal.open && executeSelected && !g_pal.results.empty()) {
    const Entry chosen = g_pal.results[static_cast<size_t>(std::clamp(g_pal.selected, 0, static_cast<int>(g_pal.results.size()) - 1))];
    Execute(chosen);
  } else if (close) {
    ClosePalette();
  }
}

}  // namespace cg::ui::detail
