#pragma once
// Bridge into the game's embedded Havok Script VM (HKS — a Lua 5.1 dialect). This is the PRIMARY
// way Chroma drives gameplay: the game's own script API (game.game:GetActivePlayer(),
// player:SetDemigod(true), game.police:Disable(), game.gfx:SetWeatherSet(...), vehicle:Repair(true)…)
// survives game patches far better than raw offsets. Only a handful of C entry points are
// build-specific, and those come from bindings.
//
// Bindings (see docs/BINDINGS.md):
//   required  Game.TickHook      function  C_ScriptMachine::Tick-like, int64 (*)(void* machine). Hooked;
//                                          everything below runs inside it, on the script thread.
//               — or —  pcall mode (no tick hook): Lua.PCall is hooked and chunks run right before
//                                          the game's own pcall on any state of the main VM (same
//                                          Lua.GlobalOffset global state as Lua.State). Needs the
//                                          three stack constants below.
//             Lua.StateOffset    constant  offset of lua_State* inside the machine (0xD0 on known builds)
//               — or —  Lua.State pointer  resolves directly to the lua_State*
//             Lua.LoadBuffer     function  int (*)(lua_State*, const char* buf, size_t len, const char* name)
//             Lua.PCall          function  int (*)(lua_State*, int nargs, int nresults, int errfunc)
//   optional  Lua.PushCClosure   function  void (*)(lua_State*, int(*)(lua_State*), int n, const char* name, int, int)
//             Lua.SetField       function  void (*)(lua_State*, int idx, const char* key)
//             Lua.CheckLString   function  const char* (*)(lua_State*, int idx, size_t* len)
//                                          (all three => return values + print capture via a C closure)
//             Lua.ToLString      function  const char* (*)(lua_State*, int idx, size_t* len)  (fallback for errors)
//             Lua.ResetState     function  int64 (*)(void* machine, char async, uint64 timeout); returns 3 when ready
//             Lua.ApiTopOffset / Lua.ApiBaseOffset / Lua.ObjectSize  constants (0x48 / 0x50 / 16) — stack restore
//             Lua.GlobalOffset   constant  offset of the global-state pointer in lua_State (0x10) — VM identity
//             Lua.PCallLock      constant  1 = also hook Lua.PCall with a critical section so worker-thread pcalls
//                                          never overlap our chunks (default 1 when Lua.PCall is bound)
//
// HKS facts baked in: LUA_GLOBALSINDEX = -10002, LUA_MULTRET = -1, every non-zero status is an error.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cg::game::vm {

enum class Status : uint8_t {
  Unbound,         // required bindings missing
  WaitingForTick,  // hook installed, script machine not seen yet
  WaitingForVm,    // lua_State seen, VM not ready (loading / resetting)
  Ready
};
Status GetStatus();
const char* StatusText();                   // human readable, for UI
std::vector<std::string> MissingBindings(); // required ones only
bool Ready();
bool PCallMode();                           // running at pcall safe points (no tick hook)
bool HasReturnValues();                     // optional C-closure bindings resolved

struct Result {
  bool ok = false;
  std::string error;                        // compile/runtime error text from the VM
  std::vector<std::string> values;          // tostring() of every returned value (needs HasReturnValues())
  double ms = 0;                            // execution time on the script thread
};
// Callbacks are always delivered on the RENDER thread (tasks::PostRender), so they may touch UI,
// features and Chroma's own Lua engine directly.
using Callback = std::function<void(const Result&)>;

// Queues a chunk for the next script tick. Not ready => callback immediately with ok=false.
// asExpression: try "return <code>" first (REPL style), fall back to running it as a statement.
void Run(std::string code, Callback cb = {}, std::string chunkName = "=chroma", bool asExpression = false);

// Lines printed by game scripts / our chunks (needs the C-closure bindings). Thread-safe, max 2000.
std::vector<std::string> TakePrintedLines();

// Increments every time the VM is (re)created or reset (new game, load save). Features that set
// script-side state (demigod, unlimited ammo, police off) re-apply when this changes.
uint64_t Generation();

// Hook lifecycle — bootstrap calls Install() after every bindings (re)load and Uninstall() on unload.
void Install();
void Uninstall();
int ThreadsInside();     // > 0 => unsafe to free the DLL
bool OnScriptThread();

}  // namespace cg::game::vm
