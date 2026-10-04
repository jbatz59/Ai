// Player features: god mode, invisibility, noclip, outfits. All script-backed (game Lua VM).
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "core/hotkeys.h"
#include "core/util.h"
#include "features/feature.h"
#include "features/game_lua.h"
#include "game/script_vm.h"
#include "render/overlay.h"
#include "ui/notify.h"
#include "ui/widgets.h"

namespace cg::features {
namespace {

namespace nt = cg::ui::notify;

// At most one chunk in flight per owner; a lost callback frees the slot after 5 s.
class Runner {
 public:
  bool Busy() const { return *start_ && util::NowMs() - *start_ < 5000; }
  bool Run(std::string code, std::string what, game::vm::Callback done = {}, bool toast = true) {
    if (Busy()) return false;
    *start_ = util::NowMs();
    auto s = start_;
    RunFeatureChunk(std::move(code), std::move(what), [s, done = std::move(done)](const game::vm::Result& r) {
      *s = 0;
      if (done) done(r);
    }, toast);
    return true;
  }

 private:
  std::shared_ptr<uint64_t> start_ = std::make_shared<uint64_t>(0);
};

// Script toggle: `on` at enable (re-applied every `period` s when > 0), `off` at disable.
class ScriptToggle : public Feature {
 public:
  ScriptToggle(std::string id, std::string name, Category c, std::string desc, std::string on, std::string off, float period)
      : Feature(std::move(id), std::move(name), c, Kind::Toggle, std::move(desc)), on_(std::move(on)), off_(std::move(off)), period_(period) {}
  std::vector<std::string> Requires() const override { return {"vm"}; }
  void OnEnable() override {
    timer_ = 0;
    runner_.Run(on_, Name());
  }
  void OnDisable() override {
    if (!off_.empty()) RunFeatureChunk(off_, Name());
  }
  void Tick(float dt) override {
    if (period_ <= 0 || (timer_ += dt) < period_) return;
    timer_ = 0;
    runner_.Run(on_, Name(), {}, false);
  }

 protected:
  Runner runner_;

 private:
  std::string on_, off_;
  float period_, timer_ = 0;
};

// One-shot script action.
class ScriptAction : public Feature {
 public:
  ScriptAction(std::string id, std::string name, Category c, std::string desc, std::string code)
      : Feature(std::move(id), std::move(name), c, Kind::Action, std::move(desc)), code_(std::move(code)) {}
  std::vector<std::string> Requires() const override { return {"vm"}; }
  void Activate() override { runner_.Run(code_, Name()); }

 protected:
  Runner runner_;
  std::string code_;
};

[[maybe_unused]] void SetBuf(char* buf, size_t n, const std::string& s) { std::snprintf(buf, n, "%s", s.c_str()); }

// Favourite chips under a free-text field. Returns true when one was picked (copied into buf).
[[maybe_unused]] bool Favourites(char* buf, size_t n, std::vector<std::string>& favs) {
  bool picked = false;
  ImGui::PushID("favs");
  if (ImGui::SmallButton("+ Favourite") && buf[0] && std::find(favs.begin(), favs.end(), std::string(buf)) == favs.end())
    favs.emplace_back(buf);
  int remove = -1;
  for (size_t i = 0; i < favs.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(favs[i].c_str()).x + 40) ImGui::NewLine();
    if (ImGui::SmallButton(favs[i].c_str())) {
      SetBuf(buf, n, favs[i]);
      picked = true;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) remove = static_cast<int>(i);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click: use  |  Right-click: remove");
    ImGui::PopID();
  }
  if (remove >= 0) favs.erase(favs.begin() + remove);
  ImGui::PopID();
  return picked;
}

[[maybe_unused]] std::vector<std::string> StrList(const nlohmann::json& j, const char* key) {
  std::vector<std::string> out;
  if (j.contains(key) && j[key].is_array())
    for (const auto& s : j[key])
      if (s.is_string()) out.push_back(s.get<std::string>());
  return out;
}

constexpr uint16_t kVkShift = 0x10, kVkCtrl = 0x11, kVkSpace = 0x20, kVkA = 0x41, kVkD = 0x44, kVkS = 0x53, kVkW = 0x57;

constexpr const char* kGodOn = R"(local p = CG.need(CG.player())
CG.must(CG.call(p, "SetDemigod", true))
CG.call(p, "EnableInjury", false)
CG.try(function() p.invulnerability = true end))";
constexpr const char* kGodOff = R"(local p = CG.need(CG.player())
CG.call(p, "SetDemigod", false)
CG.call(p, "EnableInjury", true)
CG.try(function() p.invulnerability = false end))";

std::string PhysChunk(const char* state) {
  return std::string("local o = CG.need(CG.target())\nlocal s = CG.get(\"enums\", \"PhysicsState\", ") + LuaQuote(state) +
         ")\nif s == nil then error(\"enums.PhysicsState not available on this build\", 0) end\nCG.must(CG.call(o, \"SetPhysState\", s))";
}

class Noclip : public Feature {
 public:
  Noclip() : Feature("player.noclip", "Noclip", Category::Player, Kind::Toggle,
                     "Fly through walls: W/A/S/D move, Space/Ctrl up/down, Shift fast. Works on foot and in vehicles.") {}
  std::vector<std::string> Requires() const override { return {"vm"}; }
  void OnEnable() override { RunFeatureChunk(PhysChunk("DISABLED"), Name()); }
  void OnDisable() override { RunFeatureChunk(PhysChunk("DYNAMIC"), Name()); }
  void Tick(float dt) override {
    pending_ += dt;
    if (render::MenuOpen() || runner_.Busy()) return;
    const float f = (hotkeys::IsDown(kVkW) ? 1.f : 0.f) - (hotkeys::IsDown(kVkS) ? 1.f : 0.f);
    const float r = (hotkeys::IsDown(kVkD) ? 1.f : 0.f) - (hotkeys::IsDown(kVkA) ? 1.f : 0.f);
    const float u = (hotkeys::IsDown(kVkSpace) ? 1.f : 0.f) - (hotkeys::IsDown(kVkCtrl) ? 1.f : 0.f);
    const float step = std::min(pending_, 0.25f) * (hotkeys::IsDown(kVkShift) ? fast_ : speed_);
    pending_ = 0;
    if (f == 0 && r == 0 && u == 0) return;
    std::string code = "local f, r, u = " + LuaNumber(f * step) + ", " + LuaNumber(r * step) + ", " + LuaNumber(u * step) + R"(
local o = CG.need(CG.target())
local ok, pos = CG.call(o, "GetPos")
if not ok or not pos then error(pos or "no position", 0) end
local fx, fy = CG.forward(o)
CG.must(CG.call(o, "SetPos", CG.need(CG.vec(pos.x + fx * f + fy * r, pos.y + fy * f - fx * r, pos.z + u)))))";
    runner_.Run(std::move(code), Name(), {}, false);
  }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    ImGui::SliderFloat("Speed (m/s)", &speed_, 1.f, 50.f, "%.0f");
    ImGui::SliderFloat("Shift speed (m/s)", &fast_, 5.f, 300.f, "%.0f");
  }
  void SaveExtra(nlohmann::json& j) const override { j = {{"speed", speed_}, {"fast", fast_}}; }
  void LoadExtra(const nlohmann::json& j) override {
    speed_ = j.value("speed", speed_);
    fast_ = j.value("fast", fast_);
  }

 private:
  Runner runner_;
  float speed_ = 8.f, fast_ = 40.f, pending_ = 0;
};

class Outfit : public Feature {
 public:
  Outfit() : Feature("player.outfit", "Outfit", Category::Player, Kind::Panel,
                     "Switch the player's spawn profile (outfit) by name. Names depend on the game build.") {}
  std::vector<std::string> Requires() const override { return {"vm"}; }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    ImGui::SetNextItemWidth(-90);
    const bool enter = ImGui::InputTextWithHint("##outfit", "spawn profile name", name_, sizeof(name_), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Apply") || enter) && name_[0]) Apply();
    if (Favourites(name_, sizeof(name_), favs_)) Apply();
  }
  void SaveExtra(nlohmann::json& j) const override { j = {{"name", name_}, {"favourites", favs_}}; }
  void LoadExtra(const nlohmann::json& j) override {
    SetBuf(name_, sizeof(name_), j.value("name", std::string()));
    favs_ = StrList(j, "favourites");
  }

 private:
  void Apply() {
    runner_.Run("local p = CG.need(CG.player())\nCG.must(CG.call(p, \"SwitchDefaultPlayerSpawnProfile\", " + LuaQuote(name_) + "))", Name(),
                [n = std::string(name_)](const game::vm::Result& r) {
                  if (r.ok) nt::Push(nt::Kind::Success, "Outfit", n);
                });
  }
  Runner runner_;
  char name_[128] = {};
  std::vector<std::string> favs_;
};

}  // namespace

void RegisterPlayerFeatures(Registry& r) {
  r.Add(std::make_unique<ScriptToggle>("player.god", "God mode", Category::Player,
                                       "Demigod + no injuries + invulnerability, re-applied every 2 s.", kGodOn, kGodOff, 2.f));
  r.Add(std::make_unique<ScriptToggle>("player.invisible", "Invisible", Category::Player, "Hides the player model.",
                                       "local p = CG.need(CG.player())\nCG.must(CG.call(p, \"ShowModel\", false))",
                                       "local p = CG.need(CG.player())\nCG.must(CG.call(p, \"ShowModel\", true))", 5.f));
  r.Add(std::make_unique<Noclip>());
  r.Add(std::make_unique<Outfit>());
}

}  // namespace cg::features
