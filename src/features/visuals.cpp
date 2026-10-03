// Visual features: HUD element switches (script) and screen overlays (speedometer, coordinates).
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
#include "game/state.h"
#include "render/draw.h"
#include "render/overlay.h"
#include "render/theme.h"
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

namespace th = render::theme;

class HudElements : public Feature {
 public:
  HudElements() : Feature("visuals.hud", "HUD elements", Category::Visuals, Kind::Panel, "Show or hide the radar and the game's speedometer.") {}
  std::vector<std::string> Requires() const override { return {"vm"}; }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    if (ImGui::Checkbox("Radar", &radar_)) Send("RadarShow", radar_);
    ImGui::SameLine();
    if (ImGui::Checkbox("Speedometer", &speedo_)) Send("SpeedometerShow", speedo_);
  }
  void SaveExtra(nlohmann::json& j) const override { j = {{"radar", radar_}, {"speedo", speedo_}}; }
  void LoadExtra(const nlohmann::json& j) override {
    radar_ = j.value("radar", true);
    speedo_ = j.value("speedo", true);
  }

 private:
  void Send(const char* m, bool on) {
    RunFeatureChunk(std::string("CG.must(CG.call(CG.get(\"game\", \"hud\"), \"") + m + "\", " + LuaBool(on) + "))", Name());
  }
  bool radar_ = true, speedo_ = true;
};

// Small themed text box anchored to a screen corner.
void Box(ImVec2 anchor, bool right, const char* text, float scale) {
  const auto& c = th::Colors();
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  ImFont* font = ImGui::GetFont();
  const float size = ImGui::GetFontSize() * scale;
  const ImVec2 ts = font->CalcTextSizeA(size, 1e9f, 0, text);
  const ImVec2 pad{10, 6};
  const ImVec2 p0{right ? anchor.x - ts.x - pad.x * 2 : anchor.x, anchor.y - ts.y - pad.y * 2};
  const ImVec2 p1{p0.x + ts.x + pad.x * 2, anchor.y};
  dl->AddRectFilled(p0, p1, th::U32(c.bg, 0.78f), 6.f);
  dl->AddRect(p0, p1, th::U32(c.accentDim), 6.f);
  dl->AddText(font, size, {p0.x + pad.x, p0.y + pad.y}, th::U32(c.accent), text);
}

class SpeedOverlay : public Feature {
 public:
  SpeedOverlay() : Feature("visuals.speedometer", "Speedometer overlay", Category::Visuals, Kind::Toggle, "Speed readout in the bottom-right corner.") {}
  void DrawOverlay() override {
    const auto s = game::state::Get();
    if (!s.valid) return;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.0f %s", s.speedMs * (mph_ ? 2.23694f : 3.6f), mph_ ? "mph" : "km/h");
    Box({render::ScreenWidth() - 24.f, render::ScreenHeight() - 120.f}, true, buf, 1.6f);
  }
  bool HasSettings() const override { return true; }
  void DrawSettings() override { ImGui::Checkbox("Miles per hour", &mph_); }
  void SaveExtra(nlohmann::json& j) const override { j = {{"mph", mph_}}; }
  void LoadExtra(const nlohmann::json& j) override { mph_ = j.value("mph", false); }

 private:
  bool mph_ = false;
};

class CoordsOverlay : public Feature {
 public:
  CoordsOverlay() : Feature("visuals.coords", "Coordinates overlay", Category::Visuals, Kind::Toggle, "Player / vehicle position in the bottom-left corner.") {}
  void DrawOverlay() override {
    const auto s = game::state::Get();
    if (!s.valid) return;
    const game::Vec3 p = s.inVehicle ? s.vehiclePos : s.playerPos;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s  X %.1f  Y %.1f  Z %.1f", s.inVehicle ? "Car" : "On foot", p.x, p.y, p.z);
    Box({24.f, render::ScreenHeight() - 24.f}, false, buf, 1.0f);
  }
};

}  // namespace

void RegisterVisualFeatures(Registry& r) {
  r.Add(std::make_unique<HudElements>());
  r.Add(std::make_unique<SpeedOverlay>());
  r.Add(std::make_unique<CoordsOverlay>());
}

}  // namespace cg::features
