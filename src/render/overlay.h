#pragma once
// Owns the Dear ImGui context and the per-frame pipeline. Called from the Present detour:
//
//   OnPresent(sc):
//     lazy init (device, context, HWND, ImGui, input hooks, theme, fonts)
//     hotkeys::BeginFrame -> menu toggle / panic / unload hotkeys
//     tasks::DrainRender, (tasks::DrainGame if no game-thread hook)
//     features::Registry::Tick(dt), script::Engine::Tick(dt), mem::hwbp::Tick (1 Hz)
//     ImGui NewFrame -> ui::DrawFrame() -> ImGui Render -> draw to the back buffer
//     Config::SaveIfDirty (1 Hz), mp_guard::Refresh (0.2 Hz)
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>

struct ImFont;

namespace cg::render {

void OnPresent(IDXGISwapChain* swapChain);
void OnResizeBuffersBegin();                 // release our RTV before the game resizes
void OnResizeBuffersEnd(IDXGISwapChain* sc); // recreate lazily on next Present
void Shutdown();                             // render thread state teardown (ImGui, RTV, input)

bool Initialized();
bool MenuOpen();
void SetMenuOpen(bool open);
void ToggleMenu();

HWND GameWindow();
ID3D11Device* Device();
ID3D11DeviceContext* Context();
float ScreenWidth();
float ScreenHeight();
uint64_t FrameCount();
float DeltaTime();    // seconds, clamped to [0, 0.25]
float Fps();          // smoothed

// ---- Frame capture & textures (render thread unless noted) ------------------------------------
// Captures the final back buffer (game + overlay) at the end of the next frame. When hideMenu is
// set, the menu and tool windows are skipped on that frame (HUD, effects and toasts still draw) —
// ui::DrawFrame checks CaptureHidesMenu(). The callback runs on the render thread with tightly
// packed RGBA8 pixels, box-downscaled so width <= maxWidth when maxWidth > 0. Thread-safe to call.
using CaptureCallback = std::function<void(bool ok, std::vector<uint8_t> rgba, int width, int height)>;
void RequestCapture(CaptureCallback cb, int maxWidth = 0, bool hideMenu = true);
// Convenience: capture and write a PNG (".png") or BMP (anything else) on a worker thread.
// `done` runs on the render thread. Thread-safe to call.
void RequestScreenshot(std::wstring path, std::function<void(bool ok, std::wstring path)> done = {}, bool hideMenu = true);
bool CaptureHidesMenu();

struct Texture {
  ImTextureID id = 0;   // ID3D11ShaderResourceView*; usable with ImGui::Image / AddImage
  int width = 0, height = 0;
  bool Valid() const { return id != 0; }
};
Texture CreateTexture(const uint8_t* rgba, int width, int height);
Texture LoadTextureFile(const std::wstring& path);   // PNG/JPG/BMP via stb_image
void DestroyTexture(Texture& t);
bool SavePng(const std::wstring& path, const uint8_t* rgba, int width, int height);   // any thread

// Fonts created by fonts.cpp (nullptr until init). Body is the default font.
struct Fonts {
  ImFont* body = nullptr;
  ImFont* bold = nullptr;
  ImFont* title = nullptr;
  ImFont* mono = nullptr;
  ImFont* icons = nullptr;   // merged into body when Segoe MDL2/Fluent icons exist; else nullptr
  bool hasIcons = false;
};
const Fonts& GetFonts();

}  // namespace cg::render
