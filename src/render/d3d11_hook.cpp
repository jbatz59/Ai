#include "render/d3d11_hook.h"

#include <atomic>

#include <d3d11.h>
#include <dxgi1_2.h>

#include "core/log.h"
#include "mem/hook.h"
#include "render/overlay.h"

namespace cg::render {
namespace {

using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(WINAPI*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

// vtable slots (IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7, IDXGISwapChain 8-17, IDXGISwapChain1 18-28)
constexpr size_t kPresentSlot = 8;
constexpr size_t kResizeBuffersSlot = 13;
constexpr size_t kPresent1Slot = 22;

constexpr const char* kHookPresent = "dxgi.Present";
constexpr const char* kHookPresent1 = "dxgi.Present1";
constexpr const char* kHookResize = "dxgi.ResizeBuffers";

PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;
ResizeBuffersFn g_resizeBuffers = nullptr;

// Threads currently executing inside one of our detours (including the call to the original).
// Unload waits for this to reach zero before the DLL can be freed.
std::atomic<int> g_inside{0};
std::atomic<bool> g_active{false};
// Present1 may be implemented on top of Present (or vice versa) by DXGI or other overlays;
// render the overlay only once per frame per thread.
thread_local bool t_inPresent = false;

struct InsideGuard {
  InsideGuard() { g_inside.fetch_add(1, std::memory_order_acq_rel); }
  ~InsideGuard() { g_inside.fetch_sub(1, std::memory_order_acq_rel); }
};

void RenderOverlay(IDXGISwapChain* sc, UINT flags) {
  if (!g_active.load(std::memory_order_acquire) || t_inPresent || (flags & DXGI_PRESENT_TEST)) return;
  t_inPresent = true;
  OnPresent(sc);
  t_inPresent = false;
}

HRESULT WINAPI HkPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags) {
  InsideGuard guard;
  const bool outer = !t_inPresent;
  RenderOverlay(sc, flags);
  if (outer) t_inPresent = true;
  const HRESULT hr = g_present(sc, syncInterval, flags);
  if (outer) t_inPresent = false;
  return hr;
}

HRESULT WINAPI HkPresent1(IDXGISwapChain1* sc, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
  InsideGuard guard;
  const bool outer = !t_inPresent;
  RenderOverlay(sc, flags);
  if (outer) t_inPresent = true;
  const HRESULT hr = g_present1(sc, syncInterval, flags, params);
  if (outer) t_inPresent = false;
  return hr;
}

HRESULT WINAPI HkResizeBuffers(IDXGISwapChain* sc, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
  InsideGuard guard;
  if (g_active.load(std::memory_order_acquire)) OnResizeBuffersBegin();
  const HRESULT hr = g_resizeBuffers(sc, count, width, height, format, flags);
  if (g_active.load(std::memory_order_acquire)) OnResizeBuffersEnd(sc);
  return hr;
}

LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

struct DummyVTables {
  void* present = nullptr;
  void* present1 = nullptr;
  void* resizeBuffers = nullptr;
};

// Creates a hidden window + device + swap chain just long enough to read the DXGI vtable.
bool ReadSwapChainVTable(DummyVTables& out) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = DummyWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"ChromaDummyD3D11";
  const ATOM atom = RegisterClassExW(&wc);
  if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    log::Error("render", "RegisterClassExW failed ({})", GetLastError());
    return false;
  }
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    log::Error("render", "CreateWindowExW for dummy swap chain failed ({})", GetLastError());
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return false;
  }

  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 1;
  sd.BufferDesc.Width = 16;
  sd.BufferDesc.Height = 16;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  IDXGISwapChain* sc = nullptr;
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  HRESULT hr = E_FAIL;
  for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
    hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (SUCCEEDED(hr)) break;
  }

  bool ok = false;
  if (SUCCEEDED(hr) && sc) {
    void** vtbl = *reinterpret_cast<void***>(sc);
    out.present = vtbl[kPresentSlot];
    out.resizeBuffers = vtbl[kResizeBuffersSlot];
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1))) && sc1) {
      out.present1 = (*reinterpret_cast<void***>(sc1))[kPresent1Slot];
      sc1->Release();
    }
    ok = out.present && out.resizeBuffers;
  } else {
    log::Error("render", "D3D11CreateDeviceAndSwapChain failed (0x{:08X})", static_cast<uint32_t>(hr));
  }

  if (sc) sc->Release();
  if (ctx) ctx->Release();
  if (dev) dev->Release();
  DestroyWindow(hwnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return ok;
}

}  // namespace

bool InstallD3D11Hooks() {
  DummyVTables vt;
  if (!ReadSwapChainVTable(vt)) return false;

  auto& hooks = mem::Hooks::Get();
  g_active.store(true, std::memory_order_release);
  if (!hooks.Install(kHookPresent, vt.present, reinterpret_cast<void*>(&HkPresent), reinterpret_cast<void**>(&g_present))) {
    g_active.store(false, std::memory_order_release);
    return false;
  }
  if (!hooks.Install(kHookResize, vt.resizeBuffers, reinterpret_cast<void*>(&HkResizeBuffers),
                     reinterpret_cast<void**>(&g_resizeBuffers))) {
    log::Warn("render", "ResizeBuffers hook failed; overlay will recreate targets lazily");
  }
  // Present1 is optional (flip-model games); same-address guard: some runtimes share the stub.
  if (vt.present1 && vt.present1 != vt.present &&
      !hooks.Install(kHookPresent1, vt.present1, reinterpret_cast<void*>(&HkPresent1), reinterpret_cast<void**>(&g_present1))) {
    log::Warn("render", "Present1 hook failed; continuing with Present only");
  }
  log::Info("render", "D3D11 hooks installed (Present={}, Present1={}, ResizeBuffers={})", vt.present, vt.present1,
            vt.resizeBuffers);
  return true;
}

void RemoveD3D11Hooks() {
  g_active.store(false, std::memory_order_release);
  auto& hooks = mem::Hooks::Get();
  for (const char* name : {kHookPresent, kHookPresent1, kHookResize}) {
    if (hooks.Exists(name)) hooks.SetEnabled(name, false);
  }
  // A thread may be parked inside the original Present (vsync) with our detour on its stack.
  for (int i = 0; i < 400 && g_inside.load(std::memory_order_acquire) > 0; ++i) Sleep(5);
  if (g_inside.load() > 0) log::Warn("render", "{} thread(s) still inside Present detour after 2 s", g_inside.load());
  for (const char* name : {kHookPresent, kHookPresent1, kHookResize}) {
    if (hooks.Exists(name)) hooks.Remove(name);
  }
}

int ThreadsInsideDetours() { return g_inside.load(std::memory_order_acquire); }

}  // namespace cg::render
