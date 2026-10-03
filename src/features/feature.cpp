#include "features/feature.h"

#include <atomic>
#include <exception>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/hotkeys.h"
#include "core/log.h"
#include "core/mp_guard.h"
#include "core/util.h"
#include "game/bindings.h"
#include "game/script_vm.h"
#include "render/fonts.h"
#include "ui/notify.h"

namespace cg::features {

namespace detail {
// Defined in cheat_table.cpp: bindings generation the cheat table was last built from.
uint64_t CheatTableSyncedGeneration();
}  // namespace detail

namespace {

constexpr std::string_view kChannel = "features";
// Pseudo-requirements. "vm": the game's script VM must be ready; the feature is script-backed and
// its OnEnable re-runs whenever the VM is recreated. "vm?": documents an optional use; never blocks.
constexpr std::string_view kReqVm = "vm";
constexpr std::string_view kReqVmOptional = "vm?";
constexpr uint64_t kAutosaveMs = 1000;
constexpr size_t kMaxIdLength = 200;

struct FeatureState {
  bool desired = false;       // the user's choice; persisted. DisableAll/Remove leave it untouched.
  bool applied = false;       // OnEnable ran and OnDisable has not run since
  bool available = false;     // as of the last evaluation
  bool scriptBacked = false;  // Requires() contains "vm"
  bool faulted = false;       // Tick/DrawOverlay threw; skipped until re-enabled or reloaded
  uint64_t appliedVmGen = 0;  // vm::Generation() at the last OnEnable
  Hotkey defaultKey;          // hotkey the feature was registered with (persisted only when changed)
};

// Registry bookkeeping. The Registry is a leaked singleton touched only from the render thread
// (and the init thread before rendering starts), so this needs no locking.
struct Shared {
  std::unordered_map<const Feature*, FeatureState> states;
  // Removed features live one more frame: a feature may remove itself from inside its own callback.
  std::vector<std::unique_ptr<Feature>> graveyard;
  bool loaded = false;
  bool builtinsRegistered = false;
  bool listenerAdded = false;
  bool ticking = false;
  uint64_t lastAutosaveMs = 0;
};

Shared& S() {
  static Shared* s = new Shared();
  return *s;
}

std::atomic<bool> g_reloadRequested{false};

void OnConfigReloaded() { g_reloadRequested.store(true); }

// enabled_ is protected; this grants the registry the same access without widening the contract.
struct EnabledAccess : Feature {
  static bool& Of(Feature& f) { return f.*(&EnabledAccess::enabled_); }
};

FeatureState* StateOf(const Feature* f) {
  auto& states = S().states;
  const auto it = states.find(f);
  return it == states.end() ? nullptr : &it->second;
}

std::string CfgKey(const Feature& f, std::string_view leaf) {
  std::string k = "features.";
  k += f.Id();
  k += '.';
  k += leaf;
  return k;
}

void Toast(ui::notify::Kind kind, std::string title, std::string text, float seconds = 3.5f) {
  try {
    ui::notify::Push(kind, std::move(title), std::move(text), seconds);
  } catch (...) {
  }
}

void ReportThrow(const Feature& f, const char* what, const char* msg) {
  try {
    log::Error(kChannel, "feature '{}' threw in {}: {}", f.Id(), what, msg);
    Toast(ui::notify::Kind::Error, f.Name() + " failed", std::string(what) + ": " + msg, 6.0f);
  } catch (...) {
  }
}

// Feature code comes from many modules (and scripts); nothing it throws may escape into the Present hook.
template <class Fn> bool Guard(const Feature& f, const char* what, Fn&& fn) noexcept {
  try {
    fn();
    return true;
  } catch (const std::exception& e) {
    ReportThrow(f, what, e.what());
  } catch (...) {
    ReportThrow(f, what, "unknown exception");
  }
  return false;
}

struct Availability {
  bool ok = true;
  bool scriptBacked = false;
};

Availability Evaluate(const Feature& f, std::string* reason) {
  Availability a;
  std::vector<std::string> req;
  try {
    req = f.Requires();
  } catch (...) {
    a.ok = false;
    if (reason) *reason = "Requires() threw an exception";
    return a;
  }
  const bool blocked = mp_guard::Blocked();
  bool needVm = false;
  std::vector<std::string_view> missing;
  const auto& bindings = game::Bindings::Get();
  for (const std::string& r : req) {
    if (r.empty() || r == kReqVmOptional) continue;
    if (r == kReqVm) {
      needVm = a.scriptBacked = true;
      continue;
    }
    if (bindings.Has(r)) continue;
    bool seen = false;
    for (std::string_view m : missing) seen = seen || m == r;
    if (!seen) missing.push_back(r);
  }
  const bool vmReady = !needVm || game::vm::Ready();
  a.ok = !blocked && missing.empty() && vmReady;
  if (!reason || a.ok) return a;

  if (blocked) {
    const std::string& why = mp_guard::Reason();
    *reason = why.empty() ? "blocked: multiplayer detected" : "blocked: multiplayer detected (" + why + ")";
    return a;
  }
  std::string out;
  if (!missing.empty()) {
    out = "needs ";
    for (size_t i = 0; i < missing.size(); ++i) {
      if (i) out += ", ";
      out += missing[i];
    }
  }
  if (!vmReady) {
    if (!out.empty()) out += "; ";
    const char* status = game::vm::StatusText();
    out += "game script VM not ready: ";
    out += status ? status : "unknown state";
  }
  *reason = std::move(out);
  return a;
}

// Runs OnEnable. A feature rejects the enable by clearing enabled_ inside OnEnable (it reports why
// itself); a throw is reported here and rolled back with OnDisable. Returns true if it stayed on.
bool DoEnable(Feature& f, FeatureState* st) {
  bool& enabled = EnabledAccess::Of(f);
  enabled = true;
  const bool ok = Guard(f, "OnEnable", [&f] { f.OnEnable(); });
  if (!ok) Guard(f, "OnDisable", [&f] { f.OnDisable(); });
  if (!ok || !enabled) {
    enabled = false;
    if (st) st->applied = false;
    return false;
  }
  if (st) {
    st->applied = true;
    st->available = true;
    st->faulted = false;
    st->appliedVmGen = game::vm::Generation();
  }
  return true;
}

// Script-backed features skip OnDisable while the VM is down: their state died with it and the
// chunk could only fail.
void DoDisable(Feature& f, FeatureState* st, bool scriptBacked) {
  bool& enabled = EnabledAccess::Of(f);
  const bool applied = st ? st->applied : enabled;
  enabled = false;
  if (st) st->applied = false;
  if (applied && !(scriptBacked && !game::vm::Ready())) Guard(f, "OnDisable", [&f] { f.OnDisable(); });
}

bool ValidId(const std::string& id) {
  if (id.empty() || id.size() > kMaxIdLength || id.front() == '.' || id.back() == '.') return false;
  char prev = 0;
  for (char c : id) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) return false;
    if (c == '.' && prev == '.') return false;   // Config keys are dotted paths: no empty segments
    prev = c;
  }
  return true;
}

void LoadOne(Feature& f, FeatureState& st) {
  auto& cfg = Config::Get();
  const std::string hk = cfg.ReadString(CfgKey(f, "hotkey"), "");
  if (hk.empty()) {
    f.Key() = st.defaultKey;
  } else {
    const Hotkey parsed = Hotkey::Parse(hk);
    if (parsed.Valid() || util::IEquals(util::Trim(hk), "unbound") || util::IEquals(util::Trim(hk), "none")) {
      f.Key() = parsed;
    } else {
      log::Warn(kChannel, "feature '{}': saved hotkey '{}' is not a key; using the default", f.Id(), hk);
      f.Key() = st.defaultKey;
    }
  }
  f.favorite = cfg.ReadBool(CfgKey(f, "favorite"), false);
  const nlohmann::json extra = cfg.ReadJson(CfgKey(f, "settings"));
  if (!extra.is_null()) Guard(f, "LoadExtra", [&] { f.LoadExtra(extra); });
  st.faulted = false;

  if (f.GetKind() != Kind::Toggle) return;
  st.desired = cfg.ReadBool(CfgKey(f, "enabled"), false);
  const Availability a = Evaluate(f, nullptr);
  st.available = a.ok;
  st.scriptBacked = a.scriptBacked;
  if (st.desired && !f.Enabled()) {
    if (a.ok) DoEnable(f, &st);
    else EnabledAccess::Of(f) = true;   // pending: OnEnable runs when it becomes available
  } else if (!st.desired && f.Enabled()) {
    DoDisable(f, &st, st.scriptBacked);
  }
}

void SaveOne(const Feature& f, const FeatureState& st) {
  auto& cfg = Config::Get();
  if (f.GetKind() == Kind::Toggle) cfg.WriteBool(CfgKey(f, "enabled"), st.desired);
  if (f.Key() == st.defaultKey) cfg.Erase(CfgKey(f, "hotkey"));
  else cfg.WriteString(CfgKey(f, "hotkey"), f.Key().ToString());
  if (f.favorite) cfg.WriteBool(CfgKey(f, "favorite"), true);
  else cfg.Erase(CfgKey(f, "favorite"));
  nlohmann::json extra = nlohmann::json::object();
  if (Guard(f, "SaveExtra", [&] { f.SaveExtra(extra); })) {
    if (extra.is_null() || (extra.is_object() && extra.empty())) cfg.Erase(CfgKey(f, "settings"));
    else cfg.WriteJson(CfgKey(f, "settings"), extra);
  }
}

}  // namespace

// ---- categories -------------------------------------------------------------------------------------

const char* CategoryName(Category c) {
  switch (c) {
    case Category::Player: return "Player";
    case Category::Vehicle: return "Vehicle";
    case Category::Weapons: return "Weapons";
    case Category::World: return "World";
    case Category::Teleport: return "Teleport";
    case Category::Camera: return "Camera";
    case Category::Visuals: return "Visuals";
    case Category::CheatTable: return "Cheat Table";
    case Category::Scripts: return "Scripts";
    case Category::Count: break;
  }
  return "Unknown";
}

const char* CategoryIcon(Category c) {
  switch (c) {
    case Category::Player: return CG_ICON_PLAYER;
    case Category::Vehicle: return CG_ICON_CAR;
    case Category::Weapons: return CG_ICON_WEAPON;
    case Category::World: return CG_ICON_WORLD;
    case Category::Teleport: return CG_ICON_TELEPORT;
    case Category::Camera: return CG_ICON_CAMERA;
    case Category::Visuals: return CG_ICON_VISUALS;
    case Category::CheatTable: return CG_ICON_TABLE;
    case Category::Scripts: return CG_ICON_SCRIPT;
    case Category::Count: break;
  }
  return CG_ICON_INFO;
}

// ---- Feature ----------------------------------------------------------------------------------------

Feature::Feature(std::string id, std::string name, Category category, Kind kind, std::string description)
    : id_(std::move(id)), name_(std::move(name)), description_(std::move(description)), category_(category), kind_(kind) {}

bool Feature::Available() const { return Evaluate(*this, nullptr).ok; }

std::string Feature::UnavailableReason() const {
  std::string reason;
  if (Evaluate(*this, &reason).ok) return {};
  return reason;
}

void Feature::SetEnabled(bool on) {
  if (kind_ != Kind::Toggle || on == enabled_) return;
  FeatureState* st = StateOf(this);
  if (on) {
    std::string reason;
    const Availability a = Evaluate(*this, &reason);
    if (!a.ok) {
      Toast(ui::notify::Kind::Warning, name_ + " unavailable", reason, 5.0f);
      return;
    }
    if (st) st->scriptBacked = a.scriptBacked;
    if (!DoEnable(*this, st)) {
      log::Info(kChannel, "feature '{}' did not enable", id_);
      return;
    }
  } else {
    const bool scriptBacked = st ? st->scriptBacked : Evaluate(*this, nullptr).scriptBacked;
    DoDisable(*this, st, scriptBacked);
  }
  if (st) st->desired = on;
  Config::Get().WriteBool(CfgKey(*this, "enabled"), on);
  Toast(on ? ui::notify::Kind::Success : ui::notify::Kind::Info, name_, on ? "Enabled" : "Disabled", 1.8f);
}

void Feature::Trigger() {
  switch (kind_) {
    case Kind::Toggle:
      SetEnabled(!enabled_);
      break;
    case Kind::Action: {
      std::string reason;
      if (!Evaluate(*this, &reason).ok) {
        Toast(ui::notify::Kind::Warning, name_ + " unavailable", reason, 5.0f);
        return;
      }
      Guard(*this, "Activate", [this] { Activate(); });
      break;
    }
    case Kind::Panel:
      break;
  }
}

// ---- Registry ---------------------------------------------------------------------------------------

Registry& Registry::Get() {
  // Leaked on purpose: features must never be destroyed during static destruction inside the game.
  static Registry* r = new Registry();
  return *r;
}

Feature* Registry::Add(std::unique_ptr<Feature> f) {
  if (!f) {
    log::Error(kChannel, "Registry::Add called with a null feature");
    return nullptr;
  }
  if (!ValidId(f->Id())) {
    log::Error(kChannel, "feature id '{}' is invalid (empty, too long, control characters or an empty '.' segment)",
               f->Id());
    return nullptr;
  }
  if (Find(f->Id())) {
    log::Error(kChannel, "duplicate feature id '{}' - the second registration was ignored", f->Id());
    return nullptr;
  }
  Feature* raw = f.get();
  FeatureState st;
  st.defaultKey = raw->Key();
  st.desired = raw->Enabled();   // arrived switched on: treated as pending until Tick applies it
  const Availability a = Evaluate(*raw, nullptr);
  st.available = a.ok;
  st.scriptBacked = a.scriptBacked;
  try {
    features_.push_back(std::move(f));
  } catch (...) {
    log::Error(kChannel, "out of memory registering feature '{}'", raw->Id());
    return nullptr;
  }
  try {
    FeatureState& stored = S().states.insert_or_assign(raw, st).first->second;
    if (S().loaded) LoadOne(*raw, stored);
  } catch (...) {
    S().states.erase(raw);
    features_.pop_back();
    log::Error(kChannel, "out of memory registering a feature");
    return nullptr;
  }
  return raw;
}

bool Registry::Remove(const std::string& id) {
  for (size_t i = 0; i < features_.size(); ++i) {
    Feature* f = features_[i].get();
    if (f->Id() != id) continue;
    FeatureState* st = StateOf(f);
    if (f->GetKind() == Kind::Toggle && f->Enabled()) DoDisable(*f, st, st && st->scriptBacked);
    std::unique_ptr<Feature> owned = std::move(features_[i]);
    features_.erase(features_.begin() + static_cast<std::ptrdiff_t>(i));
    S().states.erase(f);
    try {
      S().graveyard.push_back(std::move(owned));
    } catch (...) {
      // Out of memory: destroy now. Only unsafe if the feature is removing itself from a callback.
    }
    return true;
  }
  return false;
}

Feature* Registry::Find(const std::string& id) const {
  for (const auto& f : features_)
    if (f->Id() == id) return f.get();
  return nullptr;
}

std::vector<Feature*> Registry::InCategory(Category c) const {
  std::vector<Feature*> out;
  for (const auto& f : features_)
    if (f->GetCategory() == c) out.push_back(f.get());
  return out;
}

void Registry::Tick(float dt) {
  Shared& s = S();
  if (s.ticking) return;   // a feature calling back into Tick
  s.ticking = true;
  try {
    s.graveyard.clear();
    if (g_reloadRequested.exchange(false)) Load();
    if (game::Bindings::Get().Generation() != detail::CheatTableSyncedGeneration()) SyncCheatTableFeatures(*this);

    const uint64_t vmGen = game::vm::Generation();
    std::vector<Feature*> snapshot;
    snapshot.reserve(features_.size());
    for (const auto& f : features_) snapshot.push_back(f.get());

    for (Feature* f : snapshot) {
      FeatureState* st = StateOf(f);
      if (!st) continue;   // removed earlier this frame (object kept alive in the graveyard)
      const Availability a = Evaluate(*f, nullptr);
      const bool rising = a.ok && !st->available;
      st->available = a.ok;
      st->scriptBacked = a.scriptBacked;

      if (f->Key().Valid() && hotkeys::Pressed(f->Key())) {
        Guard(*f, "hotkey", [f] { f->Trigger(); });
        st = StateOf(f);
        if (!st) continue;
      }
      if (st->faulted || !a.ok) continue;

      switch (f->GetKind()) {
        case Kind::Toggle: {
          if (!f->Enabled()) break;
          // Pending enable (restored from config or waiting for bindings), or a script-backed
          // toggle whose VM state was wiped (save loaded, VM reset): (re)run OnEnable.
          if (!st->applied || (st->scriptBacked && (rising || st->appliedVmGen != vmGen))) {
            if (!DoEnable(*f, st)) break;
          }
          if (!Guard(*f, "Tick", [f, dt] { f->Tick(dt); })) {
            DoDisable(*f, st, st->scriptBacked);
            st->faulted = true;
          }
          break;
        }
        case Kind::Panel:
          if (!Guard(*f, "Tick", [f, dt] { f->Tick(dt); })) st->faulted = true;
          break;
        case Kind::Action:
          break;
      }
    }

    const uint64_t now = util::NowMs();
    if (s.loaded && now - s.lastAutosaveMs >= kAutosaveMs && !g_reloadRequested.load()) {
      s.lastAutosaveMs = now;
      Save();
    }
  } catch (const std::exception& e) {
    log::Error(kChannel, "Registry::Tick: {}", e.what());
  } catch (...) {
    log::Error(kChannel, "Registry::Tick: unknown exception");
  }
  s.ticking = false;
}

void Registry::DrawOverlays() {
  try {
    std::vector<Feature*> snapshot;
    snapshot.reserve(features_.size());
    for (const auto& f : features_) snapshot.push_back(f.get());
    for (Feature* f : snapshot) {
      FeatureState* st = StateOf(f);
      if (!st || st->faulted || !st->available) continue;
      if (f->GetKind() == Kind::Toggle) {
        if (!f->Enabled() || !st->applied) continue;
        if (!Guard(*f, "DrawOverlay", [f] { f->DrawOverlay(); })) {
          DoDisable(*f, st, st->scriptBacked);
          st->faulted = true;
        }
      } else if (f->GetKind() == Kind::Panel) {
        if (!Guard(*f, "DrawOverlay", [f] { f->DrawOverlay(); })) st->faulted = true;
      }
    }
  } catch (...) {
    log::Error(kChannel, "Registry::DrawOverlays failed");
  }
}

void Registry::DisableAll() {
  size_t count = 0;
  for (const auto& f : features_) {
    if (f->GetKind() != Kind::Toggle || !f->Enabled()) continue;
    FeatureState* st = StateOf(f.get());
    DoDisable(*f, st, st && st->scriptBacked);
    ++count;
  }
  if (count) log::Info(kChannel, "disabled {} feature(s); saved toggles are kept", count);
}

void Registry::Save() const {
  // Before the first Load() the in-memory state is just defaults; saving it would wipe the user's file.
  if (!S().loaded) return;
  for (const auto& f : features_) {
    const FeatureState* st = StateOf(f.get());
    if (!st) continue;
    try {
      SaveOne(*f, *st);
    } catch (...) {
      log::Error(kChannel, "saving feature '{}' failed", f->Id());
    }
  }
}

void Registry::Load() {
  Shared& s = S();
  s.loaded = true;
  if (!s.listenerAdded) {
    s.listenerAdded = true;
    Config::Get().AddReloadListener(&OnConfigReloaded);
  }
  std::vector<Feature*> snapshot;
  snapshot.reserve(features_.size());
  for (const auto& f : features_) snapshot.push_back(f.get());
  size_t on = 0, pending = 0;
  for (Feature* f : snapshot) {
    FeatureState* st = StateOf(f);
    if (!st) continue;
    try {
      LoadOne(*f, *st);
    } catch (...) {
      log::Error(kChannel, "restoring feature '{}' failed", f->Id());
      continue;
    }
    if (f->GetKind() == Kind::Toggle && f->Enabled()) ++(st->applied ? on : pending);
  }
  s.lastAutosaveMs = util::NowMs();
  log::Info(kChannel, "restored {} feature(s): {} enabled, {} waiting for bindings/VM", snapshot.size(), on, pending);
}

void Registry::RegisterBuiltins() {
  Shared& s = S();
  if (s.builtinsRegistered) return;
  s.builtinsRegistered = true;
  using RegisterFn = void (*)(Registry&);
  static constexpr std::pair<const char*, RegisterFn> kModules[] = {
      {"player", &RegisterPlayerFeatures},     {"vehicle", &RegisterVehicleFeatures}, {"weapons", &RegisterWeaponFeatures},
      {"world", &RegisterWorldFeatures},       {"teleport", &RegisterTeleportFeatures},
      {"camera", &RegisterCameraFeatures},     {"visuals", &RegisterVisualFeatures},
  };
  for (const auto& [name, fn] : kModules) {
    const size_t before = features_.size();
    try {
      fn(*this);
    } catch (const std::exception& e) {
      log::Error(kChannel, "registering {} features failed: {}", name, e.what());
    } catch (...) {
      log::Error(kChannel, "registering {} features failed", name);
    }
    log::Debug(kChannel, "{} {} feature(s) registered", features_.size() - before, name);
  }
  log::Info(kChannel, "{} built-in feature(s) registered", features_.size());
}

}  // namespace cg::features
