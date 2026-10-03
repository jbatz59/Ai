# Chroma

A single-player mod menu and reverse-engineering toolkit for **Mafia: Definitive Edition** (PC, x64, DirectX 11).
It runs as a DLL inside the game and draws a Dear ImGui menu on top of it.

> **Status — read this first.** Chroma is built and tested in an automated stand-in "game" (a D3D11
> test host under Wine). It has **not** been run against the real game by its authors. Everything
> game-specific (where the game's script engine lives in memory, which patterns match) is loaded from
> `bindings/*.json` and is **unverified** for current game builds. Features whose bindings don't resolve are
> greyed out with the exact reason; nothing silently pretends to work. See [docs/BINDINGS.md](docs/BINDINGS.md)
> for how to confirm or fix bindings on your copy with the built-in tools.

## What's in it

**Gameplay** — driven through the game's own Havok Script (Lua 5.1-style) API, which survives patches far
better than raw memory offsets:

- Player: god mode, invisibility, noclip, outfit swap
- Weapons: unlimited ammo, refill, grenades, give weapon by name (+ "remember what I'm holding")
- Vehicle: repair, indestructible, boost, keep car (no despawn), colours / tint / plate / dirt / rust, spawn by model name
- World: time of day, time flow, weather ("remember current" + favourites), police off, traffic, water
- Teleport: saved locations, quick slot, coordinates, jump forward
- Camera / visuals: FOV (memory binding), hide HUD, HUD elements, speedometer and coordinate overlays
- **Fun & Chaos**: 1930s film-reel overlay, spinning "Extra! Extra!" newspaper headlines, art-deco
  speedometer, rewind-your-route, and chaos mode
- Cheat table: add your own Cheat-Engine-style patches/freezes as JSON — no recompile

**Reverse-engineering toolkit** (to find or fix bindings yourself): memory scanner, hex viewer with
disassembly, address table with freezing, signature generator / tester / xref finder, module + export
browser, bindings status, log console, and a console that runs code inside the game's own script VM.

**Menu**: iOS-style dark theme (Chroma RGB and classic themes selectable), command palette (Ctrl+P), per-feature hotkeys, favourites, profiles, toasts,
gamepad navigation, panic key, clean unload. Multiplayer guard: if a multiplayer client (MafiaMP) is
detected, Chroma refuses to modify anything.

There is no money cheat: Mafia: DE has no money system.

## Install (short)

1. Build (below) or take `Chroma.dll` and `xinput1_4.dll` from a release.
2. Copy both next to `mafiadefinitiveedition.exe`. The game loads `xinput1_4.dll`, which loads Chroma.
3. Start the game and press **Insert**.

Alternatives (Ultimate ASI Loader, the injector) and conflicts with ReShade / other mods:
[docs/INSTALL.md](docs/INSTALL.md).

Default keys: **Insert** menu, **Ctrl+P** palette (menu open), **Ctrl+Shift+P** panic (all features off),
**Ctrl+Shift+End** unload. All rebindable in Settings.

## Build

Windows (Visual Studio 2022):

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Linux cross-compile (MinGW-w64):

```
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
cmake --build build
```

Outputs: `Chroma.dll`, `loader/xinput1_4.dll`, `loader/ChromaInjector.exe`, `tests/cg_tests.exe`,
`tests/cg_testhost.exe`.

## Docs

- [docs/INSTALL.md](docs/INSTALL.md) — install options, conflicts, uninstall, troubleshooting
- [docs/BINDINGS.md](docs/BINDINGS.md) — binding file format and how to verify bindings on your build
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — code layout, threads, safety rules

## Credits

See [THIRD_PARTY.md](THIRD_PARTY.md). MIT licensed ([LICENSE](LICENSE)).
