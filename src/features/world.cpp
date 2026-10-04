// World features: time of day, time flow, weather, police, traffic, water. Script-first with memory fallbacks.
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

std::string Gfx(const char* method, const std::string& args) {
  return std::string("CG.must(CG.call(CG.get(\"game\", \"gfx\"), \"") + method + "\"" + (args.empty() ? "" : ", " + args) + "))";
}

// Slider panel: script call when the VM is ready, memory setter otherwise (or when the script call fails).
class ScriptOrMemorySlider : public Feature {
 public:
  using MemSet = bool (*)(float);
  ScriptOrMemorySlider(std::string id, std::string name, std::string desc, const char* method, MemSet memSet, const char* memSym,
                       float lo, float hi, float def, const char* fmt)
      : Feature(std::move(id), std::move(name), Category::World, Kind::Panel, std::move(desc)),
        method_(method), memSet_(memSet), memSym_(memSym), lo_(lo), hi_(hi), value_(def), fmt_(fmt) {}
  std::vector<std::string> Requires() const override { return {"vm?"}; }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    ImGui::SetNextItemWidth(-70);
    ImGui::SliderFloat("##v", &value_, lo_, hi_, fmt_);
    ImGui::SameLine();
    if (ImGui::Button("Apply")) Apply();
    ImGui::TextDisabled("via %s", game::vm::Ready() ? method_ : memSym_);
  }
  void SaveExtra(nlohmann::json& j) const override { j = {{"value", value_}}; }
  void LoadExtra(const nlohmann::json& j) override { value_ = std::clamp(j.value("value", value_), lo_, hi_); }

 private:
  void Apply() {
    const float v = value_;
    const MemSet memSet = memSet_;
    const std::string name = Name();
    if (!game::vm::Ready()) {
      if (!memSet(v)) nt::Push(nt::Kind::Warning, name, std::string("Script VM not ready and ") + memSym_ + " unavailable");
      return;
    }
    // SetTime takes (hours, transition); SetTimeFlowSpeed takes one value (engine signatures).
    const std::string args = std::string(method_) == "SetTime" ? LuaNumber(v) + ", 0" : LuaNumber(v);
    runner_.Run(Gfx(method_, args), name, [v, memSet, name](const game::vm::Result& r) {
      if (!r.ok && !memSet(v)) nt::Push(nt::Kind::Error, name + " failed", r.error);
    }, false);
  }
  Runner runner_;
  const char* method_;
  MemSet memSet_;
  const char* memSym_;
  float lo_, hi_, value_;
  const char* fmt_;
};

class Weather : public Feature {
 public:
  Weather() : Feature("world.weather", "Weather", Category::World, Kind::Panel,
                      "Sets a weather set by name. Remember the current one to build your own list.") {}
  std::vector<std::string> Requires() const override { return {"vm"}; }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    const std::string cur = game::state::Get().weather;
    ImGui::TextDisabled("Current: %s", cur.empty() ? "(unknown)" : cur.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Remember") && !cur.empty()) {
      SetBuf(name_, sizeof(name_), cur);
      if (std::find(favs_.begin(), favs_.end(), cur) == favs_.end()) favs_.push_back(cur);
    }
    ImGui::SetNextItemWidth(-70);
    const bool enter = ImGui::InputTextWithHint("##w", "weather set name", name_, sizeof(name_), ImGuiInputTextFlags_EnterReturnsTrue);
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
  void Apply() { runner_.Run(Gfx("SetWeatherSet", LuaQuote(name_) + ", 0, 0"), Name()); }
  Runner runner_;
  char name_[128] = {};
  std::vector<std::string> favs_;
};

class Traffic : public Feature {
 public:
  Traffic() : Feature("world.traffic", "Traffic", Category::World, Kind::Panel, "Ambient traffic generators and density.") {}
  std::vector<std::string> Requires() const override { return {"vm"}; }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    if (ImGui::Button("Generators on")) Call("SwitchGenerators", "true");
    ImGui::SameLine();
    if (ImGui::Button("Generators off")) Call("SwitchGenerators", "false");
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("Cars", &count_, 0, 100);
    ImGui::SameLine();
    if (ImGui::Button("Populate")) Call("Populate", LuaNumber(count_));
  }
  void SaveExtra(nlohmann::json& j) const override { j = {{"count", count_}}; }
  void LoadExtra(const nlohmann::json& j) override { count_ = std::clamp(j.value("count", count_), 0, 100); }

 private:
  void Call(const char* m, const std::string& arg) {
    runner_.Run(std::string("CG.must(CG.call(CG.get(\"game\", \"traffic\"), \"") + m + "\", " + arg + "))", Name());
  }
  Runner runner_;
  int count_ = 20;
};

}  // namespace

void RegisterWorldFeatures(Registry& r) {
  r.Add(std::make_unique<ScriptOrMemorySlider>("world.time", "Time of day", "Sets the clock (hours).", "SetTime",
                                               &game::world::SetTimeOfDay, "World.TimeOfDay", 0.f, 24.f, 12.f, "%.1f h"));
  r.Add(std::make_unique<ScriptOrMemorySlider>("world.timeflow", "Time flow", "How fast the in-game clock runs (1 = normal).",
                                               "SetTimeFlowSpeed", &game::world::SetTimeScale, "World.TimeScale", 0.f, 50.f, 1.f, "%.2fx"));
  r.Add(std::make_unique<Weather>());
  r.Add(std::make_unique<ScriptToggle>("world.no_police", "Disable police", Category::World, "Police ignore you; re-applied every 5 s.",
                                       "CG.must(CG.call(CG.get(\"game\", \"police\"), \"Disable\"))",
                                       "CG.must(CG.call(CG.get(\"game\", \"police\"), \"Enable\"))", 5.f));
  r.Add(std::make_unique<Traffic>());
  r.Add(std::make_unique<ScriptToggle>("world.hide_water", "Hide water", Category::World, "Toggles water visibility (on = water hidden).",
                                       "CG.must(CG.call(CG.get(\"game\", \"game\"), \"SetWaterVisibility\", false))",
                                       "CG.must(CG.call(CG.get(\"game\", \"game\"), \"SetWaterVisibility\", true))", 0.f));
}

}  // namespace cg::features
