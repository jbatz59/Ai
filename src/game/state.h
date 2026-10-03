#pragma once
// Periodic snapshot of game state for HUD readouts, teleport bookmarks, speedometers and ESP.
// Sources, in order of preference per field:
//   1. memory bindings via the game facade (game.h) — every frame, cheapest
//   2. a script-VM poll (script_vm.h) every ~200 ms:
//        player:GetPos(), player:GetOwner() (vehicle), vehicle:GetPos(), game.gfx:GetTime(),
//        game.gfx:GetCurrentWeatherSetName()   — each wrapped in pcall inside the chunk
// Speeds are derived from successive positions, so they are in m/s regardless of game units.
#include <cstdint>
#include <optional>
#include <string>

#include "game/types.h"

namespace cg::game::state {

struct Snapshot {
  bool valid = false;            // at least one source produced data recently (< 2 s)
  bool inVehicle = false;
  Vec3 playerPos{};
  Vec3 vehiclePos{};
  float speedMs = 0;             // of the vehicle if in one, else the player
  std::optional<float> timeOfDay;
  std::string weather;
  uint64_t updatedMs = 0;        // util::NowMs() of the last successful update
  const char* source = "none";   // "memory", "script", "none"
};

Snapshot Get();        // thread-safe copy
void Tick(float dt);   // render thread, every frame (called by the overlay before feature ticks)
void SetPollInterval(uint32_t ms);   // default 200; 0 disables script polling

}  // namespace cg::game::state
