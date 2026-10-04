// Weapon features: unlimited ammo, refill, grenades, give weapon by name. Script-backed.
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "core/util.h"
#include "features/catalog.h"
#include "features/feature.h"
#include "features/game_lua.h"
#include "game/script_vm.h"
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

class Grenades : public ScriptAction {
 public:
  Grenades() : ScriptAction("weapons.grenades", "Add grenades", Category::Weapons, "Adds grenades to the inventory.", {}) {}
  void Activate() override {
    runner_.Run("local p = CG.need(CG.player())\nCG.must(CG.call(p, \"InventoryAddGrenades\", " + LuaNumber(count_) + "))", Name(),
                [n = count_](const game::vm::Result& r) {
                  if (r.ok) nt::Push(nt::Kind::Success, "Grenades", "+" + std::to_string(n));
                });
  }
  bool HasSettings() const override { return true; }
  void DrawSettings() override { ImGui::SliderInt("Count", &count_, 1, 20); }
  void SaveExtra(nlohmann::json& j) const override { j = {{"count", count_}}; }
  void LoadExtra(const nlohmann::json& j) override { count_ = std::clamp(j.value("count", count_), 1, 20); }

 private:
  int count_ = 5;
};

class GiveWeapon : public Feature {
 public:
  GiveWeapon() : Feature("weapons.give", "Give weapon", Category::Weapons, Kind::Panel,
                         "Pick any weapon from the list: click to select, double-click (or Give) to equip it.") {}
  std::vector<std::string> Requires() const override { return {"vm"}; }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    const bool now = DrawCatalog("##weapons", kWeapons, name_, sizeof(name_), filter_, sizeof(filter_), 7.0f);
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("Ammo", &ammo_, 0, 999);
    const std::string label = name_[0] ? "Give " + Display(name_) : std::string("Give");
    if (!name_[0]) ImGui::BeginDisabled();
    if (ImGui::Button(label.c_str()) || (now && name_[0])) Give();
    if (!name_[0]) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Give all weapons")) GiveAll();
    if (Favourites(name_, sizeof(name_), favs_)) Give();
    if (ImGui::TreeNode("Custom weapon name")) {
      ImGui::SetNextItemWidth(-90);
      const bool enter = ImGui::InputTextWithHint("##weapon", "weapon name", name_, sizeof(name_), ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      if ((ImGui::Button("Give##custom") || enter) && name_[0]) Give();
      if (ImGui::Button("Remember current")) Remember();
      ImGui::TreePop();
    }
  }
  void SaveExtra(nlohmann::json& j) const override { j = {{"name", name_}, {"ammo", ammo_}, {"favourites", favs_}}; }
  void LoadExtra(const nlohmann::json& j) override {
    SetBuf(name_, sizeof(name_), j.value("name", std::string()));
    ammo_ = std::clamp(j.value("ammo", ammo_), 0, 999);
    favs_ = StrList(j, "favourites");
  }

 private:
  void Give() {
    const std::string q = LuaQuote(name_);
    runner_.Run("local p = CG.need(CG.player())\nCG.must(CG.call(p, \"InventoryAddWeapon\", " + q + ", " + LuaNumber(ammo_) +
                    "))\nCG.call(p, \"InventorySelect\", " + q + ", true)",
                Name(), [n = Display(name_)](const game::vm::Result& r) {
                  if (r.ok) nt::Push(nt::Kind::Success, "Weapon added", n);
                });
  }
  // Every regular weapon (the developer-only entry is skipped); one chunk, failures per item ignored.
  void GiveAll() {
    std::string code = "local p = CG.need(CG.player())\nlocal n = 0\n";
    for (const CatalogItem& w : kWeapons) {
      if (std::string_view(w.group).find("Developer") != std::string_view::npos) continue;
      code += "if CG.call(p, \"InventoryAddWeapon\", " + LuaQuote(w.id) + ", " + LuaNumber(ammo_) + ") then n = n + 1 end\n";
    }
    code += "if n == 0 then error(\"no weapon could be added\", 0) end";
    runner_.Run(std::move(code), Name(), [](const game::vm::Result& r) {
      if (r.ok) nt::Push(nt::Kind::Success, "Weapons", "Every weapon added");
    });
  }
  static std::string Display(const std::string& id) {
    const std::string n = CatalogName(kWeapons, id);
    return n.empty() ? id : n;
  }
  char filter_[64] = {};
  void Remember() {
    if (!game::vm::HasReturnValues()) {
      nt::Push(nt::Kind::Warning, "Remember current", "Return values need the Lua.PushCClosure/SetField/CheckLString bindings");
      return;
    }
    // Plain pcall (no CG prelude needed): the selected weapon's name or nil.
    game::vm::Run(R"(local ok, w = pcall(function() return game.game:GetActivePlayer():InventoryGetSelected() end)
if ok and w ~= nil then return tostring(w) end
return nil)",
                  [this, alive = alive_](const game::vm::Result& r) {
                    if (!*alive) return;
                    if (!r.ok || r.values.empty() || r.values[0].empty() || r.values[0] == "nil") {
                      nt::Push(nt::Kind::Warning, "Remember current", r.ok ? "No weapon selected" : r.error);
                      return;
                    }
                    SetBuf(name_, sizeof(name_), r.values[0]);
                    if (std::find(favs_.begin(), favs_.end(), r.values[0]) == favs_.end()) favs_.push_back(r.values[0]);
                    nt::Push(nt::Kind::Success, "Remembered", r.values[0]);
                  },
                  "=chroma:weapon");
  }
  Runner runner_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  char name_[128] = {};
  int ammo_ = 120;
  std::vector<std::string> favs_;

 public:
  ~GiveWeapon() override { *alive_ = false; }
};

}  // namespace

void RegisterWeaponFeatures(Registry& r) {
  r.Add(std::make_unique<ScriptToggle>("weapons.unlimited_ammo", "Unlimited ammo", Category::Weapons,
                                       "Never run dry; re-applied every 3 s.",
                                       "local p = CG.need(CG.player())\nCG.must(CG.call(p, \"InventorySetUnlimitedAmmo\", true))",
                                       "local p = CG.need(CG.player())\nCG.call(p, \"InventorySetUnlimitedAmmo\", false)", 3.f));
  r.Add(std::make_unique<ScriptAction>("weapons.refill", "Refill ammo", Category::Weapons,
                                       "Max ammo for every weapon you carry.",
                                       "local p = CG.need(CG.player())\nCG.must(CG.call(p, \"InventoryLoadWeapons\"))"));
  r.Add(std::make_unique<Grenades>());
  r.Add(std::make_unique<GiveWeapon>());
}

}  // namespace cg::features
