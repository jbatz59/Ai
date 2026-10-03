# Bindings

Bindings are the only place game-build-specific knowledge lives. Consigliere loads every
`Consigliere\bindings\*.json` in name order; later files override earlier symbols, and
`zz_user_overrides.json` holds what you pin from the UI. **Tools → Game Bindings** shows each symbol's
status, value and error, and has a **Reload** button. You never need to recompile.

The full format (steps, kinds) is documented at the top of `src/game/bindings.h`. The short version:

```json
{
  "schema": 1,
  "symbols": {
    "Lua.PCall": {
      "kind": "function",
      "steps": [ { "pattern": "48 89 5C 24 18 55 56 57 ..." } ],
      "fallbacks": [ [ { "pattern": "E8 ? ? ? ? 85 C0 74 05" }, { "call": true } ] ],
      "verified": false,
      "notes": "where this came from"
    },
    "Human.Health": { "kind": "field", "chain": [ "0x8" ], "type": "f32" }
  },
  "cheats": []
}
```

Steps: `module`, `pattern` (+`section`, `index`), `string`, `xref`, `function_start`, `symbol`, `export`,
`add`/`sub`, `rip [dispOffset, len]`, `call`, `deref`, `vfunc`, `rtti_vtable`, `rtti_instance`.
Kinds: `address`, `pointer` (re-evaluated each use after the first `deref`), `function`, `field`
(offset chain applied to an object), `offset`, `constant`.

## The script VM (most gameplay features)

Mafia: DE runs Havok Script (a Lua 5.1 dialect). Consigliere hooks the script machine's tick and runs
its Lua there, on the game's own script thread.

| Symbol | Kind | Signature / value |
|---|---|---|
| `Game.TickHook` | function | `int64 Tick(void* scriptMachine)`, hooked. **Required.** |
| `Lua.StateOffset` | constant | offset of `lua_State*` in the machine (0xD0 on known builds) |
| `Lua.State` | pointer | alternative: resolves straight to the `lua_State*` |
| `Lua.LoadBuffer` | function | `int (L, const char* buf, size_t len, const char* name)` **Required.** |
| `Lua.PCall` | function | `int (L, int nargs, int nresults, int errfunc)` **Required.** |
| `Lua.PushCClosure`, `Lua.SetField`, `Lua.CheckLString` | function | return values + print capture |
| `Lua.ToLString` | function | readable error messages |
| `Lua.ResetState` | function | readiness (returns 3 when ready) |
| `Lua.ApiTopOffset` / `Lua.ApiBaseOffset` / `Lua.ObjectSize` | constant | 0x48 / 0x50 / 16, stack restore |
| `Lua.PCallLock` | constant | 1 = serialise worker-thread pcalls with ours |

The shipped `bindings/mafia_de.json` contains **unverified candidates** for all of these, taken from
MIT-licensed community hooks (see THIRD_PARTY.md). They may or may not match your build.

### Verify the VM on your build

1. Start the game, load into free ride, open **Tools → Game Bindings**. If `Game.TickHook`,
   `Lua.LoadBuffer` and `Lua.PCall` are **Resolved**, open **Tools → Game Lua Console**: the status should
   read *Ready*.
2. Run `return game.game:GetActivePlayer():GetPos().x`. A number means the whole chain works. Mark those
   symbols `"verified": true` in a copy of the file.
3. If a pattern failed: find the function in a disassembler (or with **Tools → Signatures** → find string →
   xrefs), generate a new signature there, and use **Save as binding**, or edit the JSON and **Reload**.

## Memory symbols (optional, for memory-backed features)

`Player.Object` (pointer), `Human.Health` / `Human.HealthMax` / `Human.Vehicle` / `Entity.Position` /
`Vehicle.Health` / `Vehicle.HealthMax` / `Vehicle.Velocity` (fields), `Entity.SetPosition`,
`Vehicle.Repair` (functions), `World.TimeOfDay`, `World.TimeScale`, `Camera.ViewProjection`, `Camera.View`,
`Camera.Position`, `Camera.Fov` (pointers), `Camera.MatrixTransposed` (constant), `Game.IsLoading` (pointer).
The **What to bind** tab in Game Bindings lists every one with its purpose.

### Typical workflow: find the player's health

1. **Memory Scanner**: type `f32`, Exact `100`, First Scan.
2. Take damage, Next Scan → *Decreased*. Repeat until a handful of results remain.
3. Add the address to the **Address Table** and freeze it to confirm.
4. Open it in the **Hex View**, follow the pointer chain back to the player object (or scan for the address
   itself), then save the offset as a `field` binding (`Human.Health`, chain `["0x…"]`).
5. Make the result patch-proof by deriving `Player.Object` from a code signature (Signatures tool) rather
   than a raw address.

## Cheat table

Add CE-style cheats to `"cheats"` (format in `src/features/cheat_table.h`): `patch` (bytes / NOP),
`freeze`, `set`, `slider`, or `lua` (runs in the game VM). They appear in the menu under their category.
