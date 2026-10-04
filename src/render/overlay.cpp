#include "render/overlay.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <stb_image.h>
#include <stb_image_write.h>

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

namespace cg::ui {
void AddressTableTick();   // ui/tools/address_table_window.cpp: frozen entries + per-entry hotkeys
}

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
  uint64_t unloadFrame = 0;   // test hook: CHROMA_TEST_UNLOAD_FRAME
  bool captureThisFrame = false;
  bool hideMenuThisFrame = false;
};

struct CaptureRequest {
  CaptureCallback cb;
  int maxWidth;
  bool hideMenu;
};
std::mutex g_captureMutex;
std::vector<CaptureRequest> g_captureQueue;   // requested, waiting for the next frame
std::vector<CaptureRequest> g_captureActive;  // being served by the current frame

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
  // One-time move to the Neon Glass redesign (1.1); a theme picked afterwards is kept.
  if (cfg.ReadInt("ui.theme_rev", 0) < 2) {
    cfg.WriteInt("ui.theme", static_cast<int>(theme::Preset::Neon));
    cfg.WriteInt("ui.theme_rev", 2);
  }
  const float scale = cfg.ReadFloat("ui.scale", AutoScale(desc.OutputWindow));
  theme::Apply(static_cast<theme::Preset>(cfg.ReadInt("ui.theme", static_cast<int>(theme::kDefaultPreset))), scale);
  fonts::Load(scale, g.fonts);

  QueryPerformanceFrequency(&g.freq);
  QueryPerformanceCounter(&g.last);
  RefreshHotkeys(true);

  g.screenshotPath = EnvW(L"CHROMA_TEST_SCREENSHOT");
  if (!g.screenshotPath.empty()) {
    const auto frame = util::ParseUInt(util::Narrow(EnvW(L"CHROMA_TEST_SCREENSHOT_FRAME")));
    g.screenshotFrame = frame.value_or(90);
  }
  g.unloadFrame = util::ParseUInt(util::Narrow(EnvW(L"CHROMA_TEST_UNLOAD_FRAME"))).value_or(0);

  g_initialized.store(true);
  ui::Init();
  script::Engine::Get().Init();

  if (EnvW(L"CHROMA_TEST_OPEN_MENU") == L"1") SetMenuOpen(true);
  if (const std::wstring page = EnvW(L"CHROMA_TEST_PAGE"); !page.empty()) ui::OpenMenuPage(util::Narrow(page), util::Narrow(EnvW(L"CHROMA_TEST_FILTER")));
  ui::notify::Push(ui::notify::Kind::Success, "Chroma loaded", "Press " + g.menuKey.ToString() + " to open the menu");
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

float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
  float v;
  if (exp == 0) v = std::ldexp(static_cast<float>(mant), -24);
  else if (exp == 31) v = mant ? 0.0f : 65504.0f;
  else v = std::ldexp(static_cast<float>(mant | 0x400), static_cast<int>(exp) - 25);
  return sign ? -v : v;
}

uint8_t ToByte(float v) {
  if (!(v > 0.0f)) return 0;
  if (v >= 1.0f) return 255;
  return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

// Reads the back buffer into tightly packed RGBA8. Handles typeless/sRGB 8-bit, 10:10:10:2 and
// FP16 (HDR add-ons such as RenoDX) back buffers, resolving MSAA first.
bool ReadBackBuffer(IDXGISwapChain* sc, std::vector<uint8_t>& out, int& width, int& height) {
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) return false;
  D3D11_TEXTURE2D_DESC d{};
  bb->GetDesc(&d);
  const DXGI_FORMAT fmt = ViewFormatFor(d.Format);
  enum class Layout { Rgba8, Bgra8, Rgb10A2, Rgba16F } layout;
  switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: layout = Layout::Rgba8; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM: layout = Layout::Bgra8; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: layout = Layout::Rgb10A2; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: layout = Layout::Rgba16F; break;
    default:
      log::Warn("render", "Capture: unsupported back buffer format {}", static_cast<int>(d.Format));
      bb->Release();
      return false;
  }

  ID3D11Texture2D* source = bb;
  ID3D11Texture2D* resolved = nullptr;
  if (d.SampleDesc.Count > 1) {
    D3D11_TEXTURE2D_DESC rd = d;
    rd.SampleDesc = {1, 0};
    rd.Format = fmt;
    rd.BindFlags = 0;
    rd.MiscFlags = 0;
    if (FAILED(g.device->CreateTexture2D(&rd, nullptr, &resolved))) {
      bb->Release();
      return false;
    }
    g.context->ResolveSubresource(resolved, 0, bb, 0, fmt);
    source = resolved;
  }

  D3D11_TEXTURE2D_DESC sd = d;
  sd.SampleDesc = {1, 0};
  sd.Usage = D3D11_USAGE_STAGING;
  sd.BindFlags = 0;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  sd.MiscFlags = 0;
  if (d.SampleDesc.Count > 1) sd.Format = fmt;
  ID3D11Texture2D* staging = nullptr;
  bool ok = false;
  if (SUCCEEDED(g.device->CreateTexture2D(&sd, nullptr, &staging)) && staging) {
    g.context->CopyResource(staging, source);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g.context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
      width = static_cast<int>(d.Width);
      height = static_cast<int>(d.Height);
      out.resize(static_cast<size_t>(width) * height * 4);
      for (int y = 0; y < height; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        uint8_t* dst = out.data() + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x, dst += 4) {
          switch (layout) {
            case Layout::Rgba8: dst[0] = row[x * 4]; dst[1] = row[x * 4 + 1]; dst[2] = row[x * 4 + 2]; break;
            case Layout::Bgra8: dst[0] = row[x * 4 + 2]; dst[1] = row[x * 4 + 1]; dst[2] = row[x * 4]; break;
            case Layout::Rgb10A2: {
              uint32_t px;
              memcpy(&px, row + x * 4, 4);
              dst[0] = static_cast<uint8_t>((px & 0x3FF) >> 2);
              dst[1] = static_cast<uint8_t>(((px >> 10) & 0x3FF) >> 2);
              dst[2] = static_cast<uint8_t>(((px >> 20) & 0x3FF) >> 2);
              break;
            }
            case Layout::Rgba16F: {
              uint16_t h[4];
              memcpy(h, row + x * 8, 8);
              // scRGB linear -> display-ish sRGB
              for (int c = 0; c < 3; ++c) dst[c] = ToByte(std::pow(std::max(0.0f, HalfToFloat(h[c])), 1.0f / 2.2f));
              break;
            }
          }
          dst[3] = 255;
        }
      }
      g.context->Unmap(staging, 0);
      ok = true;
    }
    staging->Release();
  }
  if (resolved) resolved->Release();
  bb->Release();
  return ok;
}

// Box-filter downscale so that width <= maxWidth (keeps aspect ratio).
void Downscale(const std::vector<uint8_t>& src, int w, int h, int maxWidth, std::vector<uint8_t>& dst, int& ow, int& oh) {
  if (maxWidth <= 0 || w <= maxWidth) {
    dst = src;
    ow = w;
    oh = h;
    return;
  }
  ow = maxWidth;
  oh = std::max(1, static_cast<int>(static_cast<int64_t>(h) * maxWidth / w));
  dst.assign(static_cast<size_t>(ow) * oh * 4, 0);
  for (int y = 0; y < oh; ++y) {
    const int sy0 = static_cast<int>(static_cast<int64_t>(y) * h / oh), sy1 = std::max(sy0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * h / oh));
    for (int x = 0; x < ow; ++x) {
      const int sx0 = static_cast<int>(static_cast<int64_t>(x) * w / ow), sx1 = std::max(sx0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * w / ow));
      uint32_t acc[4]{};
      uint32_t n = 0;
      for (int sy = sy0; sy < sy1 && sy < h; ++sy)
        for (int sx = sx0; sx < sx1 && sx < w; ++sx, ++n)
          for (int c = 0; c < 4; ++c) acc[c] += src[(static_cast<size_t>(sy) * w + sx) * 4 + c];
      for (int c = 0; c < 4; ++c) dst[(static_cast<size_t>(y) * ow + x) * 4 + c] = static_cast<uint8_t>(n ? acc[c] / n : 0);
    }
  }
}

void ServeCaptures(IDXGISwapChain* sc) {
  std::vector<CaptureRequest> requests;
  {
    std::lock_guard lock(g_captureMutex);
    requests.swap(g_captureActive);
  }
  if (requests.empty()) return;
  std::vector<uint8_t> full;
  int w = 0, h = 0;
  const bool ok = ReadBackBuffer(sc, full, w, h);
  for (auto& r : requests) {
    if (!r.cb) continue;
    if (!ok) {
      r.cb(false, {}, 0, 0);
      continue;
    }
    std::vector<uint8_t> px;
    int ow = 0, oh = 0;
    Downscale(full, w, h, r.maxWidth, px, ow, oh);
    r.cb(true, std::move(px), ow, oh);
  }
}

bool WriteFileW(const std::wstring& path, const void* data, size_t size) {
  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) return false;
  const bool ok = fwrite(data, 1, size, f) == size;
  return fclose(f) == 0 && ok;
}

bool SaveBmp(const std::wstring& path, const uint8_t* rgba, int w, int h) {
  std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < bgra.size(); i += 4) {
    bgra[i] = rgba[i + 2];
    bgra[i + 1] = rgba[i + 1];
    bgra[i + 2] = rgba[i];
    bgra[i + 3] = 255;
  }
  BITMAPFILEHEADER fh{};
  BITMAPINFOHEADER ih{};
  ih.biSize = sizeof(ih);
  ih.biWidth = w;
  ih.biHeight = -h;   // top-down
  ih.biPlanes = 1;
  ih.biBitCount = 32;
  ih.biCompression = BI_RGB;
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof(fh) + sizeof(ih);
  fh.bfSize = fh.bfOffBits + static_cast<DWORD>(bgra.size());
  std::vector<uint8_t> file(fh.bfSize);
  memcpy(file.data(), &fh, sizeof(fh));
  memcpy(file.data() + sizeof(fh), &ih, sizeof(ih));
  memcpy(file.data() + fh.bfOffBits, bgra.data(), bgra.size());
  return WriteFileW(path, file.data(), file.size());
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
  ui::AddressTableTick();
  script::Engine::Get().Tick(dt);
  log::FlushIfStale();   // the game may exit without unloading us; keep the file current

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

  {
    std::lock_guard lock(g_captureMutex);
    g.captureThisFrame = !g_captureQueue.empty();
    g.hideMenuThisFrame = false;
    for (const auto& r : g_captureQueue) g.hideMenuThisFrame |= r.hideMenu;
    for (auto& r : g_captureQueue) g_captureActive.push_back(std::move(r));
    g_captureQueue.clear();
  }

  theme::Tick(dt);
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

  if (!EnsureRenderTarget(sc)) {
    if (g.captureThisFrame) ServeCaptures(sc);   // still answer requests (from the game image)
    g.captureThisFrame = g.hideMenuThisFrame = false;
    return;
  }
  ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
  ID3D11DepthStencilView* oldDsv = nullptr;
  g.context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
  g.context->OMSetRenderTargets(1, &g.rtv, nullptr);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  g.context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
  for (auto*& r : oldRtv) ReleaseT(r);
  ReleaseT(oldDsv);

  if (g.captureThisFrame) {
    ServeCaptures(sc);
    g.captureThisFrame = g.hideMenuThisFrame = false;
  }
  if (g.screenshotFrame && g.frame + 1 == g.screenshotFrame) RequestScreenshot(g.screenshotPath, {}, false);
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

void RequestCapture(CaptureCallback cb, int maxWidth, bool hideMenu) {
  std::lock_guard lock(g_captureMutex);
  if (g_captureQueue.size() >= 16) {
    if (cb) tasks::PostRender([cb] { cb(false, {}, 0, 0); });
    return;
  }
  g_captureQueue.push_back({std::move(cb), maxWidth, hideMenu});
}

void RequestScreenshot(std::wstring path, std::function<void(bool ok, std::wstring path)> done, bool hideMenu) {
  RequestCapture(
      [path, done](bool ok, std::vector<uint8_t> rgba, int w, int h) {
        if (!ok) {
          if (done) done(false, path);
          return;
        }
        // Encoding a 4K PNG takes ~100 ms: keep it off the render thread.
        tasks::RunAsync([path, done, px = std::move(rgba), w, h] {
          const bool png = path.size() >= 4 && util::IEquals(util::Narrow(path.substr(path.size() - 4)), ".png");
          const bool written = png ? SavePng(path, px.data(), w, h) : SaveBmp(path, px.data(), w, h);
          if (written) log::Info("render", "Screenshot written ({}x{}): {}", w, h, util::Narrow(path));
          else log::Warn("render", "Screenshot could not be written: {}", util::Narrow(path));
          if (done) tasks::PostRender([done, path, written] { done(written, path); });
        });
      },
      0, hideMenu);
}

bool CaptureHidesMenu() { return g.hideMenuThisFrame; }

bool SavePng(const std::wstring& path, const uint8_t* rgba, int width, int height) {
  if (!rgba || width <= 0 || height <= 0) return false;
  std::vector<uint8_t> encoded;
  const int ok = stbi_write_png_to_func(
      [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<uint8_t>*>(ctx);
        v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
      },
      &encoded, width, height, 4, rgba, width * 4);
  return ok && WriteFileW(path, encoded.data(), encoded.size());
}

Texture CreateTexture(const uint8_t* rgba, int width, int height) {
  Texture t;
  if (!g.device || !rgba || width <= 0 || height <= 0 || width > 16384 || height > 16384) return t;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = static_cast<UINT>(width);
  d.Height = static_cast<UINT>(height);
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_IMMUTABLE;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init{rgba, static_cast<UINT>(width * 4), 0};
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(g.device->CreateTexture2D(&d, &init, &tex)) || !tex) return t;
  ID3D11ShaderResourceView* srv = nullptr;
  const HRESULT hr = g.device->CreateShaderResourceView(tex, nullptr, &srv);
  tex->Release();
  if (FAILED(hr) || !srv) return t;
  t.id = reinterpret_cast<ImTextureID>(srv);
  t.width = width;
  t.height = height;
  return t;
}

Texture LoadTextureFile(const std::wstring& path) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return {};
  std::vector<uint8_t> bytes;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && bytes.size() < (64u << 20)) bytes.insert(bytes.end(), buf, buf + n);
  fclose(f);
  int w = 0, h = 0, comp = 0;
  stbi_uc* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
  if (!px) return {};
  Texture t = CreateTexture(px, w, h);
  stbi_image_free(px);
  return t;
}

void DestroyTexture(Texture& t) {
  if (t.id) reinterpret_cast<ID3D11ShaderResourceView*>(t.id)->Release();
  t = {};
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
