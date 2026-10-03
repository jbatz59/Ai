#pragma once
// Bridge into the GAME's own embedded Lua VM (distinct from Consigliere's script engine).
// Requires bindings: Lua.State (pointer -> lua_State*), Lua.LoadBuffer (luaL_loadbuffer[x]),
// Lua.PCall (lua_pcall / lua_pcallk), Lua.ToLString, Lua.SetTop, Lua.GetTop, and the constant
// Lua.Version (501 or 503/504). Calls run on the game thread via tasks::PostGame.
#include <functional>
#include <string>

namespace cg::game::lua {

bool Available();
std::string MissingReason();   // human-readable list of missing bindings

using Done = std::function<void(bool ok, std::string result)>;   // runs on the game thread
void Execute(std::string code, Done done = {});

}  // namespace cg::game::lua
