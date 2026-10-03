#include "game/game.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

#include "core/mp_guard.h"
#include "core/tasks.h"
#include "game/bindings.h"
#include "mem/safe.h"

namespace cg::game {
namespace {

static_assert(std::is_trivially_copyable_v<Vec3> && sizeof(Vec3) == 12);
static_assert(std::is_trivially_copyable_v<Mat4> && sizeof(Mat4) == 64);

const Sym kPlayerObject("Player.Object");
const Sym kHumanHealth("Human.Health");
const Sym kHumanHealthMax("Human.HealthMax");
const Sym kHumanVehicle("Human.Vehicle");
const Sym kEntityPosition("Entity.Position");
const Sym kEntitySetPosition("Entity.SetPosition");
const Sym kVehicleHealth("Vehicle.Health");
const Sym kVehicleHealthMax("Vehicle.HealthMax");
const Sym kVehicleVelocity("Vehicle.Velocity");
const Sym kVehicleRepair("Vehicle.Repair");
const Sym kWorldTimeOfDay("World.TimeOfDay");
const Sym kWorldTimeScale("World.TimeScale");
const Sym kCameraViewProjection("Camera.ViewProjection");
const Sym kCameraView("Camera.View");
const Sym kCameraPosition("Camera.Position");
const Sym kCameraFov("Camera.Fov");
const Sym kCameraMatrixTransposed("Camera.MatrixTransposed");
const Sym kGameIsLoading("Game.IsLoading");

using SetPositionFn = void (*)(void*, const float*);
using RepairFn = void (*)(void*);

bool Finite(float f) { return std::isfinite(f); }
bool Finite(const Vec3& v) { return Finite(v.x) && Finite(v.y) && Finite(v.z); }
bool Finite(const Mat4& m) {
  for (const auto& row : m.m)
    for (float f : row)
      if (!Finite(f)) return false;
  return true;
}

bool WritesBlocked() { return mp_guard::Blocked(); }

// Address of a field on a plausible object.
std::optional<uintptr_t> FieldAddr(const Sym& s, uintptr_t obj) {
  if (!mem::IsPlausiblePtr(obj)) return std::nullopt;
  return s.Field(obj);
}

std::optional<float> ReadFloatField(const Sym& s, uintptr_t obj) {
  auto a = FieldAddr(s, obj);
  if (!a) return std::nullopt;
  auto v = mem::Read<float>(*a);
  if (!v || !Finite(*v)) return std::nullopt;
  return v;
}

std::optional<Vec3> ReadVecField(const Sym& s, uintptr_t obj) {
  auto a = FieldAddr(s, obj);
  if (!a) return std::nullopt;
  auto v = mem::Read<Vec3>(*a);
  if (!v || !Finite(*v)) return std::nullopt;
  return v;
}

template <class T> bool WriteField(const Sym& s, uintptr_t obj, const T& v) {
  if (WritesBlocked()) return false;
  auto a = FieldAddr(s, obj);
  if (!a) return false;
  return mem::Write<T>(*a, v);
}

// Pointer-kind symbol -> value of type T at the resolved address.
template <class T> std::optional<T> ReadPointerSym(const Sym& s) {
  auto a = s.Addr();
  if (!a) return std::nullopt;
  return mem::Read<T>(*a);
}

std::optional<float> ReadFloatSym(const Sym& s) {
  auto v = ReadPointerSym<float>(s);
  if (!v || !Finite(*v)) return std::nullopt;
  return v;
}

bool WriteFloatSym(const Sym& s, float v) {
  if (WritesBlocked() || !Finite(v)) return false;
  auto a = s.Addr();
  if (!a) return false;
  return mem::Write<float>(*a, v);
}

bool MatrixTransposed() { return kCameraMatrixTransposed.Offset().value_or(0) == 1; }

Mat4 Transposed(const Mat4& in) {
  Mat4 out;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out.m[r][c] = in.m[c][r];
  return out;
}

// Queued game-function calls re-validate the object on the game thread: the first qword (vtable)
// must be unchanged, otherwise the object was destroyed/replaced in the meantime.
std::optional<uintptr_t> VTableOf(uintptr_t obj) { return mem::ReadPtr(obj); }

bool SetEntityPosition(uintptr_t obj, const Vec3& p) {
  if (WritesBlocked() || !mem::IsPlausiblePtr(obj) || !Finite(p)) return false;
  if (auto fn = kEntitySetPosition.Addr()) {
    auto vt = VTableOf(obj);
    if (!vt) return false;
    const uintptr_t f = *fn, v = *vt;
    tasks::PostGame([obj, f, v, p] {
      if (mp_guard::Blocked() || VTableOf(obj) != v) return;
      const float xyz[3] = {p.x, p.y, p.z};
      reinterpret_cast<SetPositionFn>(f)(reinterpret_cast<void*>(obj), xyz);
    });
    return true;
  }
  return WriteField(kEntityPosition, obj, p);
}

}  // namespace

bool InGame() {
  auto obj = player::Object();
  if (!obj) return false;
  if (kGameIsLoading.Ok()) {
    auto loading = ReadPointerSym<uint8_t>(kGameIsLoading);
    if (!loading || *loading != 0) return false;
  }
  return true;
}

// ---- player -----------------------------------------------------------------------------------
namespace player {

std::optional<uintptr_t> Object() {
  auto a = kPlayerObject.Addr();
  if (!a || !mem::IsPlausiblePtr(*a) || !mem::IsReadable(*a, sizeof(uintptr_t))) return std::nullopt;
  return a;
}

std::optional<Vec3> Position() {
  auto obj = Object();
  if (!obj) return std::nullopt;
  return ReadVecField(kEntityPosition, *obj);
}

bool SetPosition(const Vec3& p) {
  auto obj = Object();
  if (!obj) return false;
  return SetEntityPosition(*obj, p);
}

std::optional<float> Health() {
  auto obj = Object();
  if (!obj) return std::nullopt;
  return ReadFloatField(kHumanHealth, *obj);
}

std::optional<float> MaxHealth() {
  auto obj = Object();
  if (!obj) return std::nullopt;
  return ReadFloatField(kHumanHealthMax, *obj);
}

bool SetHealth(float hp) {
  if (!Finite(hp)) return false;
  auto obj = Object();
  if (!obj) return false;
  return WriteField(kHumanHealth, *obj, std::max(hp, 0.0f));
}

std::optional<uintptr_t> Vehicle() {
  auto obj = Object();
  if (!obj) return std::nullopt;
  auto a = FieldAddr(kHumanVehicle, *obj);
  if (!a) return std::nullopt;
  return mem::ReadPtr(*a);   // nullopt when the slot is null (on foot)
}

}  // namespace player

// ---- vehicle ----------------------------------------------------------------------------------
namespace vehicle {

std::optional<uintptr_t> Current() { return player::Vehicle(); }

std::optional<Vec3> Position(uintptr_t veh) { return ReadVecField(kEntityPosition, veh); }

bool SetPosition(uintptr_t veh, const Vec3& p) { return SetEntityPosition(veh, p); }

std::optional<Vec3> Velocity(uintptr_t veh) { return ReadVecField(kVehicleVelocity, veh); }

bool SetVelocity(uintptr_t veh, const Vec3& v) {
  if (!Finite(v)) return false;
  return WriteField(kVehicleVelocity, veh, v);
}

std::optional<float> Health(uintptr_t veh) { return ReadFloatField(kVehicleHealth, veh); }

bool SetHealth(uintptr_t veh, float hp) {
  if (!Finite(hp)) return false;
  return WriteField(kVehicleHealth, veh, std::max(hp, 0.0f));
}

bool Repair(uintptr_t veh) {
  if (WritesBlocked() || !mem::IsPlausiblePtr(veh)) return false;
  if (auto fn = kVehicleRepair.Addr()) {
    auto vt = VTableOf(veh);
    if (!vt) return false;
    const uintptr_t f = *fn, v = *vt;
    tasks::PostGame([veh, f, v] {
      if (mp_guard::Blocked() || VTableOf(veh) != v) return;
      reinterpret_cast<RepairFn>(f)(reinterpret_cast<void*>(veh));
    });
    return true;
  }
  auto max = ReadFloatField(kVehicleHealthMax, veh);
  if (!max || *max <= 0) return false;
  return WriteField(kVehicleHealth, veh, *max);
}

}  // namespace vehicle

// ---- world ------------------------------------------------------------------------------------
namespace world {

std::optional<float> TimeOfDay() { return ReadFloatSym(kWorldTimeOfDay); }

bool SetTimeOfDay(float hours) {
  if (!Finite(hours)) return false;
  return WriteFloatSym(kWorldTimeOfDay, std::clamp(hours, 0.0f, std::nextafter(24.0f, 0.0f)));
}

std::optional<float> TimeScale() { return ReadFloatSym(kWorldTimeScale); }

bool SetTimeScale(float scale) {
  if (!Finite(scale)) return false;
  return WriteFloatSym(kWorldTimeScale, std::clamp(scale, 0.01f, 10.0f));
}

}  // namespace world

// ---- camera -----------------------------------------------------------------------------------
namespace camera {

std::optional<Mat4> ViewProjection() {
  auto m = ReadPointerSym<Mat4>(kCameraViewProjection);
  if (!m || !Finite(*m)) return std::nullopt;
  return m;
}

std::optional<Vec3> Position() {
  auto p = ReadPointerSym<Vec3>(kCameraPosition);
  if (!p || !Finite(*p)) return std::nullopt;
  return p;
}

std::optional<Vec3> Forward() {
  auto raw = ReadPointerSym<Mat4>(kCameraView);
  if (!raw || !Finite(*raw)) return std::nullopt;
  const Mat4 v = MatrixTransposed() ? Transposed(*raw) : *raw;
  const Vec3 f{v.m[0][2], v.m[1][2], v.m[2][2]};
  if (f.Length() < 1e-4f) return std::nullopt;
  return f.Normalized();
}

std::optional<float> Fov() { return ReadFloatSym(kCameraFov); }

bool SetFov(float degrees) {
  if (!Finite(degrees)) return false;
  return WriteFloatSym(kCameraFov, std::clamp(degrees, 10.0f, 170.0f));
}

}  // namespace camera

// ---- canonical symbols ------------------------------------------------------------------------
const std::vector<CanonicalSymbol>& CanonicalSymbols() {
  static const std::vector<CanonicalSymbol> kSymbols = {
      // Memory facade (game.h)
      {"Player.Object", "pointer", "ptr", "Local player human object (C_Player2); everything player:: reads starts here"},
      {"Human.Health", "field", "f32", "Current health on a human object"},
      {"Human.HealthMax", "field", "f32", "Maximum health on a human object"},
      {"Human.Vehicle", "field", "ptr", "Slot holding the vehicle the human is in (null on foot)"},
      {"Entity.Position", "field", "vec3", "World position (x, y, z; Z up) on any entity (humans, vehicles)"},
      {"Entity.SetPosition", "function", "void (*)(void* entity, const float* xyz)", "Teleport an entity (queued to the game thread); raw position write when unbound"},
      {"Vehicle.Health", "field", "f32", "Current vehicle health"},
      {"Vehicle.HealthMax", "field", "f32", "Maximum vehicle health (Repair fallback)"},
      {"Vehicle.Velocity", "field", "vec3", "Linear velocity in m/s, world space"},
      {"Vehicle.Repair", "function", "void (*)(void* vehicle)", "Full repair (queued to the game thread); Health = HealthMax when unbound"},
      {"World.TimeOfDay", "pointer", "f32", "Time of day in hours [0, 24)"},
      {"World.TimeScale", "pointer", "f32", "Global simulation time scale, 1 = normal"},
      {"Camera.ViewProjection", "pointer", "mat4", "Combined view*projection matrix (row-vector convention) for WorldToScreen"},
      {"Camera.View", "pointer", "mat4", "View matrix; its third column is the camera forward vector"},
      {"Camera.Position", "pointer", "vec3", "Camera world position"},
      {"Camera.Fov", "pointer", "f32", "Vertical field of view in degrees"},
      {"Camera.MatrixTransposed", "constant", "0|1", "1 when the camera matrices are stored column-major (transposed before use)"},
      {"Game.IsLoading", "pointer", "u8", "Non-zero while a loading screen is active (InGame() is false)"},
      // Script VM (script_vm.h)
      {"Game.TickHook", "function", "int64 (*)(void* machine)", "C_ScriptMachine::Tick-like; hooked, queued chunks run inside it. Optional: without it Chroma uses pcall safe points (needs Lua.State + the stack constants)"},
      {"Lua.StateOffset", "constant", "offset", "Offset of lua_State* inside the script machine (0xD0 on known builds); or bind Lua.State"},
      {"Lua.State", "pointer", "lua_State*", "Resolves directly to the lua_State* (alternative to Lua.StateOffset)"},
      {"Lua.LoadBuffer", "function", "int (*)(lua_State*, const char* buf, size_t len, const char* name)", "Compiles a chunk (required)"},
      {"Lua.PCall", "function", "int (*)(lua_State*, int nargs, int nresults, int errfunc)", "Protected call (required)"},
      {"Lua.PushCClosure", "function", "void (*)(lua_State*, int (*)(lua_State*), int n, const char* name, int, int)", "Pushes a C closure (return values + print capture)"},
      {"Lua.SetField", "function", "void (*)(lua_State*, int idx, const char* key)", "t[key] = top (return values + print capture)"},
      {"Lua.CheckLString", "function", "const char* (*)(lua_State*, int idx, size_t* len)", "Reads a string argument (return values + print capture)"},
      {"Lua.ToLString", "function", "const char* (*)(lua_State*, int idx, size_t* len)", "Converts a stack value to string (error text fallback)"},
      {"Lua.ResetState", "function", "int64 (*)(void* machine, char async, uint64 timeout)", "VM reset/readiness probe; returns 3 when ready"},
      {"Lua.ApiTopOffset", "constant", "offset", "Offset of the stack top pointer in lua_State (0x48) for stack restore"},
      {"Lua.ApiBaseOffset", "constant", "offset", "Offset of the stack base pointer in lua_State (0x50) for stack restore"},
      {"Lua.ObjectSize", "constant", "size", "Size of one stack slot (16) for stack restore"},
      {"Lua.GlobalOffset", "constant", "offset", "Offset of the global-state pointer in lua_State (0x10); pcall mode accepts every state of the main VM"},
      {"Lua.PCallLock", "constant", "0|1", "1 = also hook Lua.PCall with a critical section (default 1 when Lua.PCall is bound)"},
  };
  return kSymbols;
}

}  // namespace cg::game
