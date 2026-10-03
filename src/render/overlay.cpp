#include "render/overlay.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "core/bootstrap.h"
#include "core/config.h"
#include "core/hotkeys.h"
#include "core/log.h"
#include "core/mp_guard.h"
#include "core/paths.h"
#include "core/tasks.h"
#include "core/util.h"
#include "features/feature.h"
#include "game/state.h"
#include "mem/hwbp.h"
#include "render/fonts.h"
#include "render/input.h"
#include "render/theme.h"
#include "script/engine.h"
#include "ui/notify.h"
#include "ui/ui.h"

namespace cg::render {
namespace {

struct State {
  IDXGISwapChain* swapChain = nullptr;   // identity only, not AddRef'd
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  HWND hwnd = nullptr;
  IDXGISwapChain* rejected = nullptr;    // last non-D3D11 swap chain (avoid log spam)

  std::string iniPath;
  Fonts fonts;
  float width = 0, height = 0;           // ImGui display size (client area)
  LARGE_INTEGER freq{}, last{};
  float dt = 1.0f / 60.0f, fps = 60.0f;
  uint64_t frame = 0;
  double accSecond = 0, accGuard = 0;

  Hotkey menuKey, panicKey, unloadKey;
  uint64_t lastHotkeyRefresh = 0;
  bool softwareCursor = false;
  bool backendsBound = false;

  std::wstring screenshotPath;
  uint64_t screenshotFrame = 0;
  uint64_t unloadFrame = 0;   // test hook: CONSIGLIERE_TEST_UNLOAD_FRAME
};

State g;
std::mutex g_frameMutex;   // whole frame, resize handling and teardown are mutually exclusive
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_menuOpen{false};
std::atomic<bool> g_teardownRequested{false};
HANDLE g_teardownDone = nullptr;

template <class T> void ReleaseT(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

std::wstring EnvW(const wchar_t* name) {
  wchar_t buf[1024];
  const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
  return (n > 0 && n < std::size(buf)) ? std::wstring(buf, n) : std::wstring();
}

void RefreshHotkeys(bool force) {
  const uint64_t now = util::NowMs();
  if (!force && now - g.lastHotkeyRefresh < 500) return;
  g.lastHotkeyRefresh = now;
  auto& cfg = Config::Get();
  g.menuKey = Hotkey::Parse(cfg.ReadString("hotkeys.menu", "Insert"));
  g.panicKey = Hotkey::Parse(cfg.ReadString("hotkeys.panic", "Ctrl+Shift+P"));
  g.unloadKey = Hotkey::Parse(cfg.ReadString("hotkeys.unload", "Ctrl+Shift+End"));
  if (!g.menuKey.Valid()) g.menuKey = Hotkey{VK_INSERT};
}

bool OsCursorVisible() {
  CURSORINFO ci{sizeof(ci)};
  return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
}

float AutoScale(HWND hwnd) {
  RECT rc{};
  if (!GetClientRect(hwnd, &rc)) return 1.0f;
  const int h = rc.bottom - rc.top;
  if (h >= 2000) return 1.5f;
  if (h >= 1400) return 1.25f;
  return 1.0f;
}

// Typeless back buffers (Mafia: DE uses B8G8R8A8_TYPELESS) need an explicit view format.
DXGI_FORMAT ViewFormatFor(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    default: return f;
  }
}

bool EnsureRenderTarget(IDXGISwapChain* sc) {
  if (g.rtv) return true;
  ID3D11Texture2D* backBuffer = nullptr;
  if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) || !backBuffer) return false;
  D3D11_TEXTURE2D_DESC td{};
  backBuffer->GetDesc(&td);
  D3D11_RENDER_TARGET_VIEW_DESC rd{};
  rd.Format = ViewFormatFor(td.Format);
  rd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
  const HRESULT hr = g.device->CreateRenderTargetView(backBuffer, &rd, &g.rtv);
  backBuffer->Release();
  if (FAILED(hr)) {
    log::Warn("render", "CreateRenderTargetView failed (0x{:08X})", static_cast<uint32_t>(hr));
    g.rtv = nullptr;
    return false;
  }
  return true;
}

void ShutdownBackends() {
  if (!g.backendsBound) return;
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  g.backendsBound = false;
}

// (Re)binds ImGui backends to a device/window. Keeps the ImGui context, fonts and settings.
bool BindBackends(IDXGISwapChain* sc, ID3D11Device* dev, HWND hwnd) {
  ID3D11DeviceContext* ctx = nullptr;
  dev->GetImmediateContext(&ctx);
  if (!ctx) return false;
  if (!ImGui_ImplWin32_Init(hwnd)) {
    ctx->Release();
    return false;
  }
  if (!ImGui_ImplDX11_Init(dev, ctx)) {
    ImGui_ImplWin32_Shutdown();
    ctx->Release();
    return false;
  }
  ReleaseT(g.context);
  ReleaseT(g.device);
  g.device = dev;
  g.device->AddRef();
  g.context = ctx;   // owns the reference from GetImmediateContext
  g.swapChain = sc;
  g.backendsBound = true;
  if (g.hwnd != hwnd) {
    if (g.hwnd) input::Uninstall();
    g.hwnd = hwnd;
    input::Install(hwnd);
  }
  return true;
}

bool InitFor(IDXGISwapChain* sc) {
  if (sc == g.rejected) return false;
  ID3D11Device* dev = nullptr;
  if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) {
    g.rejected = sc;
    log::Warn("render", "Ignoring a swap chain that is not backed by a D3D11 device");
    return false;
  }
  DXGI_SWAP_CHAIN_DESC desc{};
  if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow || !IsWindow(desc.OutputWindow)) {
    dev->Release();
    return false;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  g.iniPath = util::Narrow(paths::Data(L"imgui.ini").wstring());
  io.IniFilename = g.iniPath.c_str();
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

  if (!BindBackends(sc, dev, desc.OutputWindow)) {
    dev->Release();
    ImGui::DestroyContext();
    log::Error("render", "ImGui backend initialisation failed");
    return false;
  }
  dev->Release();   // BindBackends took its own reference

  auto& cfg = Config::Get();
  const float scale = cfg.ReadFloat("ui.scale", AutoScale(desc.OutputWindow));
  theme::Apply(static_cast<theme::Preset>(cfg.ReadInt("ui.theme", 0)), scale);
  fonts::Load(scale, g.fonts);

  QueryPerformanceFrequency(&g.freq);
  QueryPerformanceCounter(&g.last);
  RefreshHotkeys(true);

  g.screenshotPath = EnvW(L"CONSIGLIERE_TEST_SCREENSHOT");
  if (!g.screenshotPath.empty()) {
    const auto frame = util::ParseUInt(util::Narrow(EnvW(L"CONSIGLIERE_TEST_SCREENSHOT_FRAME")));
    g.screenshotFrame = frame.value_or(90);
  }
  g.unloadFrame = util::ParseUInt(util::Narrow(EnvW(L"CONSIGLIERE_TEST_UNLOAD_FRAME"))).value_or(0);

  g_initialized.store(true);
  ui::Init();
  script::Engine::Get().Init();

  if (EnvW(L"CONSIGLIERE_TEST_OPEN_MENU") == L"1") SetMenuOpen(true);
  ui::notify::Push(ui::notify::Kind::Success, "Consigliere loaded", "Press " + g.menuKey.ToString() + " to open the menu");
  log::Info("render", "Overlay initialised on HWND {} (scale {:.2f})", static_cast<void*>(desc.OutputWindow), scale);
  return true;
}

void DoTeardown() {
  if (g_initialized.load()) {
    features::Registry::Get().DisableAll();
    script::Engine::Get().Shutdown();
    ui::Shutdown();
    input::Uninstall();
    ShutdownBackends();
    ImGui::DestroyContext();
    g_initialized.store(false);
  }
  ReleaseT(g.rtv);
  ReleaseT(g.context);
  ReleaseT(g.device);
  g.swapChain = nullptr;
  g.hwnd = nullptr;
}

void WriteScreenshot(IDXGISwapChain* sc) {
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) return;
  D3D11_TEXTURE2D_DESC d{};
  bb->GetDesc(&d);
  const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                    d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
  const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                    d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
  if ((!bgra && !rgba) || d.SampleDesc.Count != 1) {
    log::Warn("render", "Screenshot: unsupported back buffer format {}", static_cast<int>(d.Format));
    bb->Release();
    return;
  }
  D3D11_TEXTURE2D_DESC sd = d;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.BindFlags = 0;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  sd.MiscFlags = 0;
  ID3D11Texture2D* staging = nullptr;
  if (SUCCEEDED(g.device->CreateTexture2D(&sd, nullptr, &staging)) && staging) {
    g.context->CopyResource(staging, bb);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g.context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
      std::vector<uint8_t> pixels(static_cast<size_t>(d.Width) * d.Height * 4);
      for (UINT y = 0; y < d.Height; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        uint8_t* dst = pixels.data() + static_cast<size_t>(y) * d.Width * 4;
        for (UINT x = 0; x < d.Width; ++x) {
          dst[x * 4 + 0] = src[x * 4 + (rgba ? 2 : 0)];
          dst[x * 4 + 1] = src[x * 4 + 1];
          dst[x * 4 + 2] = src[x * 4 + (rgba ? 0 : 2)];
          dst[x * 4 + 3] = 255;
        }
      }
      g.context->Unmap(staging, 0);
      BITMAPFILEHEADER fh{};
      BITMAPINFOHEADER ih{};
      ih.biSize = sizeof(ih);
      ih.biWidth = static_cast<LONG>(d.Width);
      ih.biHeight = -static_cast<LONG>(d.Height);   // top-down
      ih.biPlanes = 1;
      ih.biBitCount = 32;
      ih.biCompression = BI_RGB;
      fh.bfType = 0x4D42;
      fh.bfOffBits = sizeof(fh) + sizeof(ih);
      fh.bfSize = fh.bfOffBits + static_cast<DWORD>(pixels.size());
      if (FILE* f = _wfopen(g.screenshotPath.c_str(), L"wb")) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        fwrite(pixels.data(), 1, pixels.size(), f);
        fclose(f);
        log::Info("render", "Screenshot written ({}x{})", d.Width, d.Height);
      }
    }
    staging->Release();
  }
  bb->Release();
}

void HandleGlobalHotkeys() {
  RefreshHotkeys(false);
  if (hotkeys::Pressed(g.menuKey)) ToggleMenu();
  if (hotkeys::Pressed(g.panicKey)) {
    features::Registry::Get().DisableAll();
    ui::notify::Push(ui::notify::Kind::Warning, "Panic", "All features disabled");
  }
  if (hotkeys::Pressed(g.unloadKey)) RequestUnload();
}

void Frame(IDXGISwapChain* sc) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  float dt = static_cast<float>(static_cast<double>(now.QuadPart - g.last.QuadPart) / static_cast<double>(g.freq.QuadPart));
  g.last = now;
  if (dt < 0) dt = 0;
  if (dt > 0.25f) dt = 0.25f;
  g.dt = dt;
  if (dt > 0) g.fps = g.fps * 0.95f + (1.0f / dt) * 0.05f;
  ++g.frame;

  hotkeys::BeginFrame(g.hwnd);
  HandleGlobalHotkeys();
  input::SetCaptured(g_menuOpen.load());

  // Features and scripts tick outside the ImGui frame; they draw in DrawSettings/DrawOverlay.
  tasks::DrainRender();
  if (!tasks::GameThreadHookActive()) tasks::DrainGame();
  game::state::Tick(dt);
  features::Registry::Get().Tick(dt);
  script::Engine::Get().Tick(dt);

  g.accSecond += dt;
  g.accGuard += dt;
  if (g.accSecond >= 1.0) {
    g.accSecond = 0;
    mem::hwbp::Tick();
    Config::Get().SaveIfDirty();
  }
  if (g.accGuard >= 5.0) {
    g.accGuard = 0;
    const bool wasBlocked = mp_guard::Blocked();
    mp_guard::Refresh();
    if (!wasBlocked && mp_guard::Blocked()) {
      features::Registry::Get().DisableAll();
      ui::notify::Push(ui::notify::Kind::Error, "Multiplayer detected", mp_guard::Reason(), 8.0f);
    }
  }

  ImGui_ImplDX11_NewFrame();
  input::PumpToImGui();
  ImGui_ImplWin32_NewFrame();
  ImGuiIO& io = ImGui::GetIO();
  // The game may render at a resolution that differs from its client area; ImGui works in client
  // coordinates (mouse) and the framebuffer scale maps them onto the back buffer.
  DXGI_SWAP_CHAIN_DESC desc{};
  if (SUCCEEDED(sc->GetDesc(&desc)) && io.DisplaySize.x > 0 && io.DisplaySize.y > 0 && desc.BufferDesc.Width && desc.BufferDesc.Height) {
    io.DisplayFramebufferScale = ImVec2(desc.BufferDesc.Width / io.DisplaySize.x, desc.BufferDesc.Height / io.DisplaySize.y);
  }
  g.width = io.DisplaySize.x;
  g.height = io.DisplaySize.y;
  io.MouseDrawCursor = g_menuOpen.load() && g.softwareCursor;
  ImGui::NewFrame();
  input::SetImGuiWants(io.WantCaptureMouse, io.WantCaptureKeyboard || io.WantTextInput);
  ui::DrawFrame();
  ImGui::Render();

  if (!EnsureRenderTarget(sc)) return;
  ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
  ID3D11DepthStencilView* oldDsv = nullptr;
  g.context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
  g.context->OMSetRenderTargets(1, &g.rtv, nullptr);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  g.context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
  for (auto*& r : oldRtv) ReleaseT(r);
  ReleaseT(oldDsv);

  if (g.screenshotFrame && g.frame == g.screenshotFrame) WriteScreenshot(sc);
  if (g.unloadFrame && g.frame == g.unloadFrame) RequestUnload();
}

}  // namespace

void OnPresent(IDXGISwapChain* sc) {
  std::lock_guard lock(g_frameMutex);
  if (g_teardownRequested.load()) {
    DoTeardown();
    if (g_teardownDone) SetEvent(g_teardownDone);
    return;
  }
  if (IsUnloading()) return;

  if (!g_initialized.load()) {
    if (!InitFor(sc)) return;
  } else if (sc != g.swapChain) {
    // The game recreated its swap chain (mode switch). Rebind if the device or window changed.
    ID3D11Device* dev = nullptr;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return;
    if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow) {
      dev->Release();
      return;
    }
    ReleaseT(g.rtv);
    if (dev != g.device || desc.OutputWindow != g.hwnd) {
      ShutdownBackends();
      if (!BindBackends(sc, dev, desc.OutputWindow)) {
        dev->Release();
        log::Error("render", "Failed to rebind overlay to the new swap chain; overlay disabled");
        DoTeardown();
        g.rejected = sc;
        return;
      }
      log::Info("render", "Overlay rebound to a new swap chain");
    }
    dev->Release();
    g.swapChain = sc;
  }
  Frame(sc);
}

void OnResizeBuffersBegin() {
  std::lock_guard lock(g_frameMutex);
  ReleaseT(g.rtv);   // the swap chain cannot resize while we hold a back-buffer reference
}

void OnResizeBuffersEnd(IDXGISwapChain*) {}

void Shutdown() {
  if (!g_initialized.load()) {
    std::lock_guard lock(g_frameMutex);
    DoTeardown();
    return;
  }
  g_teardownDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_teardownRequested.store(true);
  // Normally the next Present tears down on the render thread. If the game stopped presenting
  // (minimised, loading hitch) do it here; the frame mutex keeps it exclusive with Present.
  if (!g_teardownDone || WaitForSingleObject(g_teardownDone, 1500) != WAIT_OBJECT_0) {
    std::lock_guard lock(g_frameMutex);
    DoTeardown();
  }
  if (g_teardownDone) {
    CloseHandle(g_teardownDone);
    g_teardownDone = nullptr;
  }
}

bool Initialized() { return g_initialized.load(); }
bool MenuOpen() { return g_menuOpen.load(); }

void SetMenuOpen(bool open) {
  if (g_menuOpen.exchange(open) == open) return;
  // Decide once per opening whether to draw ImGui's cursor: games usually hide the OS cursor.
  if (open) g.softwareCursor = Config::Get().ReadBool("ui.software_cursor", !OsCursorVisible());
  input::SetCaptured(open);
}

void ToggleMenu() { SetMenuOpen(!g_menuOpen.load()); }

HWND GameWindow() { return g.hwnd; }
ID3D11Device* Device() { return g.device; }
ID3D11DeviceContext* Context() { return g.context; }
float ScreenWidth() { return g.width; }
float ScreenHeight() { return g.height; }
uint64_t FrameCount() { return g.frame; }
float DeltaTime() { return g.dt; }
float Fps() { return g.fps; }
const Fonts& GetFonts() { return g.fonts; }

}  // namespace cg::render
