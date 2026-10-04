// Fun & Chaos: cinematic overlays (film reel, newspaper headlines, art-deco speedometer), rewind and chaos mode.
// The first three are pure overlays and need neither bindings nor the script VM.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/util.h"
#include "features/feature.h"
#include "features/game_lua.h"
#include "game/script_vm.h"
#include "game/state.h"
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
constexpr float kPi = 3.14159265f;

ImU32 Rgba(int r, int g, int b, float a) { return IM_COL32(r, g, b, static_cast<int>(std::clamp(a, 0.f, 1.f) * 255.f)); }

// Deterministic per-frame LCG (grain must not depend on std::rand state).
struct Lcg {
  uint32_t s;
  explicit Lcg(uint64_t seed) : s(static_cast<uint32_t>(seed * 2654435761u) ^ 0x9E3779B9u) {}
  float Next() {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(s >> 8) / 16777216.f;
  }
};

ImVec2 Screen() { return {render::ScreenWidth(), render::ScreenHeight()}; }

// Vignette: concentric gradient bands on all four edges.
void Vignette(ImDrawList* dl, ImVec2 sz, float strength) {
  if (strength <= 0) return;
  const ImU32 clear = Rgba(10, 6, 2, 0);
  for (int i = 0; i < 3; ++i) {
    const float t = std::min(sz.x, sz.y) * (0.10f + 0.09f * static_cast<float>(i));
    const ImU32 dark = Rgba(10, 6, 2, strength * (0.55f - 0.15f * static_cast<float>(i)));
    dl->AddRectFilledMultiColor({0, 0}, {sz.x, t}, dark, dark, clear, clear);
    dl->AddRectFilledMultiColor({0, sz.y - t}, sz, clear, clear, dark, dark);
    dl->AddRectFilledMultiColor({0, 0}, {t, sz.y}, dark, clear, clear, dark);
    dl->AddRectFilledMultiColor({sz.x - t, 0}, sz, clear, dark, dark, clear);
  }
}

// ---- Film reel ---------------------------------------------------------------------------------------
class FilmReel : public Feature {
 public:
  FilmReel() : Feature("fun.film_reel", "Film reel", Category::Fun, Kind::Toggle,
                       "1930s projector look: sepia wash, vignette, grain, scratches, flicker and optional letterbox.") {}
  void DrawOverlay() override {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 sz = Screen();
    const uint64_t frame = render::FrameCount();
    Lcg rng(frame);
    const float flick = 1.f - flicker_ * 0.35f * rng.Next();
    dl->AddRectFilled({0, 0}, sz, Rgba(112, 66, 20, wash_ * flick));
    Vignette(dl, sz, vignette_);
    const int dots = static_cast<int>(grain_ * 1400.f);
    for (int i = 0; i < dots; ++i) {
      const ImVec2 p{rng.Next() * sz.x, rng.Next() * sz.y};
      const float s = 0.6f + rng.Next() * 1.4f;
      const bool light = rng.Next() < 0.35f;
      dl->AddRectFilled(p, {p.x + s, p.y + s}, light ? Rgba(255, 240, 210, 0.35f * grain_) : Rgba(20, 12, 4, 0.5f * grain_));
    }
    // Scratches live for a few frames and wander sideways.
    if (rng.Next() < scratches_ * 0.08f && scratchList_.size() < 4)
      scratchList_.push_back({rng.Next() * sz.x, 4 + static_cast<int>(rng.Next() * 20), rng.Next() < 0.5f});
    for (auto& s : scratchList_) {
      s.x += (rng.Next() - 0.5f) * 3.f;
      const ImU32 col = s.light ? Rgba(255, 245, 220, 0.5f * scratches_) : Rgba(15, 8, 2, 0.6f * scratches_);
      float y = 0, x = s.x;
      while (y < sz.y) {
        const float ny = y + 40.f + rng.Next() * 60.f, nx = x + (rng.Next() - 0.5f) * 2.f;
        dl->AddLine({x, y}, {nx, ny}, col, 1.f + rng.Next());
        x = nx;
        y = ny;
      }
      --s.life;
    }
    std::erase_if(scratchList_, [](const Scratch& s) { return s.life <= 0; });
    if (letterbox_) {
      const float h = sz.y * 0.1f;
      dl->AddRectFilled({0, 0}, {sz.x, h}, IM_COL32(0, 0, 0, 255));
      dl->AddRectFilled({0, sz.y - h}, sz, IM_COL32(0, 0, 0, 255));
    }
  }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    ImGui::SliderFloat("Sepia wash", &wash_, 0.f, 0.6f, "%.2f");
    ImGui::SliderFloat("Vignette", &vignette_, 0.f, 1.f, "%.2f");
    ImGui::SliderFloat("Grain", &grain_, 0.f, 1.f, "%.2f");
    ImGui::SliderFloat("Scratches", &scratches_, 0.f, 1.f, "%.2f");
    ImGui::SliderFloat("Flicker", &flicker_, 0.f, 1.f, "%.2f");
    ImGui::Checkbox("Letterbox", &letterbox_);
  }
  void SaveExtra(nlohmann::json& j) const override {
    j = {{"wash", wash_}, {"vignette", vignette_}, {"grain", grain_}, {"scratches", scratches_}, {"flicker", flicker_}, {"letterbox", letterbox_}};
  }
  void LoadExtra(const nlohmann::json& j) override {
    wash_ = j.value("wash", wash_);
    vignette_ = j.value("vignette", vignette_);
    grain_ = j.value("grain", grain_);
    scratches_ = j.value("scratches", scratches_);
    flicker_ = j.value("flicker", flicker_);
    letterbox_ = j.value("letterbox", letterbox_);
  }

 private:
  struct Scratch {
    float x;
    int life;
    bool light;
  };
  std::vector<Scratch> scratchList_;
  float wash_ = 0.22f, vignette_ = 0.7f, grain_ = 0.45f, scratches_ = 0.4f, flicker_ = 0.4f;
  bool letterbox_ = false;
};

// ---- Extra! Extra! -----------------------------------------------------------------------------------
// Rotates/scales/fades every vertex added to dl since `start` around `c`.
void TransformSince(ImDrawList* dl, int start, ImVec2 c, float angle, float scale, float alpha) {
  const float cs = std::cos(angle), sn = std::sin(angle);
  for (int i = start; i < dl->VtxBuffer.Size; ++i) {
    ImDrawVert& v = dl->VtxBuffer[i];
    const float x = (v.pos.x - c.x) * scale, y = (v.pos.y - c.y) * scale;
    v.pos = {c.x + x * cs - y * sn, c.y + x * sn + y * cs};
    const ImU32 a = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
    v.col = (v.col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(static_cast<float>(a) * alpha) << IM_COL32_A_SHIFT);
  }
}

class Headlines : public Feature {
 public:
  Headlines() : Feature("fun.headlines", "Extra! Extra!", Category::Fun, Kind::Toggle,
                        "A spinning 1930s newspaper front page announces every cheat you toggle and every teleport.") {}
  void OnEnable() override {
    seen_.clear();
    primed_ = false;
    hasPos_ = false;
  }
  void Tick(float dt) override {
    for (const auto& f : Registry::Get().All()) {
      if (f.get() == this || f->GetKind() != Kind::Toggle) continue;
      const auto it = seen_.find(f.get());
      if (it == seen_.end()) {
        seen_[f.get()] = f->Enabled();
      } else if (it->second != f->Enabled()) {
        it->second = f->Enabled();
        if (primed_) Announce(f->Enabled() ? kOn : kOff, f->Name());
      }
    }
    primed_ = true;
    // Teleport detection: a jump of > 40 m between two snapshot updates less than 1 s apart.
    const auto s = game::state::Get();
    if (s.valid && s.updatedMs != lastUpdate_) {
      const game::Vec3 p = s.inVehicle ? s.vehiclePos : s.playerPos;
      if (hasPos_ && (p - lastPos_).Length() > 40.f && s.updatedMs - lastUpdate_ < 1000) Announce(kTeleport, {});
      lastPos_ = p;
      lastUpdate_ = s.updatedMs;
      hasPos_ = true;
    }
    if (!queue_.empty()) {
      age_ += dt;
      if (age_ > 3.1f) {
        queue_.pop_front();
        age_ = 0;
      }
    }
  }
  void DrawOverlay() override {
    if (queue_.empty()) return;
    const float t = age_;
    const float zoom = std::min(t / 0.6f, 1.f);
    const float eased = 1.f - (1.f - zoom) * (1.f - zoom) * (1.f - zoom);
    const float angle = (1.f - eased) * 4.f * kPi + (zoom >= 1.f ? -0.05f : 0.f);
    const float alpha = t < 2.6f ? 1.f : std::max(0.f, 1.f - (t - 2.6f) / 0.5f);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 sz = Screen(), c{sz.x * 0.5f, sz.y * 0.45f};
    const int start = dl->VtxBuffer.Size;
    DrawPaper(dl, c, sz, queue_.front());
    TransformSince(dl, start, c, angle, std::max(eased, 0.02f), alpha);
  }

 private:
  enum Event { kOn, kOff, kTeleport };
  void Announce(Event e, std::string name) {
    static const char* const kOnT[] = {"%s SWEEPS LOST HEAVEN!", "SALIERI FAMILY UNVEILS %s!", "%s: CITY HALL STUNNED!",
                                       "SHOCKING NEW CRAZE: %s!"};
    static const char* const kOffT[] = {"%s ABOLISHED! CROWDS WEEP", "POLICE CONFIRM: %s IS NO MORE", "MAYOR BANS %s!",
                                        "END OF AN ERA FOR %s"};
    static const char* const kTpT[] = {"TOMMY ANGELO VANISHES INTO THIN AIR!", "CAB DRIVER SEEN IN TWO PLACES AT ONCE!",
                                       "MAN APPEARS FROM NOWHERE - DETECTIVES BAFFLED!", "GHOST SIGHTED ON THE STREETS OF LOST HEAVEN!"};
    if (queue_.size() >= 4) return;
    for (auto& ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    char buf[160];
    const uint32_t r = rng_();
    if (e == kTeleport) std::snprintf(buf, sizeof(buf), "%s", kTpT[r % 4]);
    else std::snprintf(buf, sizeof(buf), e == kOn ? kOnT[r % 4] : kOffT[r % 4], name.c_str());
    queue_.push_back(buf);
  }
  static void DrawPaper(ImDrawList* dl, ImVec2 c, ImVec2 sz, const std::string& text) {
    ImFont* font = ImGui::GetFont();
    float big = ImGui::GetFontSize() * 2.4f;
    ImVec2 ts = font->CalcTextSizeA(big, 1e9f, 0, text.c_str());
    if (ts.x > sz.x * 0.75f) {
      big *= sz.x * 0.75f / ts.x;
      ts = font->CalcTextSizeA(big, 1e9f, 0, text.c_str());
    }
    const float small = ImGui::GetFontSize() * 1.1f;
    const float w = std::max(ts.x + 80.f, 460.f), h = ts.y + small * 4.f + 70.f;
    const ImVec2 p0{c.x - w * 0.5f, c.y - h * 0.5f}, p1{c.x + w * 0.5f, c.y + h * 0.5f};
    const ImU32 ink = IM_COL32(28, 22, 16, 255), paper = IM_COL32(232, 222, 196, 255);
    dl->AddRectFilled({p0.x + 8, p0.y + 10}, {p1.x + 8, p1.y + 10}, IM_COL32(0, 0, 0, 110));
    dl->AddRectFilled(p0, p1, paper);
    dl->AddRect({p0.x + 6, p0.y + 6}, {p1.x - 6, p1.y - 6}, ink, 0.f, 1.f);
    const char* mast = "THE LOST HEAVEN HERALD";
    const ImVec2 ms = font->CalcTextSizeA(small * 1.3f, 1e9f, 0, mast);
    dl->AddText(font, small * 1.3f, {c.x - ms.x * 0.5f, p0.y + 16}, ink, mast);
    const float rule = p0.y + 22 + ms.y;
    dl->AddLine({p0.x + 16, rule}, {p1.x - 16, rule}, ink, 2.f);
    dl->AddLine({p0.x + 16, rule + 4}, {p1.x - 16, rule + 4}, ink, 1.f);
    dl->AddText(font, small * 0.8f, {p0.x + 18, rule + 8}, ink, "EXTRA!   EVENING EDITION   1930   FIVE CENTS");
    dl->AddText(font, big, {c.x - ts.x * 0.5f, rule + small + 16}, ink, text.c_str());
    const float colsY = rule + small + 26 + ts.y;
    for (int col = 0; col < 3; ++col) {
      const float x0 = p0.x + 18 + (w - 36) * static_cast<float>(col) / 3.f, x1 = x0 + (w - 36) / 3.f - 12;
      for (float y = colsY; y < p1.y - 14; y += 6) dl->AddLine({x0, y}, {x1 - static_cast<float>(static_cast<int>(y * 7) % 23), y}, IM_COL32(60, 50, 40, 140), 1.f);
    }
  }
  std::unordered_map<const Feature*, bool> seen_;
  bool primed_ = false, hasPos_ = false;
  game::Vec3 lastPos_{};
  uint64_t lastUpdate_ = 0;
  std::deque<std::string> queue_;
  float age_ = 0;
  std::minstd_rand rng_{static_cast<uint32_t>(util::NowMs())};
};

// ---- Art-deco speedometer ----------------------------------------------------------------------------
class DecoSpeedo : public Feature {
 public:
  DecoSpeedo() : Feature("fun.deco_speedo", "Art-deco speedometer", Category::Fun, Kind::Toggle,
                         "Brass analog gauge with sunburst face and a persistent odometer.") {}
  void Tick(float dt) override {
    const auto s = game::state::Get();
    const float v = s.valid ? s.speedMs : 0.f;
    if (s.valid && v < 120.f) odoM_ += static_cast<double>(v) * dt;   // ignore teleport spikes
    shown_ += (v - shown_) * std::min(1.f, dt * 6.f);
  }
  void DrawOverlay() override {
    const auto& pal = th::Colors();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 sz = Screen();
    const float r = radius_;
    const ImVec2 c{sz.x - r - 40.f, sz.y - r - 50.f};
    const float a0 = 0.75f * kPi, a1 = 2.25f * kPi;
    const float unit = mph_ ? 2.23694f : 3.6f;
    auto at = [&](float ang, float rr) { return ImVec2{c.x + std::cos(ang) * rr, c.y + std::sin(ang) * rr}; };
    dl->AddCircleFilled(c, r + 10, th::U32(pal.bg, 0.9f), 72);
    dl->AddCircle(c, r + 10, th::U32(pal.accent), 72, 3.f);
    dl->AddCircle(c, r + 4, th::U32(pal.accentDim), 72, 1.f);
    for (int i = 0; i <= 32; ++i) {   // sunburst
      const float ang = a0 + (a1 - a0) * static_cast<float>(i) / 32.f;
      dl->AddLine(at(ang, r * 0.2f), at(ang, r * 0.68f), th::U32(pal.accentDim, i % 2 ? 0.18f : 0.32f), i % 2 ? 1.f : 2.f);
    }
    dl->PathArcTo(c, r * 0.88f, a0, a1, 72);
    dl->PathStroke(th::U32(pal.accentDim), 2.f);
    dl->PathArcTo(c, r * 0.88f, a0 + (a1 - a0) * 0.82f, a1, 24);
    dl->PathStroke(th::U32(pal.danger), 5.f);
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * 0.9f;
    for (int v = 0; v <= max_; v += 10) {
      const float ang = a0 + (a1 - a0) * static_cast<float>(v) / static_cast<float>(max_);
      const bool major = v % 20 == 0;
      dl->AddLine(at(ang, r * (major ? 0.74f : 0.8f)), at(ang, r * 0.92f), th::U32(pal.text, major ? 1.f : 0.6f), major ? 2.5f : 1.2f);
      if (!major) continue;
      char lb[8];
      std::snprintf(lb, sizeof(lb), "%d", v);
      const ImVec2 ls = font->CalcTextSizeA(fs, 1e9f, 0, lb), lp = at(ang, r * 0.6f);
      dl->AddText(font, fs, {lp.x - ls.x * 0.5f, lp.y - ls.y * 0.5f}, th::U32(pal.accent), lb);
    }
    const char* units = mph_ ? "MPH" : "KM/H";
    const ImVec2 us = font->CalcTextSizeA(fs, 1e9f, 0, units);
    dl->AddText(font, fs, {c.x - us.x * 0.5f, c.y - r * 0.38f}, th::U32(pal.textDim), units);
    // Odometer drum.
    char odo[24];
    std::snprintf(odo, sizeof(odo), "%07.1f", odoM_ / (mph_ ? 1609.344 : 1000.0));
    const ImVec2 os = font->CalcTextSizeA(fs * 1.1f, 1e9f, 0, odo), op{c.x - os.x * 0.5f, c.y + r * 0.32f};
    dl->AddRectFilled({op.x - 6, op.y - 3}, {op.x + os.x + 6, op.y + os.y + 3}, IM_COL32(12, 10, 8, 255), 3.f);
    dl->AddRect({op.x - 6, op.y - 3}, {op.x + os.x + 6, op.y + os.y + 3}, th::U32(pal.accentDim), 3.f);
    dl->AddText(font, fs * 1.1f, op, th::U32(pal.text), odo);
    // Needle.
    const float frac = std::clamp(shown_ * unit / static_cast<float>(max_), 0.f, 1.f);
    const float ang = a0 + (a1 - a0) * frac;
    const ImVec2 dir{std::cos(ang), std::sin(ang)}, perp{-dir.y, dir.x};
    const ImVec2 tip = at(ang, r * 0.86f), back{c.x - dir.x * r * 0.14f, c.y - dir.y * r * 0.14f};
    dl->AddTriangleFilled(tip, {back.x + perp.x * 5, back.y + perp.y * 5}, {back.x - perp.x * 5, back.y - perp.y * 5}, th::U32(pal.danger));
    dl->AddCircleFilled(c, 9, th::U32(pal.accent), 24);
    dl->AddCircleFilled(c, 4, th::U32(pal.bg), 12);
  }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    ImGui::SliderFloat("Size", &radius_, 70.f, 220.f, "%.0f px");
    ImGui::SliderInt("Scale max", &max_, 60, 300);
    ImGui::Checkbox("Miles per hour", &mph_);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset odometer")) odoM_ = 0;
  }
  void SaveExtra(nlohmann::json& j) const override { j = {{"radius", radius_}, {"max", max_}, {"mph", mph_}, {"odometer_m", odoM_}}; }
  void LoadExtra(const nlohmann::json& j) override {
    radius_ = std::clamp(j.value("radius", radius_), 70.f, 220.f);
    max_ = std::clamp(j.value("max", max_) / 10 * 10, 60, 300);
    mph_ = j.value("mph", mph_);
    odoM_ = std::max(0.0, j.value("odometer_m", odoM_));
  }

 private:
  float radius_ = 120.f, shown_ = 0;
  int max_ = 160;
  bool mph_ = false;
  double odoM_ = 0;
};

// ---- Rewind --------------------------------------------------------------------------------------------
// The tape (a Panel, so it ticks and draws always) records; the Rewind action (hotkey) plays it back.
std::string SetPosChunk(const game::Vec3& p) {
  return "local o = CG.need(CG.target())\nCG.must(CG.call(o, \"SetPos\", CG.need(CG.vec(" + LuaNumber(p.x) + ", " + LuaNumber(p.y) + ", " +
         LuaNumber(p.z) + "))))";
}

class RewindTape : public Feature {
 public:
  static constexpr float kStep = 0.25f, kSeconds = 30.f, kPlay = 1.5f;
  RewindTape() : Feature("fun.rewind_tape", "Rewind tape", Category::Fun, Kind::Panel, "Records the last 30 s of your path for Rewind.") {}
  std::vector<std::string> Requires() const override { return {"vm"}; }
  bool Start() {
    if (playing_ || ring_.size() < 2) return false;
    path_.assign(ring_.rbegin(), ring_.rend());
    ring_.clear();
    playing_ = true;
    playT_ = 0;
    lastIdx_ = -1;
    return true;
  }
  void Tick(float dt) override {
    if (playing_) {
      playT_ += dt;
      const float f = std::min(playT_ / kPlay, 1.f);
      const int idx = static_cast<int>(f * static_cast<float>(path_.size() - 1));
      if (idx != lastIdx_ && runner_.Run(SetPosChunk(path_[static_cast<size_t>(idx)]), "Rewind", {}, idx == static_cast<int>(path_.size()) - 1))
        lastIdx_ = idx;
      if (f >= 1.f && lastIdx_ == static_cast<int>(path_.size()) - 1) playing_ = false;
      if (playT_ > kPlay + 3.f) playing_ = false;   // safety: VM stalled
      return;
    }
    acc_ += dt;
    if (acc_ < kStep) return;
    acc_ = 0;
    const auto s = game::state::Get();
    if (!s.valid) return;
    ring_.push_back(s.inVehicle ? s.vehiclePos : s.playerPos);
    while (ring_.size() > static_cast<size_t>(kSeconds / kStep)) ring_.pop_front();
  }
  void DrawOverlay() override {
    if (!playing_) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 sz = Screen();
    const auto& pal = th::Colors();
    Lcg rng(render::FrameCount());
    dl->AddRectFilled({0, 0}, sz, Rgba(90, 60, 30, 0.25f));
    Vignette(dl, sz, 0.8f);
    for (int i = 0; i < 6; ++i) {   // tracking bands rolling upwards
      const float y = std::fmod(sz.y - playT_ * 900.f + static_cast<float>(i) * sz.y / 6.f + sz.y * 4.f, sz.y);
      dl->AddRectFilled({0, y}, {sz.x, y + 3.f + rng.Next() * 6.f}, Rgba(255, 240, 210, 0.12f));
    }
    const ImVec2 c{sz.x * 0.5f, sz.y * 0.18f};
    const ImU32 col = th::U32(pal.accent, 0.55f + 0.45f * std::fabs(std::sin(playT_ * 10.f)));
    for (int k = 0; k < 2; ++k) {
      const float x = c.x - 60.f + static_cast<float>(k) * 34.f;
      dl->AddTriangleFilled({x, c.y}, {x + 34.f, c.y - 22.f}, {x + 34.f, c.y + 22.f}, col);
    }
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * 2.f;
    dl->AddText(font, fs, {c.x + 26.f, c.y - fs * 0.5f}, col, "REWINDING");
    const float f = std::min(playT_ / kPlay, 1.f);
    dl->AddRectFilled({c.x - 140.f, c.y + 34.f}, {c.x + 140.f, c.y + 40.f}, th::U32(pal.bg, 0.8f));
    dl->AddRectFilled({c.x + 140.f - 280.f * f, c.y + 34.f}, {c.x + 140.f, c.y + 40.f}, col);
  }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    ImGui::Text("Tape: %.1f s of %.0f s recorded", static_cast<float>(ring_.size()) * kStep, kSeconds);
    ImGui::TextDisabled("Bind a key on the Rewind action to jump back.");
  }

 private:
  std::deque<game::Vec3> ring_;
  std::vector<game::Vec3> path_;
  Runner runner_;
  bool playing_ = false;
  float acc_ = 0, playT_ = 0;
  int lastIdx_ = -1;
};

class Rewind : public Feature {
 public:
  explicit Rewind(RewindTape* tape)
      : Feature("fun.rewind", "Rewind", Category::Fun, Kind::Action, "Winds you back along the last 30 s of your path in 1.5 s."), tape_(tape) {
    Key() = Hotkey{0x78 /*F9*/};
  }
  std::vector<std::string> Requires() const override { return {"vm"}; }
  void Activate() override {
    if (!tape_ || !tape_->Start()) nt::Push(nt::Kind::Info, "Rewind", "Nothing on the tape yet");
  }

 private:
  RewindTape* tape_;
};

// ---- Chaos mode --------------------------------------------------------------------------------------
class Chaos : public Feature {
 public:
  Chaos() : Feature("fun.chaos", "Chaos mode", Category::Fun, Kind::Toggle,
                    "Every N seconds a random effect hits Lost Heaven: police, time jumps, weather, disco paint, invisibility, launches, boosts.") {}
  std::vector<std::string> Requires() const override { return {"vm"}; }
  void OnEnable() override {
    next_ = static_cast<float>(interval_);
    active_.clear();
    remaining_ = 0;
    pending_.clear();
  }
  void OnDisable() override {
    if (!revert_.empty()) RunFeatureChunk(revert_, "Chaos revert", {}, false);
    revert_.clear();
    active_.clear();
  }
  void Tick(float dt) override {
    if (remaining_ > 0) {
      remaining_ -= dt;
      if (disco_ && (discoT_ += dt) > 0.4f) {
        discoT_ = 0;
        if (pending_.empty()) pending_.push_back(DiscoChunk());
      }
      if (remaining_ <= 0) {
        if (!revert_.empty()) pending_.push_back(revert_);
        revert_.clear();
        disco_ = false;
        active_.clear();
      }
    }
    next_ -= dt;
    if (next_ <= 0) {
      next_ = static_cast<float>(interval_);
      Fire();
    }
    if (!pending_.empty() && runner_.Run(pending_.front(), "Chaos mode", {}, false)) pending_.pop_front();
  }
  void DrawOverlay() override {
    const auto& pal = th::Colors();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 sz = Screen();
    const float w = 320.f, x0 = (sz.x - w) * 0.5f, y0 = 18.f;
    const float f = std::clamp(next_ / static_cast<float>(interval_), 0.f, 1.f);
    dl->AddRectFilled({x0 - 8, y0 - 6}, {x0 + w + 8, y0 + 44}, th::U32(pal.bg, 0.8f), 6.f);
    dl->AddRectFilled({x0, y0 + 30}, {x0 + w, y0 + 36}, th::U32(pal.panel), 3.f);
    dl->AddRectFilled({x0, y0 + 30}, {x0 + w * f, y0 + 36}, th::U32(next_ < 5.f ? pal.danger : pal.accent), 3.f);
    char buf[128];
    if (!active_.empty() && remaining_ > 0) std::snprintf(buf, sizeof(buf), "CHAOS: %s  (%.0f s)", active_.c_str(), remaining_);
    else if (!active_.empty()) std::snprintf(buf, sizeof(buf), "CHAOS: %s", active_.c_str());
    else std::snprintf(buf, sizeof(buf), "CHAOS in %.0f s", next_);
    dl->AddText({x0, y0}, th::U32(pal.accent), buf);
    if (!active_.empty() && remaining_ <= 0 && (shownT_ += ImGui::GetIO().DeltaTime) > 4.f) {
      active_.clear();
      shownT_ = 0;
    }
  }
  bool HasSettings() const override { return true; }
  void DrawSettings() override {
    ImGui::SliderInt("Interval (s)", &interval_, 5, 300);
    if (ImGui::SmallButton("Trigger now")) Fire();
  }
  void SaveExtra(nlohmann::json& j) const override { j = {{"interval", interval_}}; }
  void LoadExtra(const nlohmann::json& j) override { interval_ = std::clamp(j.value("interval", interval_), 5, 300); }

 private:
  std::string DiscoChunk() {
    // Palette IDs 1-42 (the game's paint takes IDs, not RGB).
    const int c1 = static_cast<int>(rng_() % 42) + 1, c2 = static_cast<int>(rng_() % 42) + 1;
    return "local v = CG.vehicle()\nif not v then return end\nCG.call(v, \"SetColor\", " + LuaNumber(c1) + ", " + LuaNumber(c2) + ")";
  }
  void Fire() {
    if (!revert_.empty()) pending_.push_back(revert_);
    revert_.clear();
    disco_ = false;
    remaining_ = 0;
    shownT_ = 0;
    struct Effect {
      const char* name;
      std::string code, revert;
      float seconds;
      bool disco;
    };
    const auto s = game::state::Get();
    std::vector<Effect> pool = {
        {"THE COPS ARE ON TO YOU", "CG.must(CG.call(CG.get(\"game\", \"police\"), \"Enable\"))", {}, 0, false},
        {"TIME JUMP +6 HOURS",
         "local g = CG.get(\"game\", \"gfx\")\nlocal ok, t = CG.call(g, \"GetTime\")\nif not ok or type(t) ~= \"number\" then t = " +
             LuaNumber(s.timeOfDay.value_or(12.f)) + " end\nCG.must(CG.call(g, \"SetTime\", (t + 6) % 24, 0))",
         {}, 0, false},
        {"INVISIBLE MAN", "CG.call(CG.need(CG.player()), \"ShowModel\", false)", "CG.call(CG.need(CG.player()), \"ShowModel\", true)", 10.f, false},
        {"UP, UP AND AWAY",
         "local o = CG.need(CG.target())\nlocal p = CG.must(CG.call(o, \"GetPos\"))\nCG.must(CG.call(o, \"SetPos\", CG.need(CG.vec(p.x + " +
             LuaNumber(static_cast<int>(rng_() % 21) - 10) + ", p.y + " + LuaNumber(static_cast<int>(rng_() % 21) - 10) + ", p.z + 10))))",
         {}, 0, false},
        {"TRAFFIC JAM", "CG.must(CG.call(CG.get(\"game\", \"traffic\"), \"Populate\", 60))", {}, 0, false},
    };
    if (s.inVehicle) {
      pool.push_back({"DISCO PAINT", DiscoChunk(), {}, 10.f, true});
      pool.push_back({"SUPER BOOST", "local v = CG.vehicle()\nif v then CG.must(CG.call(v, \"SetSpeed\", 80)) end", {}, 0, false});
    }
    const auto favs = StrList(Config::Get().ReadJson("features.world.weather.settings"), "favourites");
    if (!favs.empty())
      pool.push_back({"STORM FRONT ROLLS IN",
                      "CG.must(CG.call(CG.get(\"game\", \"gfx\"), \"SetWeatherSet\", " + LuaQuote(favs[rng_() % favs.size()]) + ", 0, 0))", {}, 0,
                      false});
    const Effect& e = pool[rng_() % pool.size()];
    active_ = e.name;
    pending_.push_back(e.code);
    revert_ = e.revert;
    remaining_ = e.seconds;
    disco_ = e.disco;
    discoT_ = 0;
    while (pending_.size() > 4) pending_.pop_front();
  }

  Runner runner_;
  std::deque<std::string> pending_;
  std::string active_, revert_;
  float next_ = 30.f, remaining_ = 0, discoT_ = 0, shownT_ = 0;
  bool disco_ = false;
  int interval_ = 30;
  std::minstd_rand rng_{static_cast<uint32_t>(util::NowMs() ^ 0x5A17u)};
};

}  // namespace

void RegisterFunFeatures(Registry& r) {
  r.Add(std::make_unique<FilmReel>());
  r.Add(std::make_unique<Headlines>());
  r.Add(std::make_unique<DecoSpeedo>());
  auto* tape = static_cast<RewindTape*>(r.Add(std::make_unique<RewindTape>()));
  r.Add(std::make_unique<Rewind>(tape));
  r.Add(std::make_unique<Chaos>());
}

}  // namespace cg::features
