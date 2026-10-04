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

// Script toggle: `on` at enable, then `tick` (default: `on`) every `period` s when > 0, `off` at disable.
class ScriptToggle : public Feature {
 public:
  ScriptToggle(std::string id, std::string name, Category c, std::string desc, std::string on, std::string off, float period,
               std::string tick = {})
      : Feature(std::move(id), std::move(name), c, Kind::Toggle, std::move(desc)), on_(std::move(on)), off_(std::move(off)),
        tick_(tick.empty() ? on_ : std::move(tick)), period_(period) {}
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
    runner_.Run(tick_, Name(), {}, false);
  }

 protected:
  Runner runner_;

 private:
  std::string on_, off_, tick_;
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
                        "Paint, wheels, window tint, plate, dirt/rust, fuel, lights and siren of the current vehicle.") {}
  std::vector<std::string> Requires() const override { return {"vm"}; }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    // The game takes palette IDs (paint/wheels 1-42, tint 1-10), not RGB; sliders apply on release.
    if (Slider("Paint", &paint_, 1, 42)) Send(V("CG.must(CG.call(v, \"SetColor\", " + Num(paint_) + ", " + Num(paint2_) + "))"));
    if (Slider("Second paint", &paint2_, 1, 42)) Send(V("CG.must(CG.call(v, \"SetColor\", " + Num(paint_) + ", " + Num(paint2_) + "))"));
    if (Slider("Wheels", &wheels_, 1, 42)) Send(V("CG.must(CG.call(v, \"SetWheelColor\", " + Num(wheels_) + ", " + Num(wheels_) + "))"));
    if (Slider("Window tint", &tint_, 1, 10)) Send(V("CG.must(CG.call(v, \"SetWindowTint\", " + Num(tint_) + "))"));
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("##plate", plate_, sizeof(plate_));
    ImGui::SameLine();
    if (ImGui::Button("Set plate")) Send(V("CG.must(CG.call(v, \"SetSPZText\", " + LuaQuote(plate_) + ", true))"));
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("Dirt", &dirt_, 0.f, 1.f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) Send(V("CG.must(CG.call(v, \"SetDirty\", " + LuaNumber(dirt_) + "))"));
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("Rust", &rust_, 0.f, 1.f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) Send(V("CG.must(CG.call(v, \"SetRust\", " + LuaNumber(rust_) + "))"));
    if (ImGui::Button("Clean")) Send(V("CG.call(v, \"SetDirty\", 0)\nCG.must(CG.call(v, \"SetRust\", 0))"));
    ImGui::SameLine();
    if (ImGui::Button("Fill tank")) Send(V("CG.must(CG.call(v, \"SetActualFuel\", 100))"));
    ImGui::SameLine();
    if (ImGui::Button("Engine on/off"))
      Send(V("local ok, on = CG.call(v, \"IsEngineOn\")\nlocal s = not (ok and on)\nCG.must(CG.call(v, \"SetEngineOn\", s, s))"));
    if (ImGui::Checkbox("Lights", &lights_)) Send(V(std::string("CG.must(CG.call(v, \"SetLightState\", true, ") + LuaBool(lights_) + "))"));
    ImGui::SameLine();
    if (ImGui::Checkbox("Siren", &siren_)) Send(V(std::string("CG.must(CG.call(v, \"SetSirenOn\", ") + LuaBool(siren_) + "))"));
  }
  void SaveExtra(nlohmann::json& j) const override {
    j = {{"paint", paint_}, {"paint2", paint2_}, {"wheels", wheels_}, {"tint", tint_}, {"plate", plate_}, {"dirt", dirt_}, {"rust", rust_}};
  }
  void LoadExtra(const nlohmann::json& j) override {
    paint_ = std::clamp(j.value("paint", paint_), 1, 42);
    paint2_ = std::clamp(j.value("paint2", paint2_), 1, 42);
    wheels_ = std::clamp(j.value("wheels", wheels_), 1, 42);
    tint_ = std::clamp(j.value("tint", tint_), 1, 10);
    SetBuf(plate_, sizeof(plate_), j.value("plate", std::string(plate_)));
    dirt_ = std::clamp(j.value("dirt", dirt_), 0.f, 1.f);
    rust_ = std::clamp(j.value("rust", rust_), 0.f, 1.f);
  }

 private:
  static std::string Num(int v) { return LuaNumber(v); }
  static bool Slider(const char* label, int* v, int lo, int hi) {
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt(label, v, lo, hi);
    return ImGui::IsItemDeactivatedAfterEdit();
  }
  void Send(std::string code) { runner_.Run(std::move(code), Name()); }
  Runner runner_;
  int paint_ = 1, paint2_ = 1, wheels_ = 1, tint_ = 1;
  char plate_[16] = "CG-1931";
  float dirt_ = 0, rust_ = 0;
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
  // Repair(true) resets the car (it visibly freezes for a moment), so it only runs once when switched on;
  // every second after that only explosions stay off and the engine is kept healthy (no reset).
  r.Add(std::make_unique<ScriptToggle>("vehicle.indestructible", "Indestructible", Category::Vehicle,
                                       "Can't explode and the engine never dies. Dents stay; use Repair to clean them.",
                                       "local v = CG.vehicle()\nif v then\n  CG.call(v, \"DisableExplosion\", true)\n"
                                       "  CG.call(v, \"SetMotorDamage\", 0)\n  CG.call(v, \"Repair\", true)\nend",
                                       "local v = CG.vehicle()\nif v then CG.call(v, \"DisableExplosion\", false) end", 1.f,
                                       "local v = CG.vehicle()\nif v then\n  CG.call(v, \"DisableExplosion\", true)\n"
                                       "  CG.call(v, \"SetMotorDamage\", 0)\nend"));
  r.Add(std::make_unique<Boost>());
  r.Add(std::make_unique<ScriptToggle>("vehicle.keep", "Keep vehicle", Category::Vehicle,
                                       "Whatever you drive never despawns. Works on foot too: it applies when you get in.",
                                       "local v = CG.vehicle()\nif v then CG.must(CG.call(v, \"SetDespawnImmunity\", true)) end",
                                       "local v = CG.vehicle()\nif v then CG.call(v, \"SetDespawnImmunity\", false) end", 5.f));
  r.Add(std::make_unique<Customize>());
  r.Add(std::make_unique<Spawn>());
}

}  // namespace cg::features
