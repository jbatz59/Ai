#include "core/mp_guard.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string_view>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include "core/log.h"
#include "core/util.h"

namespace cg::mp_guard {
namespace {

constexpr size_t kMaxModules = 4096;

struct State {
  std::mutex mutex;              // serializes Refresh
  std::string reason;            // written once, before `blocked` is published
  std::vector<HMODULE> modules;  // reused buffer
};

State& S() {
  static State* s = new State();
  return *s;
}

// Blocking is sticky: once a multiplayer client was seen in the process, gameplay writes stay
// refused for the rest of the session even if the module were to unload.
std::atomic<bool> g_blocked{false};
std::once_flag g_firstRefresh;

const std::string& EmptyString() {
  static const std::string* e = new std::string();
  return *e;
}

bool IsMultiplayerModule(const std::string& lowerName) {
  return lowerName.find("mafiamp") != std::string::npos || lowerName.find("mafiahub") != std::string::npos ||
         lowerName == "frameworkclient.dll";
}

// Arguments after the executable path; argv[0] is skipped because the install folder name is not
// evidence of anything.
std::string CommandLineArgsLower() {
  const wchar_t* cmd = GetCommandLineW();
  if (!cmd) return {};
  std::wstring_view s(cmd);
  size_t i = 0;
  if (!s.empty() && s[0] == L'"') {
    const size_t close = s.find(L'"', 1);
    i = close == std::wstring_view::npos ? s.size() : close + 1;
  } else {
    while (i < s.size() && s[i] != L' ' && s[i] != L'\t') ++i;
  }
  return util::ToLower(util::Narrow(s.substr(i)));
}

void Block(State& s, std::string reason) {
  s.reason = std::move(reason);
  g_blocked.store(true, std::memory_order_release);
  log::Warn("mp_guard", "multiplayer detected ({}); gameplay features, patches and memory writes are disabled",
            s.reason);
}

void RefreshImpl() {
  if (g_blocked.load(std::memory_order_acquire)) return;
  try {
    State& s = S();
    std::lock_guard lock(s.mutex);
    if (g_blocked.load(std::memory_order_relaxed)) return;

    const HANDLE process = GetCurrentProcess();
    if (s.modules.size() < 256) s.modules.resize(256);
    DWORD needed = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
      const DWORD bytes = static_cast<DWORD>(s.modules.size() * sizeof(HMODULE));
      if (!EnumProcessModules(process, s.modules.data(), bytes, &needed)) {
        needed = 0;
        break;
      }
      if (needed <= bytes) break;
      const size_t want = std::min<size_t>(needed / sizeof(HMODULE) + 32, kMaxModules);
      if (want <= s.modules.size()) break;
      s.modules.resize(want);
    }
    const size_t count = std::min<size_t>(needed / sizeof(HMODULE), s.modules.size());
    wchar_t name[MAX_PATH];
    for (size_t i = 0; i < count; ++i) {
      const DWORD len = GetModuleBaseNameW(process, s.modules[i], name, MAX_PATH);
      if (len == 0) continue;
      const std::string lower = util::ToLower(util::Narrow(std::wstring_view(name, len)));
      if (IsMultiplayerModule(lower)) {
        Block(s, "module " + lower + " is loaded");
        return;
      }
    }

    const std::string args = CommandLineArgsLower();
    for (const char* marker : {"mafiamp", "mafiahub"}) {
      if (args.find(marker) != std::string::npos) {
        Block(s, std::string("command line mentions ") + marker);
        return;
      }
    }
  } catch (...) {
    // Out of memory while enumerating: keep the previous verdict; the next Refresh retries.
  }
}

}  // namespace

void Refresh() {
  RefreshImpl();
}

bool Blocked() {
  // Writes must be refused even if nobody called Refresh yet.
  try {
    std::call_once(g_firstRefresh, RefreshImpl);
  } catch (...) {
  }
  return g_blocked.load(std::memory_order_acquire);
}

const std::string& Reason() {
  return g_blocked.load(std::memory_order_acquire) ? S().reason : EmptyString();
}

}  // namespace cg::mp_guard
