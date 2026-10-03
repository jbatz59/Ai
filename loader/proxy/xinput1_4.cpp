// xinput1_4.dll proxy. Mafia: Definitive Edition imports xinput1_4.dll, so a copy of this file in the
// game folder is loaded at startup. It forwards every export to the real System32 DLL and, from a
// worker thread (never under the loader lock), loads Consigliere.dll from the same folder — but
// only inside mafiadefinitiveedition.exe (or when CONSIGLIERE_ANY_PROCESS=1).
#include <windows.h>

#include <cstdint>
#include <cwchar>
#include <string>

namespace {

HMODULE g_self = nullptr;
HMODULE g_real = nullptr;
INIT_ONCE g_realOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*) {
  wchar_t path[MAX_PATH];
  const UINT n = GetSystemDirectoryW(path, MAX_PATH);
  if (n == 0 || n > MAX_PATH - 20) return TRUE;
  wcscat_s(path, L"\\xinput1_4.dll");
  g_real = LoadLibraryW(path);
  return TRUE;
}

FARPROC Real(const char* nameOrOrdinal) {
  InitOnceExecuteOnce(&g_realOnce, LoadReal, nullptr, nullptr);
  return g_real ? GetProcAddress(g_real, nameOrOrdinal) : nullptr;
}

std::wstring ModuleDir(HMODULE m) {
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(m, buf, static_cast<DWORD>(std::size(buf)));
  std::wstring p(buf, n);
  const auto slash = p.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

void LogLine(const std::wstring& dir, const char* text) {
  CreateDirectoryW((dir + L"\\Consigliere").c_str(), nullptr);
  HANDLE f = CreateFileW((dir + L"\\Consigliere\\loader.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(f, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
  WriteFile(f, "\r\n", 2, &written, nullptr);
  CloseHandle(f);
}

DWORD WINAPI LoaderThread(LPVOID) {
  InitOnceExecuteOnce(&g_realOnce, LoadReal, nullptr, nullptr);
  const std::wstring dir = ModuleDir(g_self);
  wchar_t exe[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
  std::wstring exeName(exe, n);
  exeName = exeName.substr(exeName.find_last_of(L"\\/") + 1);
  wchar_t any[8] = {};
  const bool anyProcess = GetEnvironmentVariableW(L"CONSIGLIERE_ANY_PROCESS", any, 8) && any[0] == L'1';
  if (!anyProcess && _wcsicmp(exeName.c_str(), L"mafiadefinitiveedition.exe") != 0) return 0;

  for (const wchar_t* name : {L"\\Consigliere.dll", L"\\Consigliere.asi"}) {
    const std::wstring path = dir + name;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    if (LoadLibraryW(path.c_str())) {
      LogLine(dir, "xinput1_4 proxy: Consigliere loaded");
    } else {
      char msg[96];
      snprintf(msg, sizeof(msg), "xinput1_4 proxy: LoadLibrary(Consigliere) failed, error %lu", GetLastError());
      LogLine(dir, msg);
    }
    return 0;
  }
  LogLine(dir, "xinput1_4 proxy: Consigliere.dll not found next to the game exe");
  return 0;
}

template <class Fn> Fn Resolve(const char* nameOrOrdinal) { return reinterpret_cast<Fn>(reinterpret_cast<void*>(Real(nameOrOrdinal))); }

}  // namespace

extern "C" {

// Signatures from xinput.h, written out to avoid depending on a specific SDK version.
DWORD WINAPI Proxy_XInputGetState(DWORD user, void* state) {
  auto fn = Resolve<DWORD(WINAPI*)(DWORD, void*)>("XInputGetState");
  return fn ? fn(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI Proxy_XInputSetState(DWORD user, void* vibration) {
  auto fn = Resolve<DWORD(WINAPI*)(DWORD, void*)>("XInputSetState");
  return fn ? fn(user, vibration) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI Proxy_XInputGetCapabilities(DWORD user, DWORD flags, void* caps) {
  auto fn = Resolve<DWORD(WINAPI*)(DWORD, DWORD, void*)>("XInputGetCapabilities");
  return fn ? fn(user, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
}
void WINAPI Proxy_XInputEnable(BOOL enable) {
  if (auto fn = Resolve<void(WINAPI*)(BOOL)>("XInputEnable")) fn(enable);
}
DWORD WINAPI Proxy_XInputGetBatteryInformation(DWORD user, BYTE devType, void* info) {
  auto fn = Resolve<DWORD(WINAPI*)(DWORD, BYTE, void*)>("XInputGetBatteryInformation");
  return fn ? fn(user, devType, info) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI Proxy_XInputGetKeystroke(DWORD user, DWORD reserved, void* keystroke) {
  auto fn = Resolve<DWORD(WINAPI*)(DWORD, DWORD, void*)>("XInputGetKeystroke");
  return fn ? fn(user, reserved, keystroke) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI Proxy_XInputGetAudioDeviceIds(DWORD user, LPWSTR render, UINT* renderCount, LPWSTR capture, UINT* captureCount) {
  auto fn = Resolve<DWORD(WINAPI*)(DWORD, LPWSTR, UINT*, LPWSTR, UINT*)>("XInputGetAudioDeviceIds");
  return fn ? fn(user, render, renderCount, capture, captureCount) : ERROR_DEVICE_NOT_CONNECTED;
}

// Undocumented ordinal-only exports (XInputGetStateEx, guide-button wait/cancel, power off,
// bus information, capabilities ex). All take at most four integer/pointer arguments, so a
// four-register pass-through forwards them exactly.
using Generic4 = uintptr_t(WINAPI*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
#define CG_ORDINAL_FORWARD(n)                                                                 \
  uintptr_t WINAPI Proxy_Ordinal##n(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {     \
    auto fn = Resolve<Generic4>(MAKEINTRESOURCEA(n));                                         \
    return fn ? fn(a, b, c, d) : static_cast<uintptr_t>(ERROR_DEVICE_NOT_CONNECTED);          \
  }
CG_ORDINAL_FORWARD(100)
CG_ORDINAL_FORWARD(101)
CG_ORDINAL_FORWARD(102)
CG_ORDINAL_FORWARD(103)
CG_ORDINAL_FORWARD(104)
CG_ORDINAL_FORWARD(108)

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_self = module;
    DisableThreadLibraryCalls(module);
    if (HANDLE t = CreateThread(nullptr, 0, LoaderThread, nullptr, 0, nullptr)) CloseHandle(t);
  }
  return TRUE;
}

}  // extern "C"
