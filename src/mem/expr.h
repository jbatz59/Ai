#pragma once
// Address expressions used everywhere a user can type an address (address table, hex view,
// scanner ranges, scripts):
//   "7FF6A1B20000"  "0x7FF6A1B20000"      raw hex (bare numbers are hex, Cheat-Engine style)
//   "game.exe+1A2B"  "\"my mod.dll\"+10"  module base + offset (quote names with spaces)
//   "Player.Ptr"                          bindings symbol (via resolver)
//   "[Player.Ptr]+0x78"  "[[a]+10]+8"     brackets dereference a pointer
//   "a + 16*4 - 0x10"                     + - * on integers, decimal with '#' prefix: "#16"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace cg::mem {

struct ExprResult {
  bool ok = false;
  uintptr_t value = 0;
  std::string error;
};

// Resolves identifiers that are not module names (bindings symbols, script variables).
using SymbolResolver = std::function<std::optional<uintptr_t>(std::string_view)>;

ExprResult EvalAddress(std::string_view expr, const SymbolResolver& resolver = {});

}  // namespace cg::mem
