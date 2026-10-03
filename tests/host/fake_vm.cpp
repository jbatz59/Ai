// A fake Havok Script VM for the test host: real Lua 5.4 behind HKS-shaped entry points, exported
// from the exe so a bindings file can find them with "export" steps. It deliberately has NO tick
// hook, so Chroma has to use pcall safe points, exactly like Mafia: DE build 0x6092B6F8. Like the
// game, it never calls pcall on the main state: script work runs on a child thread of the VM.
#include "fake_vm.h"

#include <cstddef>
#include <cstdio>

extern "C" {
#include "lauxlib.h"
#include "lstate.h"
#include "lua.h"
#include "lualib.h"
}

namespace {

constexpr int kHksGlobalsIndex = -10002;

struct FakeMachine {
  unsigned char pad[0xD0];
  lua_State* L;   // +0xD0, like C_ScriptMachine
};
FakeMachine g_machine{};
lua_State* g_scriptThread = nullptr;   // child thread the "game" runs its scripts on

const char* kGameApi = R"lua(
unpack = unpack or table.unpack
local P = {}
P.__index = P
function P:SetDemigod(v) self.demigod = v; __host_demigod = v end
function P:EnableInjury(v) self.injury = v end
function P:GetPos() return self.pos end
function P:GetOwner() return nil end
local player = setmetatable({ demigod = false, injury = true, pos = { x = -1234.5, y = 456.25, z = 12 } }, P)
game = { game = { GetActivePlayer = function(self) return player end } }
__host_frames = 0
function __host_frame() __host_frames = __host_frames + 1 end
)lua";

}  // namespace

extern "C" {
// Data export: the global "C_ScriptGameMachine*" the Lua.State binding dereferences.
__declspec(dllexport) FakeMachine* cg_fake_machine = nullptr;

__declspec(dllexport) __attribute__((noinline)) int FakeLoadBuffer(lua_State* L, const char* buf, size_t len, const char* name) {
  return luaL_loadbuffer(L, buf, len, name);
}
__declspec(dllexport) __attribute__((noinline)) int FakePCall(lua_State* L, int nargs, int nresults, int errfunc) {
  return lua_pcall(L, nargs, nresults, errfunc);
}
__declspec(dllexport) __attribute__((noinline)) void FakePushCClosure(lua_State* L, lua_CFunction fn, int n, const char*, int, int) {
  lua_pushcclosure(L, fn, n);
}
__declspec(dllexport) __attribute__((noinline)) void FakeSetField(lua_State* L, int idx, const char* key) {
  if (idx == kHksGlobalsIndex) lua_setglobal(L, key);
  else lua_setfield(L, idx, key);
}
__declspec(dllexport) __attribute__((noinline)) const char* FakeCheckLString(lua_State* L, int idx, size_t* len) {
  return luaL_checklstring(L, idx, len);
}
__declspec(dllexport) __attribute__((noinline)) const char* FakeToLString(lua_State* L, int idx, size_t* len) {
  return lua_tolstring(L, idx, len);
}
}

// Called through a volatile pointer so the call really goes through the (hooked) export.
static int (*volatile g_pcall)(lua_State*, int, int, int) = FakePCall;

std::string FakeVmBindings(bool withGlobalOffset) {
  char buf[4096];
  std::snprintf(buf, sizeof(buf), R"json({
  "schema": 1,
  "game_build": "Chroma test host fake VM",
  "symbols": {
    "Lua.State":         { "kind": "pointer", "steps": [ { "export": "cg_testhost.exe!cg_fake_machine" }, { "deref": true }, { "add": "0xD0" }, { "deref": true } ], "verified": true },
    "Lua.LoadBuffer":    { "kind": "function", "steps": [ { "export": "cg_testhost.exe!FakeLoadBuffer" } ], "verified": true },
    "Lua.PCall":         { "kind": "function", "steps": [ { "export": "cg_testhost.exe!FakePCall" } ], "verified": true },
    "Lua.PushCClosure":  { "kind": "function", "steps": [ { "export": "cg_testhost.exe!FakePushCClosure" } ], "verified": true },
    "Lua.SetField":      { "kind": "function", "steps": [ { "export": "cg_testhost.exe!FakeSetField" } ], "verified": true },
    "Lua.CheckLString":  { "kind": "function", "steps": [ { "export": "cg_testhost.exe!FakeCheckLString" } ], "verified": true },
    "Lua.ToLString":     { "kind": "function", "steps": [ { "export": "cg_testhost.exe!FakeToLString" } ], "verified": true },
    "Lua.ApiTopOffset":  { "kind": "constant", "value": "%zu", "verified": true },
    "Lua.ApiBaseOffset": { "kind": "constant", "value": "%zu", "verified": true },
    "Lua.ObjectSize":    { "kind": "constant", "value": "%zu", "verified": true },
    "Lua.%s":  { "kind": "constant", "value": "%zu", "verified": true }
  }
})json",
                offsetof(lua_State, top), offsetof(lua_State, stack), sizeof(StackValue),
                withGlobalOffset ? "GlobalOffset" : "UnusedOffset", offsetof(lua_State, l_G));
  return buf;
}

bool FakeVmInit(bool exposeState) {
  lua_State* L = luaL_newstate();
  if (!L) return false;
  luaL_openlibs(L);
  if (luaL_dostring(L, kGameApi) != LUA_OK) {
    std::printf("host: fake VM init failed: %s\n", lua_tostring(L, -1));
    return false;
  }
  g_scriptThread = lua_newthread(L);
  luaL_ref(L, LUA_REGISTRYINDEX);   // keep the thread alive
  // exposeState=false: the machine exists but its state slot is empty (a broken Lua.State binding).
  g_machine.L = exposeState ? L : nullptr;
  cg_fake_machine = &g_machine;
  return true;
}

FakeVmFrame FakeVmTick() {
  FakeVmFrame out;
  lua_State* L = g_scriptThread;
  if (!L) return out;
  const int before = lua_gettop(L);
  lua_getglobal(L, "__host_frame");
  out.status = g_pcall(L, 0, 0, 0);
  if (out.status != LUA_OK) lua_pop(L, 1);
  out.stackBalanced = lua_gettop(L) == before;
  lua_getglobal(L, "__host_demigod");
  out.demigod = lua_toboolean(L, -1) != 0;
  lua_pop(L, 1);
  return out;
}
