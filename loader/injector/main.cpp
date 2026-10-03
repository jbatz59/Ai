// ChromaInjector: loads Chroma.dll into a running Mafia: Definitive Edition (single-player).
//   ChromaInjector [--pid N | --process name.exe] [--dll path] [--wait] [--timeout seconds]
// Exit codes: 0 ok, 1 usage, 2 process not found, 3 injection failed.
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <cstdio>
#include <cwchar>
#include <string>

namespace {

std::wstring ErrorText(DWORD code) {
  wchar_t* buf = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                 reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
  std::wstring s = buf ? buf : L"unknown error";
  if (buf) LocalFree(buf);
  while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
  return s + L" (" + std::to_wstring(code) + L")";
}

DWORD FindProcess(const std::wstring& name) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  DWORD pid = 0;
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
    if (_wcsicmp(pe.szExeFile, name.c_str()) == 0) {
      pid = pe.th32ProcessID;
      break;
    }
  }
  CloseHandle(snap);
  return pid;
}

bool AlreadyLoaded(HANDLE process, const std::wstring& dll) {
  HMODULE mods[1024];
  DWORD needed = 0;
  if (!EnumProcessModulesEx(process, mods, sizeof(mods), &needed, LIST_MODULES_64BIT)) return false;
  for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
    wchar_t path[MAX_PATH * 2];
    if (GetModuleFileNameExW(process, mods[i], path, static_cast<DWORD>(std::size(path))) && _wcsicmp(path, dll.c_str()) == 0) return true;
  }
  return false;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::wstring processName = L"mafiadefinitiveedition.exe";
  DWORD pid = 0;
  bool wait = false;
  DWORD timeoutMs = 15000;
  wchar_t self[MAX_PATH * 2];
  GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
  std::wstring dll = self;
  dll = dll.substr(0, dll.find_last_of(L"\\/") + 1) + L"Chroma.dll";

  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    auto next = [&]() -> const wchar_t* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (a == L"--pid") { if (auto v = next()) pid = static_cast<DWORD>(_wtoi(v)); }
    else if (a == L"--process") { if (auto v = next()) processName = v; }
    else if (a == L"--dll") { if (auto v = next()) dll = v; }
    else if (a == L"--wait") wait = true;
    else if (a == L"--timeout") { if (auto v = next()) timeoutMs = static_cast<DWORD>(_wtoi(v)) * 1000; }
    else {
      fwprintf(stderr, L"usage: ChromaInjector [--pid N | --process name.exe] [--dll path] [--wait] [--timeout seconds]\n");
      return 1;
    }
  }
  wchar_t full[MAX_PATH * 2];
  if (!GetFullPathNameW(dll.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr) ||
      GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
    fwprintf(stderr, L"DLL not found: %ls\n", dll.c_str());
    return 1;
  }
  dll = full;
  wprintf(L"Chroma injector - for single-player use only.\n");

  if (!pid) {
    pid = FindProcess(processName);
    if (!pid && wait) {
      wprintf(L"Waiting for %ls...\n", processName.c_str());
      while (!(pid = FindProcess(processName))) Sleep(500);
      Sleep(5000);   // let the game create its window and device
    }
  }
  if (!pid) {
    fwprintf(stderr, L"Process %ls not found (start the game first or use --wait)\n", processName.c_str());
    return 2;
  }

  HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                   PROCESS_VM_READ, FALSE, pid);
  if (!process) {
    fwprintf(stderr, L"OpenProcess failed: %ls\n", ErrorText(GetLastError()).c_str());
    return 3;
  }
  BOOL wow64 = FALSE;
  if (IsWow64Process(process, &wow64) && wow64) {
    fwprintf(stderr, L"Target is a 32-bit process; Chroma is 64-bit only\n");
    CloseHandle(process);
    return 3;
  }
  if (AlreadyLoaded(process, dll)) {
    wprintf(L"Chroma is already loaded in process %lu\n", pid);
    CloseHandle(process);
    return 0;
  }

  const size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
  void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  int rc = 3;
  if (!remote || !WriteProcessMemory(process, remote, dll.c_str(), bytes, nullptr)) {
    fwprintf(stderr, L"Writing the DLL path failed: %ls\n", ErrorText(GetLastError()).c_str());
  } else {
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (!thread) {
      fwprintf(stderr, L"CreateRemoteThread failed: %ls\n", ErrorText(GetLastError()).c_str());
    } else {
      if (WaitForSingleObject(thread, timeoutMs) != WAIT_OBJECT_0) {
        fwprintf(stderr, L"Timed out waiting for LoadLibrary in the target\n");
      } else {
        DWORD code = 0;
        GetExitCodeThread(thread, &code);
        if (code) {
          wprintf(L"Injected %ls into process %lu\n", dll.c_str(), pid);
          rc = 0;
        } else {
          fwprintf(stderr, L"LoadLibrary failed inside the target (missing file, or blocked)\n");
        }
      }
      CloseHandle(thread);
    }
  }
  if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
  CloseHandle(process);
  return rc;
}
