#include "core/paths.h"

#include <mutex>
#include <string>
#include <system_error>

#include "core/log.h"
#include "core/util.h"

namespace cg::paths {
namespace {

struct State {
  HMODULE self = nullptr;
  std::filesystem::path moduleDir;
  std::filesystem::path dataDir;
  std::filesystem::path gameExe;
};

State& S() {
  static State* s = new State();
  return *s;
}

std::once_flag g_once;

// Handles paths beyond MAX_PATH: grows the buffer until the name fits (Windows caps at 32767 chars).
std::wstring ModulePath(HMODULE module) {
  std::wstring buf(MAX_PATH, L'\0');
  for (int attempt = 0; attempt < 8; ++attempt) {
    const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    if (n < buf.size()) {
      buf.resize(n);
      return buf;
    }
    if (buf.size() >= 65536) return {};
    buf.resize(buf.size() * 2);
  }
  return {};
}

HMODULE ModuleOfThisCode() {
  HMODULE h = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&ModuleOfThisCode), &h);
  return h;
}

void Compute(HMODULE self) {
  State& s = S();
  try {
    s.self = self ? self : ModuleOfThisCode();
    const std::wstring selfPath = ModulePath(s.self);
    if (!selfPath.empty()) s.moduleDir = std::filesystem::path(selfPath).parent_path();
    const std::wstring exePath = ModulePath(nullptr);
    if (!exePath.empty()) s.gameExe = std::filesystem::path(exePath);
    if (s.moduleDir.empty()) return;
    s.dataDir = s.moduleDir / L"Chroma";

    std::error_code ec;
    std::filesystem::create_directories(s.dataDir, ec);
    if (ec) {
      log::Error("core", "cannot create data folder '{}': {}", util::Narrow(s.dataDir.wstring()), ec.message());
      return;
    }
    for (const wchar_t* sub : {L"bindings", L"scripts", L"profiles", L"tables", L"crash", L"screenshots"}) {
      std::filesystem::create_directories(s.dataDir / sub, ec);
      if (ec) log::Warn("core", "cannot create folder '{}': {}", util::Narrow((s.dataDir / sub).wstring()), ec.message());
    }
  } catch (...) {
    // Path state stays partially filled (possibly empty); callers then get empty/relative paths
    // rather than an exception escaping into DllMain/bootstrap.
  }
}

const State& Ready() {
  try {
    std::call_once(g_once, Compute, nullptr);
  } catch (...) {
  }
  return S();
}

}  // namespace

void Init(HMODULE self) {
  try {
    std::call_once(g_once, Compute, self);
  } catch (...) {
  }
}

HMODULE Self() { return Ready().self; }
const std::filesystem::path& ModuleDir() { return Ready().moduleDir; }
const std::filesystem::path& DataDir() { return Ready().dataDir; }
const std::filesystem::path& GameExe() { return Ready().gameExe; }

std::filesystem::path Data(std::wstring_view rel) {
  try {
    return Ready().dataDir / rel;
  } catch (...) {
    return {};
  }
}

}  // namespace cg::paths
