#pragma once
// Bindings = the only place game-version-specific knowledge lives. They are loaded from JSON
// (Chroma/bindings/*.json, merged in file-name order, later files override earlier ones;
// user pins from the UI live in Chroma/bindings/zz_user_overrides.json), so a game patch
// never requires recompiling — fix the JSON (or pin a symbol from the RE tools) and hit Reload.
//
// ---------------------------------------------------------------------------------------------
// File format (schema 1):
// {
//   "schema": 1,
//   "game_build": "optional free text, shown in UI",
//   "symbols": {
//     "<Name>": {
//       "kind": "address" | "pointer" | "offset" | "field" | "function" | "constant",
//       "steps": [ <step>, ... ],          // address / pointer / function
//       "fallbacks": [ [<step>...], ... ], // tried in order if "steps" fails
//       "chain": [ "0x78", "0x40" ],       // field: offsets applied to an object base (see Field())
//       "value": "0x1A0",                  // offset / constant
//       "type": "f32",                     // optional ValueType hint for UI and generic cheats
//       "expect_class": "C_Player2",       // optional RTTI validation of the resolved object
//       "verified": false,                 // UI shows unverified symbols in amber
//       "notes": "how this was found / which build"
//     }
//   },
//   "cheats": [ <cheat>, ... ]             // generic data-driven features, see features/cheat_table.h
// }
//
// Steps (executed left to right on a running uintptr_t "cursor"):
//   {"module": "name.exe"}           cursor = module base (default: host exe when name omitted/"")
//   {"pattern": "48 8B 05 ? ? ? ?", "index": 0, "section": ".text"}   first (or index-th) match in module
//                                    "unique": true => fail unless it matches exactly once
//   {"rtti_vtable": "C_Player2"}     cursor = primary vtable address of RTTI class
//   {"rtti_instance": "C_Game"}      cursor = first live object of class (slow heap scan, cached)
//   {"string": "text", "wide": false} cursor = address of string literal in module
//   {"xref": 0}                      cursor = n-th instruction referencing cursor (string -> code)
//   {"function_start": true}         cursor = start of the function containing cursor (.pdata)
//   {"symbol": "Other.Name"}         cursor = resolved value of another symbol (dependency)
//   {"export": "dll!Function"}       cursor = exported function
//   {"add": "0x10"} / {"sub": 16}    arithmetic
//   {"rip": [3, 7]}                  cursor = cursor + 7 + *(int32*)(cursor + 3)
//   {"call": true}                   cursor = target of E8/E9 rel32 at cursor
//   {"deref": true}                  cursor = *(uint64*)cursor    <-- makes the symbol DYNAMIC
//   {"vfunc": 12}                    cursor = (*(uint64**)cursor)[12]  (cursor must be an object) <-- dynamic
//
// Kinds:
//   address  : static after resolve (no deref/vfunc steps allowed)
//   pointer  : static prefix resolved once; the dynamic tail (from the first deref/vfunc on) is
//              re-evaluated on every Addr() call, so it follows objects that are re-created.
//   function : like address; the UI marks it callable.
//   offset   : plain integer ("value"); Offset() returns it.
//   field    : offsets relative to an object (CE pointer semantics, see mem::Deref). Field(name, obj).
//   constant : plain integer for anything else (vtable indices, enum values, sizes).
// ---------------------------------------------------------------------------------------------
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace cg::game {

enum class SymbolKind : uint8_t { Address, Pointer, Function, Offset, Field, Constant };
enum class SymbolStatus : uint8_t { Pending, Resolved, Failed, Overridden };

const char* SymbolKindName(SymbolKind k);
const char* SymbolStatusName(SymbolStatus s);

struct SymbolInfo {
  std::string name;
  SymbolKind kind = SymbolKind::Address;
  SymbolStatus status = SymbolStatus::Pending;
  uintptr_t staticValue = 0;   // resolved static prefix / offset / constant
  bool dynamic = false;
  bool verified = false;
  std::string type;            // ValueType hint ("" if none)
  std::string expectClass;
  std::string error;           // why it failed
  std::string notes;
  std::string sourceFile;
  double resolveMs = 0;
};

class Bindings {
 public:
  static Bindings& Get();

  // Loads every *.json in `dir` (sorted), then resolves all symbols. Blocking; call from
  // tasks::RunAsync. Returns false only if no file could be parsed. Safe to call again (reload).
  bool LoadDirectory(const std::filesystem::path& dir);
  bool Reload();               // same directory as the last LoadDirectory
  bool Resolving() const;
  float Progress() const;      // 0..1 while resolving
  uint64_t Generation() const; // increments after every completed (re)load

  // Lookups — thread-safe, lock-free-ish hot path (shared lock).
  bool Has(std::string_view name) const;                          // resolved or overridden
  std::optional<uintptr_t> Addr(std::string_view name) const;     // address/pointer/function
  std::optional<int64_t> Offset(std::string_view name) const;     // offset/constant
  std::optional<uintptr_t> Field(std::string_view name, uintptr_t object) const;   // field
  std::optional<SymbolInfo> Info(std::string_view name) const;
  std::vector<SymbolInfo> List() const;

  // Cheats array merged from all files (copy; small).
  nlohmann::json Cheats() const;
  std::string GameBuild() const;
  std::vector<std::string> LoadErrors() const;   // parse errors per file

  // User pins: force a symbol to a fixed address/offset (persisted to zz_user_overrides.json).
  void Pin(const std::string& name, SymbolKind kind, uintptr_t value);
  void Unpin(const std::string& name);

  // Adds/replaces a symbol definition at runtime (RE tools "Save as binding"); persisted to the
  // overrides file and resolved immediately.
  bool Define(const std::string& name, const nlohmann::json& definition, std::string* error = nullptr);

 private:
  Bindings();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Named handle so feature code reads like data: static const game::Sym kHealth("Human.Health");
class Sym {
 public:
  explicit Sym(const char* name) : name_(name) {}
  const char* Name() const { return name_; }
  bool Ok() const { return Bindings::Get().Has(name_); }
  std::optional<uintptr_t> Addr() const { return Bindings::Get().Addr(name_); }
  std::optional<int64_t> Offset() const { return Bindings::Get().Offset(name_); }
  std::optional<uintptr_t> Field(uintptr_t obj) const { return Bindings::Get().Field(name_, obj); }

 private:
  const char* name_;
};

}  // namespace cg::game
