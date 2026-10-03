#include "core/bootstrap.h"

#include <atomic>
#include <string>

#include "cg_version.h"
#include "core/config.h"
#include "core/crash.h"
#include "core/log.h"
#include "core/mp_guard.h"
#include "core/paths.h"
#include "core/tasks.h"
#include "core/util.h"
#include "features/feature.h"
#include "game/bindings.h"
#include "game/script_vm.h"
#include "mem/hook.h"
#include "mem/hwbp.h"
#include "mem/module.h"
#include "mem/patch.h"
#include "mem/rtti.h"
#include "render/d3d11_hook.h"
#include "render/input.h"
#include "render/overlay.h"

namespace cg {
namespace {

HMODULE g_self = nullptr;
HANDLE g_instanceMutex = nullptr;
std::atomic<bool> g_unloading{false};
std::atomic<bool> g_ready{false};
std::atomic<const char*> g_status{"Starting"};

void ResolveBindings() {
  g_status.store("Resolving bindings");
  const auto started = util::NowMs();
  game::Bindings::Get().LoadDirectory(paths::Data(L"bindings"));
  size_t ok = 0, total = 0;
  for (const auto& s : game::Bindings::Get().List()) {
    ++total;
    if (s.status == game::SymbolStatus::Resolved || s.status == game::SymbolStatus::Overridden) ++ok;
  }
  log::Info("bindings", "{}/{} symbols resolved in {} ms", ok, total, util::NowMs() - started);
  if (g_unloading.load()) return;
  game::vm::Install();
  // Feature state is restored on the render thread so it never races Registry::Tick.
  tasks::PostRender([] {
    auto& registry = features::Registry::Get();
    features::SyncCheatTableFeatures(registry);
    registry.Load();
  });
  g_ready.store(true);
  g_status.store("Ready");

  // Self-check for the RE tools: does this build keep MSVC RTTI? (Reported absent in Mafia: DE.)
  const auto& classes = mem::rtti::Index(mem::Module::Main());
  log::Info("mem", "RTTI self-check: {} polymorphic classes with type descriptors in {}", classes.size(),
            mem::Module::Main().name);
}

bool WaitForGameWindow() {
  g_status.store("Waiting for game window");
  while (!g_unloading.load()) {
    HWND found = nullptr;
    EnumWindows(
        [](HWND h, LPARAM param) -> BOOL {
          DWORD owner = 0;
          GetWindowThreadProcessId(h, &owner);
          RECT rc{};
          if (owner == static_cast<DWORD>(GetCurrentProcessId()) && IsWindowVisible(h) && !GetWindow(h, GW_OWNER) &&
              GetClientRect(h, &rc) && rc.right - rc.left >= 320 && rc.bottom - rc.top >= 200) {
            *reinterpret_cast<HWND*>(param) = h;
            return FALSE;
          }
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    if (found) return true;
    Sleep(250);
  }
  return false;
}

DWORD WINAPI InitThread(LPVOID) {
  paths::Init(g_self);
  log::Init(paths::Data(L"consigliere.log"));
  log::Info("core", "{} {} starting (host: {})", CG_NAME, CG_VERSION, util::Narrow(paths::GameExe().filename().wstring()));
  crash::Install();
  Config::Get().Load(paths::Data(L"config.json"));
  log::SetMinLevel(static_cast<log::Level>(Config::Get().ReadInt("log.level", static_cast<int>(log::Level::Info))));

  mp_guard::Refresh();
  if (mp_guard::Blocked()) {
    g_status.store("Blocked: multiplayer");
    log::Error("core", "Multiplayer client detected ({}). Consigliere is single-player only and will stay inactive.",
               mp_guard::Reason());
    return 0;
  }

  if (!mem::Hooks::Get().Init()) {
    g_status.store("Failed: hook engine");
    log::Error("core", "MinHook initialisation failed");
    return 0;
  }
  features::Registry::Get().RegisterBuiltins();

  if (!WaitForGameWindow()) return 0;
  // The game creates its device right after its window; our probe device is independent of it.
  if (!render::InstallD3D11Hooks()) {
    g_status.store("Failed: D3D11 hook");
    log::Error("core", "Could not hook Direct3D 11 Present; the overlay is unavailable");
    return 0;
  }
  tasks::RunAsync(ResolveBindings);
  return 0;
}

DWORD WINAPI UnloadThread(LPVOID) {
  log::Info("core", "Unloading");
  g_status.store("Unloading");

  // 1. Render-thread teardown: features off, scripts closed, UI + ImGui + input released.
  render::Shutdown();
  if (!render::Initialized()) features::Registry::Get().DisableAll();

  // 2. Undo everything we did to the game.
  mem::hwbp::Shutdown();
  mem::Patches::Get().RestoreAll();
  game::vm::Uninstall();
  render::RemoveD3D11Hooks();

  tasks::ShutdownPool();
  mem::Hooks::Get().Shutdown();
  Config::Get().Save();

  const bool safe = render::input::SafeToFree() && render::ThreadsInsideDetours() == 0 && game::vm::ThreadsInside() == 0;
  log::Info("core", "{}", safe ? "Unloaded cleanly" : "Unloaded; staying resident because another module still references our code");
  crash::Uninstall();
  log::Shutdown();
  if (g_instanceMutex) {
    CloseHandle(g_instanceMutex);
    g_instanceMutex = nullptr;
  }
  if (!safe) return 0;
  Sleep(100);   // let any thread that just left a detour finish its epilogue
  FreeLibraryAndExitThread(g_self, 0);
}

}  // namespace

bool Bootstrap(HMODULE self) {
  g_self = self;
  // One instance per process (e.g. both an ASI loader and the version.dll proxy present).
  const std::wstring name = L"Local\\Consigliere." + std::to_wstring(GetCurrentProcessId());
  g_instanceMutex = CreateMutexW(nullptr, FALSE, name.c_str());
  if (g_instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(g_instanceMutex);
    g_instanceMutex = nullptr;
    return false;
  }
  if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
  return true;
}

void RequestUnload() {
  if (g_unloading.exchange(true)) return;
  if (HANDLE t = CreateThread(nullptr, 0, UnloadThread, nullptr, 0, nullptr)) CloseHandle(t);
}

bool IsUnloading() { return g_unloading.load(); }
bool IsReady() { return g_ready.load(); }
const char* StatusText() { return g_status.load(); }

}  // namespace cg
