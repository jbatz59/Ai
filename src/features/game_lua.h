#pragma once
// Helpers for features that drive the game through its own script VM (Havok Script, Lua 5.1 dialect).
//
// Build chunks with LuaQuote/LuaNumber/LuaBool (never splice raw user text into Lua source) and run
// them with RunFeatureChunk. Every chunk queued through RunFeatureChunk can rely on the CG prelude
// below being installed in the game VM, so a method missing on this game build produces a readable
// message ("SetDemigod not available on this build") instead of a Lua stack trace.
//
// ---- CG prelude (global table `CG`, re-installed whenever game::vm::Generation() changes) -----------
//   CG.player()               -> player | nil, err     game.game:GetActivePlayer(), nil while loading
//   CG.vehicle()              -> vehicle | nil, err    player:GetOwner() (nil, err when on foot)
//   CG.target()               -> obj, inVehicle | nil, err   the vehicle when in one, else the player
//   CG.vec(x, y, z)           -> vector | nil, err     Math:newVector(x, y, z)
//   CG.get(k1, k2, ...)       -> value | nil           safe walk from the globals: CG.get("game", "police")
//   CG.has(obj, name)         -> bool                  obj[name] exists (never raises)
//   CG.try(f, ...)            -> true, results... | false, err     pcall with the error as a string
//   CG.call(obj, method, ...) -> true, results... | false, err     obj:method(...) protected; missing obj or
//                                                                  method => "<method> not available on this build"
//   CG.must(ok, ...)          -> ...  raises error(err, 0) when ok is false — wraps CG.call/CG.try:
//                                     CG.must(CG.call(p, "SetDemigod", true))
//   CG.need(value, err)       -> value  raises error(err, 0) when value is nil — wraps CG.player() & co:
//                                     local p = CG.need(CG.player())
// Errors raised with error(msg, 0) reach the toast verbatim ("God mode failed: no active player").
// Example chunk:
//   local p = CG.need(CG.player())
//   CG.must(CG.call(p, "SetDemigod", true))
// --------------------------------------------------------------------------------------------------------
#include <string>
#include <string_view>

#include "game/script_vm.h"

namespace cg::features {

// Lua string literal for any bytes (embedded NULs, quotes, newlines, "]]", UTF-8). The result is pure
// printable ASCII: '\\', '"', '\'' are backslash-escaped, every other byte outside 0x20..0x7E becomes a
// three-digit \ddd escape. Valid in Lua 5.1 (HKS) and 5.4.
std::string LuaQuote(std::string_view s);

// Lua number expression, independent of the C locale, shortest form that round-trips the double
// ("5", "0.1", "1e+21"). Negative values are parenthesised ("(-2.5)") so "x-" .. LuaNumber(-1) can
// never form a "--" comment. Non-finite values become "nil" so the game call fails loudly instead of
// receiving NaN/inf.
std::string LuaNumber(double v);

inline const char* LuaBool(bool b) { return b ? "true" : "false"; }

inline constexpr int kCgPreludeVersion = 1;

// Lua source of the CG prelude (documented above).
std::string_view CgPrelude();

// What RunFeatureChunk queues: `code` prefixed (on its first line, so error line numbers stay
// unchanged) with a guard that raises kCgPreludeMissingMarker when CG is absent or outdated.
std::string GuardedChunk(std::string_view code);
inline constexpr std::string_view kCgPreludeMissingMarker = "chroma: CG prelude missing";

// Queues the prelude unless it was already queued for the current game::vm::Generation().
// Render thread. Not needed before RunFeatureChunk (which calls it); useful for consoles.
void EnsurePrelude();

// Runs a feature chunk in the game VM on the next script tick: ensures the prelude, then queues
// GuardedChunk(code) under the chunk name `what`. If the VM was reset between the two (the guard
// fires) the prelude and chunk are queued once more. On failure a toast "<what> failed: <error>" is
// shown unless toastOnError is false (the error is still logged). `done` (optional) receives the
// final result on the render thread.
void RunFeatureChunk(std::string code, std::string what, game::vm::Callback done = {}, bool toastOnError = true);

}  // namespace cg::features
