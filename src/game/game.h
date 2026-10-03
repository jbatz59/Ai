#pragma once
// Typed facade over bindings. Every function is crash-safe and returns nullopt/false when a
// binding is missing, a pointer is implausible, or mp_guard blocks gameplay writes.
// Canonical symbol names used here are documented in docs/BINDINGS.md — keep them in sync.
//
// Threading: reads/writes of plain memory are safe from the render thread. Anything that CALLS a
// game function (SetPosition via function, Repair) is queued with tasks::PostGame and the
// function returns true when it was queued.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "game/types.h"

namespace cg::game {

bool InGame();   // local player resolves to a plausible object and Game.IsLoading (if bound) is 0

namespace player {
std::optional<uintptr_t> Object();                 // Player.Object
std::optional<Vec3> Position();                    // Entity.Position on the player
bool SetPosition(const Vec3& p);                   // Entity.SetPosition (queued) else raw write
std::optional<float> Health();                     // Human.Health
std::optional<float> MaxHealth();                  // Human.HealthMax
bool SetHealth(float hp);
std::optional<uintptr_t> Vehicle();                // Human.Vehicle (nullopt when on foot)
}  // namespace player

namespace vehicle {
std::optional<uintptr_t> Current();                // == player::Vehicle()
std::optional<Vec3> Position(uintptr_t veh);
bool SetPosition(uintptr_t veh, const Vec3& p);
std::optional<Vec3> Velocity(uintptr_t veh);       // Vehicle.Velocity (m/s, world space)
bool SetVelocity(uintptr_t veh, const Vec3& v);
std::optional<float> Health(uintptr_t veh);        // Vehicle.Health
bool SetHealth(uintptr_t veh, float hp);
bool Repair(uintptr_t veh);                        // Vehicle.Repair (queued) else Health = HealthMax
}  // namespace vehicle

namespace world {
std::optional<float> TimeOfDay();                  // World.TimeOfDay, hours 0..24
bool SetTimeOfDay(float hours);
std::optional<float> TimeScale();                  // World.TimeScale, 1 = normal
bool SetTimeScale(float scale);
}  // namespace world

namespace camera {
std::optional<Mat4> ViewProjection();              // Camera.ViewProjection
std::optional<Vec3> Position();                    // Camera.Position
std::optional<Vec3> Forward();                     // Camera.View third column (m[0][2], m[1][2], m[2][2]) normalised
                                                   // (View transposed first when Camera.MatrixTransposed == 1)
std::optional<float> Fov();                        // Camera.Fov (degrees)
bool SetFov(float degrees);
}  // namespace camera

// Names of every symbol the facade can use, with a one-line purpose (Bindings UI, docs, tests).
struct CanonicalSymbol {
  const char* name;
  const char* kind;    // "pointer", "field", "function", ...
  const char* type;    // value type hint or signature
  const char* purpose;
};
const std::vector<CanonicalSymbol>& CanonicalSymbols();

}  // namespace cg::game
