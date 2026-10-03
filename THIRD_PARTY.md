# Third-party code and credits

## Vendored libraries (in `third_party/`)

| Library | Version | License |
|---|---|---|
| Dear ImGui | 1.92.9b | MIT, Copyright (c) 2014-2026 Omar Cornut |
| MinHook | master (2024) | BSD-2-Clause, Copyright (C) 2009-2017 Tsuda Kageyu |
| nlohmann/json | 3.12.0 | MIT, Copyright (c) 2013-2026 Niels Lohmann |
| Lua | 5.4.8 | MIT, Copyright (C) 1994-2025 Lua.org, PUC-Rio (vendored for future scripting; not active in this release) |
| stb_image / stb_image_write | 2.30 / 1.16 | Public domain or MIT (dual), Sean Barrett |
| Inter (font, embedded) | variable, google/fonts | SIL Open Font License 1.1, Copyright 2020 The Inter Project Authors |

Full license texts are next to each library.

## Knowledge and binding candidates

- **Kamzik123/Mafia3ScriptHook** (MIT, Copyright (c) 2026 Kamzik123) — Havok Script function patterns
  and the script-machine tick / pcall-lock approach for Mafia III: DE. Used as unverified candidates in
  `bindings/mafia_de.json`.
- **MartinJK/Mafia-Definitive-Edition-ScriptHook**, M1DE component (MIT, Copyright (c) 2020 Martin K.) —
  2020-era Mafia: DE call-site patterns, the lua_State offset and the ApiStack layout. Used as fallbacks.
- **NOMAD Group/mafia-de-scripthook-trainer** (MIT, Copyright (c) 2020 NOMAD Group) — which game script
  functions exist (GetActivePlayer, SetDemigod, InventorySetUnlimitedAmmo, police, weather, vehicle calls).

No code, patterns, offsets or layouts from MafiaHub/MafiaMP (restrictive license) were used.
