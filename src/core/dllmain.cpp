#include <windows.h>

#include "core/bootstrap.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    // Refuse a second copy (e.g. loaded both as Chroma.dll and Chroma.asi).
    return cg::Bootstrap(module) ? TRUE : FALSE;
  }
  return TRUE;
}
