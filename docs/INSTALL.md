# Installing Chroma

Everything goes into the folder that contains `mafiadefinitiveedition.exe`
(Steam: `steamapps\common\Mafia Definitive Edition\`). Single-player only.

## Option 1 — xinput1_4.dll proxy (recommended)

Copy `Chroma.dll` and `xinput1_4.dll` into the game folder. The game imports `xinput1_4.dll`, so
Windows loads our proxy from the game folder first. The proxy forwards every XInput call to the real
`C:\Windows\System32\xinput1_4.dll` and loads `Chroma.dll` from a worker thread. It only does that inside
`mafiadefinitiveedition.exe`. Proxy messages go to `Chroma\loader.log`.

## Option 2 — Ultimate ASI Loader

If you already use [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (MIT), rename
`Chroma.dll` to `Chroma.asi` and put it where your ASI loader looks (game folder or `scripts\`).
Don't also install our `xinput1_4.dll`. A second copy would refuse to load anyway, but there's no point.

## Option 3 — Injector

Start the game, wait for the main menu, then run `ChromaInjector.exe` (same folder as
`Chroma.dll`). `--wait` waits for the game to start; `--pid N` targets a specific process.

## Conflicts

- **ReShade, DXVK (Windows 7 fix), RenoDX, the R.E.A.L. VR mod** use `dxgi.dll` / `d3d11.dll`. They coexist
  with Chroma. Our Present hook chains with theirs, and Chroma draws on top of their effects.
- **Another `xinput1_4.dll`** in the game folder (rare): use Option 2 or 3 instead.
- **NOMAD ScriptHook / other script hooks**: both may hook the game's script machine; if the game misbehaves,
  use one at a time.
- **RTSS / overlays**: if the game starts with a black screen, try disabling other overlays first.
- **Microsoft Store / Game Pass build**: untested (protected install folder).

## Files Chroma creates

`Chroma\` next to the DLL:

| Path | What |
|---|---|
| `chroma.log` | log (attach it to bug reports) |
| `config.json` | settings, hotkeys, enabled features |
| `bindings\` | binding files (`mafia_de.json` shipped, `zz_user_overrides.json` for your pins) |
| `profiles\`, `tables\` | saved profiles, address tables |
| `crash\` | crash reports + minidumps (only if Chroma itself faults) |

## Uninstall

Delete `Chroma.dll`, `xinput1_4.dll` (Option 1) and the `Chroma\` folder. Nothing else is touched;
game files and saves are never modified.

## Troubleshooting

1. Open `Chroma\chroma.log`. The first lines identify your game build (PE timestamp/checksum).
2. Menu doesn't appear: check the log for "D3D11 hooks installed" and "Overlay initialised". If the log
   doesn't exist, the DLL never loaded: check `loader.log`, or use the injector.
3. Features greyed out: hover them for the reason, or open **Tools → Game Bindings**. Most gameplay
   features need the script-VM bindings (`Game.TickHook`, `Lua.LoadBuffer`, `Lua.PCall`); see
   [BINDINGS.md](BINDINGS.md).
