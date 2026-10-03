// Teleport features: saved locations, quick slot, coordinates, forward hop.
#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/util.h"
#include "features/feature.h"
#include "features/game_lua.h"
#include "game/game.h"
#include "game/script_vm.h"
#include "game/state.h"
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

Runner g_tp;   // shared: one teleport chunk in flight at a time

std::optional<game::Vec3> CurrentPos() {
  const auto s = game::state::Get();
  if (!s.valid) return game::player::Position();
  return s.inVehicle ? s.vehiclePos : s.playerPos;
}

// Script SetPos on the vehicle/player; memory game::player::SetPosition as fallback.
void TeleportTo(const game::Vec3& p, const std::string& label) {
  auto done = [p, label](bool ok, const std::string& err) {
    if (ok) nt::Push(nt::Kind::Success, "Teleported", label, 2.0f);
    else nt::Push(nt::Kind::Error, "Teleport failed", err);
  };
  if (!game::vm::Ready()) {
    done(game::player::SetPosition(p), "script VM not ready and Entity.SetPosition unavailable");
    return;
  }
  const std::string code = "local o = CG.need(CG.target())\nCG.must(CG.call(o, \"SetPos\", CG.need(CG.vec(" + LuaNumber(p.x) + ", " +
                           LuaNumber(p.y) + ", " + LuaNumber(p.z) + "))))";
  if (!g_tp.Run(code, "Teleport", [p, done](const game::vm::Result& r) {
        if (r.ok) done(true, {});
        else done(game::player::SetPosition(p), r.error);
      }, false))
    nt::Push(nt::Kind::Info, "Teleport", "busy, try again");
}

nlohmann::json Pt(const std::string& name, const game::Vec3& p) { return {{"name", name}, {"x", p.x}, {"y", p.y}, {"z", p.z}}; }
game::Vec3 FromJson(const nlohmann::json& j) { return {j.value("x", 0.f), j.value("y", 0.f), j.value("z", 0.f)}; }

class Locations : public Feature {
 public:
  Locations() : Feature("teleport.locations", "Locations", Category::Teleport, Kind::Panel, "Saved places. Save where you stand, teleport back any time.") {}
  std::vector<std::string> Requires() const override { return {"vm?"}; }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    Load();
    ImGui::SetNextItemWidth(-120);
    ImGui::InputTextWithHint("##name", "name for this spot", name_, sizeof(name_));
    ImGui::SameLine();
    if (ImGui::Button("Save current")) {
      if (const auto p = CurrentPos()) {
        list_.push_back(Pt(name_[0] ? name_ : "Location " + std::to_string(list_.size() + 1), *p));
        name_[0] = 0;
        Store();
      } else {
        nt::Push(nt::Kind::Warning, "Locations", "Player position unknown");
      }
    }
    int remove = -1;
    for (size_t i = 0; i < list_.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      auto& e = list_[i];
      const std::string n = e.value("name", std::string("?"));
      if (renaming_ == static_cast<int>(i)) {
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputText("##rn", rename_, sizeof(rename_), ImGuiInputTextFlags_EnterReturnsTrue)) {
          e["name"] = std::string(rename_);
          renaming_ = -1;
          Store();
        }
      } else if (ImGui::Button(n.c_str(), {200, 0})) {
        TeleportTo(FromJson(e), n);
      }
      ImGui::SameLine();
      ImGui::TextDisabled("%.0f %.0f %.0f", e.value("x", 0.f), e.value("y", 0.f), e.value("z", 0.f));
      ImGui::SameLine();
      if (ImGui::SmallButton("Rename")) {
        renaming_ = static_cast<int>(i);
        SetBuf(rename_, sizeof(rename_), n);
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Delete")) remove = static_cast<int>(i);
      ImGui::PopID();
    }
    if (remove >= 0) {
      list_.erase(list_.begin() + remove);
      renaming_ = -1;
      Store();
    }
  }

 private:
  void Load() {
    if (loaded_) return;
    loaded_ = true;
    const auto j = Config::Get().ReadJson("teleport.locations");
    if (j.is_array())
      for (const auto& e : j)
        if (e.is_object()) list_.push_back(e);
  }
  void Store() { Config::Get().WriteJson("teleport.locations", nlohmann::json(list_)); }
  bool loaded_ = false;
  std::vector<nlohmann::json> list_;
  char name_[64] = {}, rename_[64] = {};
  int renaming_ = -1;
};

class QuickSave : public Feature {
 public:
  QuickSave() : Feature("teleport.quick_save", "Quick slot save", Category::Teleport, Kind::Action, "Stores the current position in the quick slot.") {}
  void Activate() override {
    const auto p = CurrentPos();
    if (!p) return nt::Push(nt::Kind::Warning, Name(), "Player position unknown");
    Config::Get().WriteJson("teleport.quickslot", Pt("quick", *p));
    nt::Push(nt::Kind::Success, Name(), "Position stored", 1.8f);
  }
};

class QuickLoad : public Feature {
 public:
  QuickLoad() : Feature("teleport.quick_load", "Quick slot teleport", Category::Teleport, Kind::Action, "Teleports to the quick slot.") {}
  std::vector<std::string> Requires() const override { return {"vm?"}; }
  void Activate() override {
    const auto j = Config::Get().ReadJson("teleport.quickslot");
    if (!j.is_object()) return nt::Push(nt::Kind::Warning, Name(), "Quick slot is empty");
    TeleportTo(FromJson(j), "Quick slot");
  }
};

class Coordinates : public Feature {
 public:
  Coordinates() : Feature("teleport.coords", "Coordinates", Category::Teleport, Kind::Panel, "Teleport to exact world coordinates (Z up).") {}
  std::vector<std::string> Requires() const override { return {"vm?"}; }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    ImGui::SetNextItemWidth(260);
    ImGui::InputFloat3("X / Y / Z", xyz_, "%.1f");
    if (ImGui::Button("Use current"))
      if (const auto p = CurrentPos()) xyz_[0] = p->x, xyz_[1] = p->y, xyz_[2] = p->z;
    ImGui::SameLine();
    if (ImGui::Button("Teleport")) TeleportTo({xyz_[0], xyz_[1], xyz_[2]}, "Coordinates");
  }
  void SaveExtra(nlohmann::json& j) const override { j = {{"xyz", {xyz_[0], xyz_[1], xyz_[2]}}}; }
  void LoadExtra(const nlohmann::json& j) override {
    if (j.contains("xyz") && j["xyz"].size() == 3)
      for (int i = 0; i < 3; ++i) xyz_[i] = j["xyz"][i].get<float>();
  }

 private:
  float xyz_[3] = {};
};

class Forward : public Feature {
 public:
  Forward() : Feature("teleport.forward", "Forward", Category::Teleport, Kind::Action, "Hops forward along the facing direction.") {}
  std::vector<std::string> Requires() const override { return {"vm"}; }
  void Activate() override {
    g_tp.Run("local dist = " + LuaNumber(dist_) + R"(
local o = CG.need(CG.target())
local ok, pos = CG.call(o, "GetPos")
if not ok or not pos then error(pos or "no position", 0) end
local d = CG.must(CG.call(o, "GetDir"))
local l = math.sqrt(d.x * d.x + d.y * d.y)
if l < 0.001 then error("no facing direction", 0) end
CG.must(CG.call(o, "SetPos", CG.need(CG.vec(pos.x + d.x / l * dist, pos.y + d.y / l * dist, pos.z + 0.5)))))",
             Name());
  }
  bool HasSettings() const override { return true; }
  void DrawSettings() override { ImGui::SliderFloat("Distance (m)", &dist_, 2.f, 200.f, "%.0f"); }
  void SaveExtra(nlohmann::json& j) const override { j = {{"distance", dist_}}; }
  void LoadExtra(const nlohmann::json& j) override { dist_ = j.value("distance", dist_); }

 private:
  float dist_ = 10.f;
};

}  // namespace

void RegisterTeleportFeatures(Registry& r) {
  r.Add(std::make_unique<Locations>());
  r.Add(std::make_unique<QuickSave>());
  r.Add(std::make_unique<QuickLoad>());
  r.Add(std::make_unique<Coordinates>());
  r.Add(std::make_unique<Forward>());
}

}  // namespace cg::features
