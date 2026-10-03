// Vehicle features. Everything acts on player:GetOwner(); on foot the chunk fails with "not in a vehicle".
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "core/util.h"
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

// Prefix for every vehicle chunk: `v` is the current vehicle.
constexpr const char* kVeh = "local v = CG.vehicle()\nif not v then error(\"not in a vehicle\", 0) end\n";

std::string V(const std::string& body) { return kVeh + body; }

class Boost : public ScriptAction {
 public:
  Boost() : ScriptAction("vehicle.boost", "Boost", Category::Vehicle, "Sets the vehicle's speed instantly.", {}) {}
  void Activate() override { runner_.Run(V("CG.must(CG.call(v, \"SetSpeed\", " + LuaNumber(speed_) + "))"), Name()); }
  bool HasSettings() const override { return true; }
  void DrawSettings() override { ImGui::SliderFloat("Speed", &speed_, 5.f, 150.f, "%.0f"); }
  void SaveExtra(nlohmann::json& j) const override { j = {{"speed", speed_}}; }
  void LoadExtra(const nlohmann::json& j) override { speed_ = j.value("speed", speed_); }

 private:
  float speed_ = 45.f;
};

class Customize : public Feature {
 public:
  Customize() : Feature("vehicle.customize", "Customize", Category::Vehicle, Kind::Panel,
                        "Paint, window tint, licence plate, dirt/rust, fuel and switches of the current vehicle.") {}
  std::vector<std::string> Requires() const override { return {"vm"}; }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    ImGui::ColorEdit3("Primary", c1_, ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine();
    ImGui::ColorEdit3("Secondary", c2_, ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine();
    if (ImGui::Button("Paint")) Send(V("CG.must(CG.call(v, \"SetColor\", col(" + Col(c1_) + "), col(" + Col(c2_) + ")))"));
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("Window tint", &tint_, 0, 100);
    ImGui::SameLine();
    if (ImGui::Button("Tint")) Send(V("CG.must(CG.call(v, \"SetWindowTint\", " + LuaNumber(tint_) + "))"));
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("Plate", plate_, sizeof(plate_));
    ImGui::SameLine();
    if (ImGui::Button("Set plate")) Send(V("CG.must(CG.call(v, \"SetSPZText\", " + LuaQuote(plate_) + ", true))"));
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("Dirt", &dirt_, 0.f, 1.f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("Rust", &rust_, 0.f, 1.f, "%.2f");
    ImGui::SameLine();
    if (ImGui::Button("Weather it"))
      Send(V("CG.call(v, \"SetDirty\", " + LuaNumber(dirt_) + ")\nCG.must(CG.call(v, \"SetRust\", " + LuaNumber(rust_) + "))"));
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("Fuel", &fuel_, 0.f, 100.f, "%.0f");
    ImGui::SameLine();
    if (ImGui::Button("Fill")) Send(V("CG.must(CG.call(v, \"SetActualFuel\", " + LuaNumber(fuel_) + "))"));
    if (ImGui::Button("Engine on")) Send(V("CG.must(CG.call(v, \"SetEngineOn\", true, true))"));
    ImGui::SameLine();
    if (ImGui::Button("Engine off")) Send(V("CG.must(CG.call(v, \"SetEngineOn\", false, false))"));
    ImGui::SameLine();
    if (ImGui::Checkbox("Lights", &lights_)) Send(V(std::string("CG.must(CG.call(v, \"SetLightState\", ") + LuaBool(lights_) + ", true))"));
    ImGui::SameLine();
    if (ImGui::Checkbox("Siren", &siren_)) Send(V(std::string("CG.must(CG.call(v, \"SetSirenOn\", ") + LuaBool(siren_) + "))"));
    ImGui::SameLine();
    if (ImGui::Button("Kill engine")) Send(V("CG.must(CG.call(v, \"SetMotorDamage\", 1))"));
  }
  void SaveExtra(nlohmann::json& j) const override {
    j = {{"c1", {c1_[0], c1_[1], c1_[2]}}, {"c2", {c2_[0], c2_[1], c2_[2]}}, {"tint", tint_}, {"plate", plate_},
         {"dirt", dirt_}, {"rust", rust_}, {"fuel", fuel_}};
  }
  void LoadExtra(const nlohmann::json& j) override {
    for (int i = 0; i < 3; ++i) {
      if (j.contains("c1") && j["c1"].size() == 3) c1_[i] = j["c1"][i].get<float>();
      if (j.contains("c2") && j["c2"].size() == 3) c2_[i] = j["c2"][i].get<float>();
    }
    tint_ = j.value("tint", tint_);
    SetBuf(plate_, sizeof(plate_), j.value("plate", std::string(plate_)));
    dirt_ = j.value("dirt", dirt_);
    rust_ = j.value("rust", rust_);
    fuel_ = j.value("fuel", fuel_);
  }

 private:
  static std::string Col(const float* c) {
    return LuaNumber(static_cast<int>(c[0] * 255.f + .5f)) + ", " + LuaNumber(static_cast<int>(c[1] * 255.f + .5f)) + ", " +
           LuaNumber(static_cast<int>(c[2] * 255.f + .5f));
  }
  void Send(std::string code) {
    // col(r, g, b): Math:newColor (0..255) where it exists, else a 0..1 vector.
    runner_.Run(R"(local function col(r, g, b)
  if Math and Math.newColor then
    local ok, c = CG.try(Math.newColor, Math, r, g, b, 255)
    if ok and c ~= nil then return c end
  end
  return CG.vec(r / 255, g / 255, b / 255)
end
)" + code,
                Name());
  }
  Runner runner_;
  float c1_[3] = {0.08f, 0.08f, 0.09f}, c2_[3] = {0.55f, 0.45f, 0.25f};
  int tint_ = 0;
  char plate_[16] = "CG-1931";
  float dirt_ = 0, rust_ = 0, fuel_ = 100;
  bool lights_ = false, siren_ = false;
};

class Spawn : public Feature {
 public:
  Spawn() : Feature("vehicle.spawn", "Spawn vehicle", Category::Vehicle, Kind::Panel,
                    "Spawns a vehicle by model name through the game's debug console module (carpls).") {}
  std::vector<std::string> Requires() const override { return {"vm"}; }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    ImGui::SetNextItemWidth(-90);
    const bool enter = ImGui::InputTextWithHint("##model", "model name", name_, sizeof(name_), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Spawn") || enter) && name_[0]) Go();
    if (Favourites(name_, sizeof(name_), favs_)) Go();
  }
  void SaveExtra(nlohmann::json& j) const override { j = {{"name", name_}, {"favourites", favs_}}; }
  void LoadExtra(const nlohmann::json& j) override {
    SetBuf(name_, sizeof(name_), j.value("name", std::string()));
    favs_ = StrList(j, "favourites");
  }

 private:
  void Go() {
    runner_.Run("local name = " + LuaQuote(name_) + R"(
local m = CG.get("package", "loaded", "common.base.game_structure_console")
if type(m) ~= "table" then m = CG.get("package", "loaded", "common", "base", "game_structure_console") end
if type(m) ~= "table" or type(m.carpls) ~= "function" then error("vehicle spawner (carpls) not available on this build", 0) end
CG.must(CG.try(m.carpls, name)))",
                Name(), [n = std::string(name_)](const game::vm::Result& r) {
                  if (r.ok) nt::Push(nt::Kind::Success, "Spawned", n);
                });
  }
  Runner runner_;
  char name_[128] = {};
  std::vector<std::string> favs_;
};

}  // namespace

void RegisterVehicleFeatures(Registry& r) {
  r.Add(std::make_unique<ScriptAction>("vehicle.repair", "Repair", Category::Vehicle, "Fully repairs the current vehicle.",
                                       V("CG.must(CG.call(v, \"Repair\", true))")));
  r.Add(std::make_unique<ScriptToggle>("vehicle.indestructible", "Indestructible", Category::Vehicle,
                                       "No explosions; repaired every second.",
                                       V("CG.call(v, \"DisableExplosion\", true)\nCG.must(CG.call(v, \"Repair\", true))"),
                                       "local v = CG.vehicle()\nif v then CG.call(v, \"DisableExplosion\", false) end", 1.f));
  r.Add(std::make_unique<Boost>());
  r.Add(std::make_unique<ScriptToggle>("vehicle.keep", "Keep vehicle", Category::Vehicle,
                                       "The current vehicle never despawns.",
                                       V("CG.must(CG.call(v, \"SetDespawnImmunity\", true))"),
                                       "local v = CG.vehicle()\nif v then CG.call(v, \"SetDespawnImmunity\", false) end", 5.f));
  r.Add(std::make_unique<Customize>());
  r.Add(std::make_unique<Spawn>());
}

}  // namespace cg::features
