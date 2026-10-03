#pragma once
#include <windows.h>

namespace cg {

// Called from DllMain(DLL_PROCESS_ATTACH). Never blocks: spawns the init thread and returns.
// Returns false if another Chroma instance is already loaded in this process.
bool Bootstrap(HMODULE self);

// Ask Chroma to unload itself (menu button / End hotkey). Safe from any thread. The unload
// thread disables features, restores patches, removes hooks, then FreeLibraryAndExitThread().
void RequestUnload();
bool IsUnloading();

// True once rendering hooks are live and bindings finished their first resolution pass.
bool IsReady();

// State string for the UI ("Waiting for game window", "Resolving bindings", "Ready", "Blocked: multiplayer").
const char* StatusText();

}  // namespace cg
