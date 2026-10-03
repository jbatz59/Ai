// Bindings status, log console and the game Lua console.
#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/log.h"
#include "core/tasks.h"
#include "core/util.h"
#include "game/bindings.h"
#include "game/game.h"
#include "game/script_vm.h"
#include "render/theme.h"
#include "ui/tools/tools_common.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace cg::ui {
namespace {

ImVec4 StatusColor(game::SymbolStatus s) {
  const auto& c = render::theme::Colors();
  switch (s) {
    case game::SymbolStatus::Resolved: return c.success;
    case game::SymbolStatus::Overridden: return c.info;
    case game::SymbolStatus::Failed: return c.danger;
    default: return c.textFaint;
  }
}

// ---------------------------------------------------------------------------------------------
class BindingsWindow final : public Window {
 public:
  const char* Id() const override { return "tools.bindings"; }
  const char* Title() const override { return "Game Bindings"; }
  const char* Group() const override { return "Game Bindings"; }
  float DefaultWidth() const override { return 860; }

  void Draw() override {
    auto& b = game::Bindings::Get();
    const auto& c = render::theme::Colors();
    if (b.Resolving()) {
      ImGui::ProgressBar(b.Progress(), ImVec2(220, 0), "Resolving...");
    } else if (ImGui::Button("Reload bindings")) {
      tasks::RunAsync([] { game::Bindings::Get().Reload(); });
    }
    ImGui::SameLine();
    ImGui::TextColored(c.textDim, "Build: %s | Script VM: %s", b.GameBuild().empty() ? "?" : b.GameBuild().c_str(),
                       game::vm::StatusText());
    for (const auto& e : b.LoadErrors()) ImGui::TextColored(c.danger, "%s", e.c_str());

    SearchBox("##bindfilter", filter_, sizeof(filter_), "Filter symbols...");
    if (ImGui::BeginTabBar("##bindtabs")) {
      if (ImGui::BeginTabItem("Symbols")) {
        DrawSymbols();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("What to bind")) {
        DrawCanonical();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }

 private:
  void DrawSymbols() {
    const uint64_t gen = game::Bindings::Get().Generation();
    if (gen != gen_ || util::NowMs() - refreshed_ > 1000) {
      symbols_ = game::Bindings::Get().List();
      gen_ = gen;
      refreshed_ = util::NowMs();
    }
    const auto& c = render::theme::Colors();
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##symbols", 5, flags, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Symbol", 0, 2.0f);
    ImGui::TableSetupColumn("Kind", 0, 0.8f);
    ImGui::TableSetupColumn("Value", 0, 1.6f);
    ImGui::TableSetupColumn("Status", 0, 0.9f);
    ImGui::TableSetupColumn("Error / notes", 0, 3.0f);
    ImGui::TableHeadersRow();
    for (const auto& s : symbols_) {
      if (filter_[0] && !util::IContains(s.name, filter_)) continue;
      ImGui::PushID(s.name.c_str());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(s.name.c_str());
      if (!s.verified) {
        ImGui::SameLine();
        ImGui::TextColored(c.warning, "unverified");
      }
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(game::SymbolKindName(s.kind));
      ImGui::TableNextColumn();
      std::string value;
      if (s.status == game::SymbolStatus::Resolved || s.status == game::SymbolStatus::Overridden) {
        if (s.kind == game::SymbolKind::Pointer) {
          const auto a = game::Bindings::Get().Addr(s.name);
          value = a ? util::Hex(*a) : "(null now)";
        } else {
          value = util::Hex(s.staticValue);
        }
      }
      ImGui::TextUnformatted(value.c_str());
      if (!value.empty() && ImGui::IsItemClicked()) {
        ImGui::SetClipboardText(value.c_str());
      }
      ImGui::TableNextColumn();
      ImGui::TextColored(StatusColor(s.status), "%s", game::SymbolStatusName(s.status));
      ImGui::TableNextColumn();
      ImGui::TextWrapped("%s", s.error.empty() ? s.notes.c_str() : s.error.c_str());
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  void DrawCanonical() {
    const auto& c = render::theme::Colors();
    ImGui::TextWrapped("Symbols Chroma knows how to use. Missing ones only disable the features that need them. "
                       "See docs/BINDINGS.md for how to find each one on your build.");
    if (!ImGui::BeginTable("##canon", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
      return;
    ImGui::TableSetupColumn("Symbol", 0, 1.5f);
    ImGui::TableSetupColumn("Kind", 0, 0.7f);
    ImGui::TableSetupColumn("Type", 0, 1.5f);
    ImGui::TableSetupColumn("Purpose", 0, 3.0f);
    ImGui::TableHeadersRow();
    for (const auto& s : game::CanonicalSymbols()) {
      if (filter_[0] && !util::IContains(s.name, filter_)) continue;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      const bool has = game::Bindings::Get().Has(s.name);
      ImGui::TextColored(has ? c.success : c.textDim, "%s %s", has ? "+" : "-", s.name);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(s.kind);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(s.type);
      ImGui::TableNextColumn();
      ImGui::TextWrapped("%s", s.purpose);
    }
    ImGui::EndTable();
  }

  char filter_[128] = {};
  std::vector<game::SymbolInfo> symbols_;
  uint64_t gen_ = ~0ull;
  uint64_t refreshed_ = 0;
};

// ---------------------------------------------------------------------------------------------
class LogWindow final : public Window {
 public:
  const char* Id() const override { return "tools.log"; }
  const char* Title() const override { return "Log"; }
  const char* Group() const override { return "Consoles"; }

  void Draw() override {
    const auto& c = render::theme::Colors();
    const uint64_t gen = log::Generation();
    if (gen != gen_) {
      entries_ = log::Copy();
      gen_ = gen;
    }
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("##lvl", &minLevel_, "Trace\0Debug\0Info\0Warn\0Error\0");
    ImGui::SameLine();
    SearchBox("##logfilter", filter_, sizeof(filter_), "Filter...");
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &autoScroll_);
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
      std::string all;
      for (const auto& e : entries_) all += "[" + e.channel + "] " + e.text + "\n";
      ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) log::Clear();

    std::vector<const log::Entry*> visible;
    visible.reserve(entries_.size());
    for (const auto& e : entries_) {
      if (static_cast<int>(e.level) < minLevel_) continue;
      if (filter_[0] && !util::IContains(e.text, filter_) && !util::IContains(e.channel, filter_)) continue;
      visible.push_back(&e);
    }
    if (ImGui::BeginChild("##logscroll", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
      ImGuiListClipper clip;
      clip.Begin(static_cast<int>(visible.size()));
      while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
          const log::Entry& e = *visible[static_cast<size_t>(i)];
          const ImVec4 col = e.level >= log::Level::Error ? c.danger : e.level == log::Level::Warn ? c.warning
                             : e.level <= log::Level::Debug ? c.textFaint : c.text;
          ImGui::TextColored(col, "[%s] %s", e.channel.c_str(), e.text.c_str());
        }
      }
      if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
  }

 private:
  std::vector<log::Entry> entries_;
  uint64_t gen_ = ~0ull;
  int minLevel_ = 2;
  bool autoScroll_ = true;
  char filter_[128] = {};
};

// ---------------------------------------------------------------------------------------------
// Results arrive through vm callbacks on the render thread; the shared state outlives the window.
struct LuaConsoleState {
  std::deque<std::pair<bool, std::string>> lines;   // (isError, text)
  void Add(bool err, std::string t) {
    lines.emplace_back(err, std::move(t));
    while (lines.size() > 2000) lines.pop_front();
  }
};

class GameLuaWindow final : public Window {
 public:
  const char* Id() const override { return "tools.gamelua"; }
  const char* Title() const override { return "Game Lua Console"; }
  const char* Group() const override { return "Consoles"; }

  void Draw() override {
    const auto& c = render::theme::Colors();
    for (auto& line : game::vm::TakePrintedLines()) state_->Add(false, "print: " + line);
    ImGui::TextColored(game::vm::Ready() ? c.success : c.warning, "Script VM: %s", game::vm::StatusText());
    if (!game::vm::Ready()) {
      const auto missing = game::vm::MissingBindings();
      if (!missing.empty()) {
        std::string m = "Missing bindings:";
        for (const auto& s : missing) m += " " + s;
        ImGui::TextWrapped("%s", m.c_str());
      }
    }
    const float inputH = ImGui::GetTextLineHeightWithSpacing() * 4;
    if (ImGui::BeginChild("##out", ImVec2(0, -inputH - ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders)) {
      for (const auto& [err, text] : state_->lines) ImGui::TextColored(err ? c.danger : c.text, "%s", text.c_str());
      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::InputTextMultiline("##code", code_, sizeof(code_), ImVec2(-90, inputH));
    ImGui::SameLine();
    const bool run = ImGui::Button("Run", ImVec2(80, inputH)) ||
                     (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter) && ImGui::GetIO().KeyCtrl);
    if (run && code_[0]) {
      state_->Add(false, std::string("> ") + code_);
      std::weak_ptr<LuaConsoleState> weak = state_;
      game::vm::Run(
          code_,
          [weak](const game::vm::Result& r) {
            auto s = weak.lock();
            if (!s) return;
            if (!r.ok) {
              s->Add(true, r.error);
              return;
            }
            std::string out;
            for (const auto& v : r.values) out += (out.empty() ? "" : ", ") + v;
            s->Add(false, out.empty() ? "ok (" + std::to_string(r.ms) + " ms)" : out);
          },
          "=console", true);
    }
    ImGui::TextColored(c.textFaint, "Runs inside the game's own script VM. Example: return game.game:GetActivePlayer():GetPos().x");
  }

 private:
  std::shared_ptr<LuaConsoleState> state_ = std::make_shared<LuaConsoleState>();
  char code_[4096] = {};
};

}  // namespace

std::unique_ptr<Window> MakeBindingsWindow() { return std::make_unique<BindingsWindow>(); }
std::unique_ptr<Window> MakeLogConsoleWindow() { return std::make_unique<LogWindow>(); }
std::unique_ptr<Window> MakeGameLuaConsoleWindow() { return std::make_unique<GameLuaWindow>(); }

}  // namespace cg::ui
