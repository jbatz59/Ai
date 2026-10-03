// Camera features: field of view (memory) and HUD hiding (script).
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
#include "game/game.h"
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

class Fov : public Feature {
 public:
  Fov() : Feature("camera.fov", "Field of view", Category::Camera, Kind::Panel, "Camera FOV in degrees; Lock re-writes it every frame.") {}
  std::vector<std::string> Requires() const override { return {"Camera.Fov"}; }
  void Tick(float) override {
    if (lock_) game::camera::SetFov(fov_);
  }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    const auto cur = game::camera::Fov();
    ImGui::SetNextItemWidth(-140);
    if (ImGui::SliderFloat("##fov", &fov_, 30.f, 120.f, "%.0f deg")) game::camera::SetFov(fov_);
    ImGui::SameLine();
    ImGui::Checkbox("Lock", &lock_);
    ImGui::SameLine();
    if (ImGui::SmallButton("Read") && cur) fov_ = *cur;
    if (cur) ImGui::TextDisabled("Game: %.1f deg", *cur);
  }
  void SaveExtra(nlohmann::json& j) const override { j = {{"fov", fov_}, {"lock", lock_}}; }
  void LoadExtra(const nlohmann::json& j) override {
    fov_ = std::clamp(j.value("fov", fov_), 30.f, 120.f);
    lock_ = j.value("lock", false);
  }

 private:
  float fov_ = 70.f;
  bool lock_ = false;
};

}  // namespace

void RegisterCameraFeatures(Registry& r) {
  r.Add(std::make_unique<Fov>());
  r.Add(std::make_unique<ScriptToggle>("camera.hide_hud", "Hide HUD", Category::Camera, "Hides the game HUD (cinematic shots).",
                                       "CG.must(CG.call(CG.get(\"game\", \"hud\"), \"Show\", false))",
                                       "CG.must(CG.call(CG.get(\"game\", \"hud\"), \"Show\", true))", 0.f));
}

}  // namespace cg::features
