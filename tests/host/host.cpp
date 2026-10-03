// Chroma test host: a stand-in "game" for end-to-end tests without Mafia: Definitive Edition.
//
//  * creates a D3D11 window + swap chain and presents ~60 fps
//  * plants a fake player/world in .data behind a unique byte marker and writes a bindings file
//    describing it (so bindings -> features -> UI run for real)
//  * loads Chroma.dll (overlay hooks Present like in the game)
//  * optional: screenshot via CHROMA_TEST_SCREENSHOT, unload via CHROMA_TEST_UNLOAD_FRAME
//
// Usage: cg_testhost.exe [--frames N] [--dll path] [--screenshot out.bmp] [--screenshot-frame N]
//                        [--open-menu] [--unload-frame N] [--config file.json] [--fake-vm] [--fake-vm-nostate] [--fake-vm-noglobal]
//                        [--exhaust-near]
#include <windows.h>

#include <d3d11.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "fake_vm.h"

namespace fs = std::filesystem;

struct FakeVec3 {
  float x, y, z;
};
struct FakeHuman {
  void* vtable;       // 0x00
  float health;       // 0x08
  float healthMax;    // 0x0C
  void* vehicle;      // 0x10
  FakeVec3 position;  // 0x18
};
struct Anchor {
  char magic[16];       // 0x00 "CGHOST_ANCHOR_1\0"
  FakeHuman* player;    // 0x10
  float timeOfDay;      // 0x18
  float timeScale;      // 0x1C
};

static FakeHuman g_player = {nullptr, 100.0f, 100.0f, nullptr, {-1234.5f, 456.25f, 12.0f}};
// volatile + used every frame so the optimiser keeps the marker in .data.
static volatile Anchor g_anchor = {"CGHOST_ANCHOR_1", &g_player, 12.0f, 1.0f};

static const char* kBindings = R"json({
  "schema": 1,
  "game_build": "Chroma test host",
  "symbols": {
    "Host.Anchor":     { "kind": "address", "steps": [ { "pattern": "43 47 48 4F 53 54 5F 41 4E 43 48 4F 52 5F 31 00", "section": "any" } ], "verified": true, "notes": "test host marker" },
    "Player.Object":   { "kind": "pointer", "steps": [ { "symbol": "Host.Anchor" }, { "add": "0x10" }, { "deref": true } ], "verified": true },
    "Human.Health":    { "kind": "field", "chain": [ "0x8" ], "type": "f32", "verified": true },
    "Human.HealthMax": { "kind": "field", "chain": [ "0xC" ], "type": "f32", "verified": true },
    "Human.Vehicle":   { "kind": "field", "chain": [ "0x10" ], "verified": true },
    "Entity.Position": { "kind": "field", "chain": [ "0x18" ], "type": "f32", "verified": true },
    "World.TimeOfDay": { "kind": "address", "steps": [ { "symbol": "Host.Anchor" }, { "add": "0x18" } ], "type": "f32", "verified": true },
    "World.TimeScale": { "kind": "address", "steps": [ { "symbol": "Host.Anchor" }, { "add": "0x1C" } ], "type": "f32", "verified": true }
  },
  "cheats": [
    { "id": "host_time_noon", "name": "Set noon (host)", "category": "World", "type": "set",
      "target": { "symbol": "World.TimeOfDay" }, "value_type": "f32", "value": "12" }
  ]
})json";

static bool g_running = true;

// Like Mafia: DE, whose reservations fill the address space around its image: reserve every free
// region within ±2 GB of the exe, so MinHook cannot place a trampoline near exe code.
static size_t ExhaustNearMemory() {
  const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  const uintptr_t lo = base > 0x90000000ull ? base - 0x80000000ull : 0x10000ull;
  const uintptr_t hi = base + 0x80000000ull;
  size_t regions = 0;
  for (uintptr_t p = lo; p < hi;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) break;
    const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress), re = rb + mbi.RegionSize;
    if (mbi.State == MEM_FREE) {
      const uintptr_t a = (rb + 0xFFFF) & ~static_cast<uintptr_t>(0xFFFF);
      const uintptr_t e = (re < hi ? re : hi) & ~static_cast<uintptr_t>(0xFFFF);
      if (e > a && VirtualAlloc(reinterpret_cast<void*>(a), e - a, MEM_RESERVE, PAGE_NOACCESS)) ++regions;
    }
    if (re <= p) break;
    p = re;
  }
  return regions;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_CLOSE || m == WM_DESTROY) {
    g_running = false;
    return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

int wmain(int argc, wchar_t** argv) {
  int frames = 300;
  fs::path exeDir = [] {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
  }();
  fs::path dll = exeDir / L"Chroma.dll";
  fs::path configFile;
  bool fakeVm = false, fakeVmNoState = false, fakeVmNoGlobal = false, exhaustNear = false;
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
    if (a == L"--frames") frames = _wtoi(next().c_str());
    else if (a == L"--dll") dll = next();
    else if (a == L"--screenshot") SetEnvironmentVariableW(L"CHROMA_TEST_SCREENSHOT", next().c_str());
    else if (a == L"--screenshot-frame") SetEnvironmentVariableW(L"CHROMA_TEST_SCREENSHOT_FRAME", next().c_str());
    else if (a == L"--open-menu") SetEnvironmentVariableW(L"CHROMA_TEST_OPEN_MENU", L"1");
    else if (a == L"--unload-frame") SetEnvironmentVariableW(L"CHROMA_TEST_UNLOAD_FRAME", next().c_str());
    else if (a == L"--config") configFile = next();
    else if (a == L"--fake-vm") fakeVm = true;
    else if (a == L"--fake-vm-nostate") fakeVm = fakeVmNoState = true;
    else if (a == L"--fake-vm-noglobal") fakeVm = fakeVmNoGlobal = true;   // reproduces 1.0.2 (main state only)
    else if (a == L"--exhaust-near") exhaustNear = true;   // reproduces MH_ERROR_MEMORY_ALLOC (Mafia DE)
  }

  // Data folder lives next to the DLL: <dll dir>/Chroma/
  const fs::path dataDir = fs::absolute(dll).parent_path() / L"Chroma";
  std::error_code ec;
  fs::create_directories(dataDir / L"bindings", ec);
  std::ofstream(dataDir / L"bindings" / L"test_host.json", std::ios::binary) << kBindings;
  if (fakeVm) {
    if (!FakeVmInit(!fakeVmNoState)) return 4;
    std::ofstream(dataDir / L"bindings" / L"test_host_vm.json", std::ios::binary) << FakeVmBindings(!fakeVmNoGlobal);
  } else {
    fs::remove(dataDir / L"bindings" / L"test_host_vm.json", ec);
  }
  if (!configFile.empty()) {   // stream copy: fs::copy_file is unreliable under MinGW + Wine
    std::ifstream in(configFile, std::ios::binary);
    std::ofstream(dataDir / L"config.json", std::ios::binary | std::ios::trunc) << in.rdbuf();
  }

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.lpszClassName = L"ChromaTestHost";
  RegisterClassExW(&wc);
  RECT rc{0, 0, 1280, 720};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Chroma Test Host", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
                              rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
  ShowWindow(hwnd, SW_SHOW);
  SetForegroundWindow(hwnd);

  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Width = 1280;
  sd.BufferDesc.Height = 720;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  IDXGISwapChain* sc = nullptr;
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc,
                                             &dev, nullptr, &ctx);
  if (FAILED(hr)) {
    std::printf("host: D3D11CreateDeviceAndSwapChain failed 0x%08lx\n", static_cast<unsigned long>(hr));
    return 2;
  }
  ID3D11Texture2D* bb = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
  dev->CreateRenderTargetView(bb, nullptr, &rtv);
  bb->Release();

  if (exhaustNear) std::printf("host: reserved %zu free region(s) within 2 GB of the exe\n", ExhaustNearMemory());
  HMODULE mod = LoadLibraryW(dll.c_str());
  std::printf("host: LoadLibrary(%ls) -> %p (err %lu)\n", dll.c_str(), static_cast<void*>(mod), mod ? 0ul : GetLastError());
  if (!mod) return 3;
  const std::wstring dllName = dll.filename().wstring();
  int demigodFrame = -1, unbalanced = 0, gameErrors = 0;

  for (int f = 0; f < frames && g_running; ++f) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    // "Gameplay": the world clock advances and the player slowly bleeds health.
    g_anchor.timeOfDay = g_anchor.timeOfDay + 0.01f * g_anchor.timeScale;
    if (g_anchor.timeOfDay >= 24.0f) g_anchor.timeOfDay = 0.0f;
    g_player.health = g_player.health > 1.0f ? g_player.health - 0.05f : 100.0f;
    if (fakeVm) {
      const FakeVmFrame vf = FakeVmTick();
      if (!vf.stackBalanced) ++unbalanced;
      if (vf.status != 0) ++gameErrors;
      if (vf.demigod && demigodFrame < 0) demigodFrame = f;
    }

    const float t = static_cast<float>(f) / 120.0f;
    const float clear[4] = {0.10f + 0.05f * (t - static_cast<int>(t)), 0.11f, 0.14f, 1.0f};
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->ClearRenderTargetView(rtv, clear);
    sc->Present(0, 0);
    Sleep(16);
  }

  const bool stillLoaded = GetModuleHandleW(dllName.c_str()) != nullptr;
  std::printf("host: frames done; chroma_loaded=%d health=%.2f time=%.2f\n", stillLoaded ? 1 : 0, g_player.health,
              g_anchor.timeOfDay);
  if (fakeVm) {
    std::printf("host: fakevm demigod=%d first_frame=%d unbalanced_frames=%d game_pcall_errors=%d\n", demigodFrame >= 0 ? 1 : 0,
                demigodFrame, unbalanced, gameErrors);
    if (!stillLoaded) std::printf("host: fakevm cleaned_up=%d\n", FakeVmCleanedUp() ? 1 : 0);
  }
  rtv->Release();
  ctx->Release();
  sc->Release();
  dev->Release();
  DestroyWindow(hwnd);
  return 0;
}
