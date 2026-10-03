// Periodic game-state snapshot: memory facade every frame, script-VM poll as fallback/extra fields.
#include "game/state.h"

#include <atomic>
#include <cmath>
#include <exception>
#include <mutex>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/util.h"
#include "game/game.h"
#include "game/script_vm.h"

namespace cg::game::state {
namespace {

constexpr uint64_t kStaleMs = 2000;
constexpr uint64_t kInFlightTimeoutMs = 5000;
constexpr uint64_t kMinSpeedWindowMs = 50;
constexpr float kMaxSpeedMs = 200.0f;      // faster => teleport / respawn, ignored
constexpr float kSmoothingSeconds = 0.25f;

// Every query is pcall-guarded so one missing method never fails the whole poll.
constexpr const char* kPollChunk =
    "local inV, px, py, pz, vx, vy, vz, t, w = '0', '', '', '', '', '', '', '', '' "
    "local okP, p = pcall(function() return game.game:GetActivePlayer() end)\n"
    "if okP and p then\n"
    "  local okPos, pos = pcall(function() return p:GetPos() end)\n"
    "  if okPos and pos then px, py, pz = tostring(pos.x), tostring(pos.y), tostring(pos.z) end\n"
    "  local okV, v = pcall(function() return p:GetOwner() end)\n"
    "  if okV and v and v ~= p then\n"
    "    local okVp, vp = pcall(function() return v:GetPos() end)\n"
    "    if okVp and vp then inV = '1' vx, vy, vz = tostring(vp.x), tostring(vp.y), tostring(vp.z) end\n"
    "  end\n"
    "end\n"
    "local okT, tt = pcall(function() return game.gfx:GetTime() end)\n"
    "if okT and tt ~= nil then t = tostring(tt) end\n"
    "local okW, ww = pcall(function() return game.gfx:GetCurrentWeatherSetName() end)\n"
    "if okW and ww ~= nil then w = tostring(ww) end\n"
    "return inV, px, py, pz, vx, vy, vz, t, w";

struct ScriptData {
  bool have = false;
  bool inVehicle = false;
  std::optional<Vec3> player;
  std::optional<Vec3> vehicle;
  std::optional<float> time;
  std::string weather;
  uint64_t ms = 0;
};

struct SpeedTracker {
  bool have = false;
  bool inVehicle = false;
  const char* source = nullptr;
  Vec3 last{};
  uint64_t lastMs = 0;
  float speed = 0;
};

std::mutex g_mutex;           // guards g_snap
Snapshot g_snap;

// Render-thread state (Tick and vm callbacks both run there).
ScriptData g_script;
SpeedTracker g_speed;
uint64_t g_lastPollMs = 0;
uint64_t g_inFlightSinceMs = 0;
uint64_t g_lastUpdateMs = 0;
bool g_inFlight = false;
std::atomic<uint32_t> g_pollMs{200};

std::optional<float> ParseFloat(std::string_view s) {
  const std::string t = util::Trim(s);
  if (t.empty() || t == "nil") return std::nullopt;
  const auto d = util::ParseDouble(t);
  if (!d || !std::isfinite(*d) || std::fabs(*d) > 1e9) return std::nullopt;
  return static_cast<float>(*d);
}

std::optional<Vec3> ParseVec(const std::vector<std::string>& v, size_t i) {
  if (v.size() < i + 3) return std::nullopt;
  const auto x = ParseFloat(v[i]), y = ParseFloat(v[i + 1]), z = ParseFloat(v[i + 2]);
  if (!x || !y || !z) return std::nullopt;
  return Vec3{*x, *y, *z};
}

void OnPollResult(const vm::Result& r) {
  g_inFlight = false;
  if (!r.ok || r.values.size() < 9) return;
  ScriptData d;
  d.inVehicle = util::Trim(r.values[0]) == "1";
  d.player = ParseVec(r.values, 1);
  d.vehicle = ParseVec(r.values, 4);
  if (!d.vehicle) d.inVehicle = false;
  d.time = ParseFloat(r.values[7]);
  d.weather = util::Trim(r.values[8]);
  if (d.weather == "nil") d.weather.clear();
  d.have = d.player.has_value() || d.vehicle.has_value() || d.time.has_value() || !d.weather.empty();
  if (!d.have) return;
  d.ms = util::NowMs();
  g_script = std::move(d);
}

void Poll(uint64_t now) {
  const uint32_t interval = g_pollMs.load();
  if (interval == 0) return;
  if (g_inFlight && now - g_inFlightSinceMs > kInFlightTimeoutMs) g_inFlight = false;   // lost callback
  if (g_inFlight || now - g_lastPollMs < interval) return;
  if (!vm::Ready() || !vm::HasReturnValues()) return;
  g_inFlight = true;
  g_inFlightSinceMs = now;
  g_lastPollMs = now;
  vm::Run(kPollChunk, [](const vm::Result& r) {
    try {
      OnPollResult(r);
    } catch (...) {
      g_inFlight = false;
    }
  }, "=cg_state");
}

void UpdateSpeed(const Vec3& pos, uint64_t sampleMs, bool inVehicle, const char* source) {
  SpeedTracker& s = g_speed;
  if (!s.have || s.inVehicle != inVehicle || s.source != source || sampleMs < s.lastMs) {
    s = SpeedTracker{true, inVehicle, source, pos, sampleMs, s.have && s.inVehicle == inVehicle ? s.speed : 0.0f};
    return;
  }
  const uint64_t dtMs = sampleMs - s.lastMs;
  if (dtMs < kMinSpeedWindowMs) return;
  const float dt = static_cast<float>(dtMs) / 1000.0f;
  const float inst = (pos - s.last).Length() / dt;
  s.last = pos;
  s.lastMs = sampleMs;
  if (!std::isfinite(inst) || inst > kMaxSpeedMs) return;   // teleport: keep previous speed
  const float alpha = 1.0f - std::exp(-dt / kSmoothingSeconds);
  s.speed += (inst - s.speed) * alpha;
  if (s.speed < 0.01f) s.speed = 0;
}

}  // namespace

Snapshot Get() {
  std::lock_guard lk(g_mutex);
  Snapshot s = g_snap;
  if (s.valid && util::NowMs() - s.updatedMs > kStaleMs) {
    s.valid = false;
    s.source = "none";
  }
  return s;
}

void SetPollInterval(uint32_t ms) { g_pollMs.store(ms); }

void Tick(float /*dt*/) {
  try {
    const uint64_t now = util::NowMs();
    Poll(now);

    // 1. memory facade
    const auto memPlayer = player::Position();
    const auto memVeh = vehicle::Current();
    const auto memVehPos = memVeh ? vehicle::Position(*memVeh) : std::nullopt;
    const auto memTime = world::TimeOfDay();

    // 2. script data (fresh only)
    const bool scriptFresh = g_script.have && now - g_script.ms <= kStaleMs;

    Snapshot s;
    bool usedMemory = false, usedScript = false;
    if (memPlayer) {
      s.playerPos = *memPlayer;
      s.inVehicle = memVeh.has_value();
      usedMemory = true;
    } else if (scriptFresh && g_script.player) {
      s.playerPos = *g_script.player;
      s.inVehicle = g_script.inVehicle;
      usedScript = true;
    } else if (scriptFresh) {
      s.inVehicle = g_script.inVehicle;
    }
    if (memVehPos) {
      s.vehiclePos = *memVehPos;
      usedMemory = true;
    } else if (scriptFresh && g_script.vehicle && s.inVehicle) {
      s.vehiclePos = *g_script.vehicle;
      usedScript = true;
    }
    if (memTime) {
      s.timeOfDay = memTime;
      usedMemory = true;
    } else if (scriptFresh && g_script.time) {
      s.timeOfDay = g_script.time;
      usedScript = true;
    }
    if (scriptFresh) {
      s.weather = g_script.weather;
      if (!s.weather.empty()) usedScript = true;
    }

    // Speed from whichever position source feeds the tracked entity.
    const bool vehFromMemory = memVehPos.has_value();
    const bool playerFromMemory = memPlayer.has_value();
    if (s.inVehicle && (vehFromMemory || (scriptFresh && g_script.vehicle))) {
      if (vehFromMemory) UpdateSpeed(s.vehiclePos, now, true, "memory");
      else UpdateSpeed(s.vehiclePos, g_script.ms, true, "script");
    } else if (!s.inVehicle && (playerFromMemory || (scriptFresh && g_script.player))) {
      if (playerFromMemory) UpdateSpeed(s.playerPos, now, false, "memory");
      else UpdateSpeed(s.playerPos, g_script.ms, false, "script");
    } else {
      g_speed = SpeedTracker{};
    }
    s.speedMs = g_speed.have ? g_speed.speed : 0.0f;

    if (usedMemory) g_lastUpdateMs = now;
    else if (usedScript) g_lastUpdateMs = g_script.ms;
    s.updatedMs = g_lastUpdateMs;
    s.valid = (usedMemory || usedScript) && g_lastUpdateMs != 0 && now - g_lastUpdateMs <= kStaleMs;
    s.source = !s.valid ? "none" : usedMemory ? "memory" : "script";
    if (!s.valid) s.speedMs = 0;

    std::lock_guard lk(g_mutex);
    g_snap = std::move(s);
  } catch (const std::exception& e) {
    log::Debug("state", "tick failed: {}", e.what());
  } catch (...) {
  }
}

}  // namespace cg::game::state
