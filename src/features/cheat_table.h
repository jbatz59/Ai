#pragma once
// Data-driven cheats (Cheat-Engine-table style) declared in bindings JSON "cheats": [...].
// They become real Features (category from "category", default CheatTable) and are rebuilt on
// every bindings reload (ids prefixed "table.").
//
// Common fields: "id", "name", "category" (Player|Vehicle|Weapons|World|Teleport|Camera|Visuals|CheatTable),
//                "description", "hotkey" ("Ctrl+F1"), "requires": ["Sym", ...] (extra requirements)
//
// Targets ("target") — where the value lives:
//   {"symbol": "World.TimeScale"}                         address/pointer symbol
//   {"symbol": "Player.Object", "field": "Human.Health"}  field symbol applied to the object
//   {"expr": "[game.exe+1A2B]+10"}                        mem::EvalAddress expression (symbols allowed)
//
// Types:
//   "patch"  : {"site": {"symbol": "X"} | {"expr": ...}, "bytes": "90 90 90" | "nop": 5}
//              Toggle. Applies through mem::Patches (original bytes verified).
//   "freeze" : {"target": ..., "value_type": "f32", "value": "1000" | "max:<FieldSymbol>"}
//              Toggle. Writes the value every frame while enabled.
//   "set"    : {"target": ..., "value_type": "i32", "value": "5"}            Action. Writes once.
//   "slider" : {"target": ..., "value_type": "f32", "min": 0, "max": 24, "default": 12, "format": "%.1f h"}
//              Panel. Live slider; optional "freeze": true adds a lock checkbox.
//   "lua"    : {"code": "..."}  Action. Runs in the GAME's Lua VM via game::lua (if available).
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace cg::features {

// Validates one cheat definition; returns "" if OK, else the reason (shown in the Bindings window).
std::string ValidateCheat(const nlohmann::json& cheat);

}  // namespace cg::features
