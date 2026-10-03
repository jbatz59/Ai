# Architecture

Consigliere is a DLL that lives inside `mafiadefinitiveedition.exe`. It draws a Dear ImGui menu on
top of the game's Direct3D 11 swap chain and talks to game memory through a **bindings** layer
that is loaded from JSON at runtime.

```
loader/        version.dll / dinput8.dll proxies + ConsigliereInjector.exe  -> load Consigliere.dll
src/core/      bootstrap, logging, config, hotkeys, task queues, crash reports, multiplayer guard
src/mem/       safe memory access, patterns, RTTI, hooks, patches, HW breakpoints, scanner, disasm
src/render/    D3D11 Present hook, ImGui overlay, input capture, theme, fonts, world drawing
src/game/      bindings (JSON -> addresses), typed game facade, bridge into the game's Lua VM
src/features/  Feature model + registry, built-in features, data-driven cheat table
src/ui/        menu shell, pages, HUD, command palette, toasts, RE toolkit windows (ui/tools)
src/script/    embedded Lua 5.4 for user scripts (Consigliere/scripts/*.lua)
bindings/      shipped binding files (game-version specific knowledge lives ONLY here)
```

## Layering (lower layers never include higher ones)

```
core  <-  mem  <-  game  <-  features  <-  ui
                    ^           ^          ^
render (overlay orchestrates everything; ui/features may use render/theme, render/draw, overlay getters)
script (uses everything below ui; ui hosts its console)
```

## Threads

| Thread | What runs there |
|---|---|
| Init thread (from DllMain) | paths/log/crash/config, MinHook init, waits for the game window, installs D3D11 hooks, kicks off bindings resolution via `tasks::RunAsync` |
| Render thread (game's Present) | everything per-frame: hotkeys, `tasks::DrainRender`, feature ticks, script ticks, ImGui UI |
| Game thread | `tasks::DrainGame` when the `Game.TickHook` binding resolves (else drained on the render thread) |
| Worker pool | bindings resolution, memory scans, RTTI indexing, instance searches |
| Unload thread | disables features, restores patches, removes hooks, frees the DLL |

Rules:
- Never block the render thread for more than ~1 ms. Anything slow goes to `tasks::RunAsync`.
- Never hold a mutex while calling into ImGui or into another module's API that may lock.
- The Lua VM (script engine) is touched only from the render thread.

## Safety rules (non-negotiable)

1. All game memory access goes through `mem::Read/Write/Deref` (`mem/safe.h`). Raw pointer
   dereferences of game memory are forbidden.
2. Every game-specific address comes from `game::Bindings`. No hard-coded offsets in C++.
3. A feature lists the bindings it needs in `Requires()`; if any is missing it is greyed out
   with the reason. Nothing silently fails.
4. `mp_guard` blocks every gameplay write when a multiplayer client is detected. Single-player only.
5. Unload must leave the game exactly as found: patches restored, hooks removed, cursor/input
   released, features disabled.
6. No C++ exceptions escape a module boundary, a hook, a WndProc, or a Lua C function. nlohmann::json
   access is wrapped (`value()`, `contains()`, or try/catch).

## Code conventions

- C++20, MinGW-w64 (GCC 13) **and** MSVC must both compile it. No `__try` without `#ifdef _MSC_VER`.
- Namespace `cg::<module>`. Files `snake_case.cpp`, types `PascalCase`, functions `PascalCase`,
  locals `camelCase`, members `trailing_`.
- `#include` order: own header, std, windows, third-party, project.
- Log via `cg::log::Info("channel", "fmt {}", x)`; channel = module name.
- Dear ImGui is **1.92.x** (new dynamic font system: `ImGui::PushFont(font, size)`; use
  `ImGui::GetFontSize()`; `ImFontConfig` sizing via `AddFontFromFileTTF(path, size)`).
- UI colors come from `render::theme::Colors()`; never hard-code RGBA in pages.
- Type-check a file quickly with `tools/check.sh path/to/file.cpp`.
