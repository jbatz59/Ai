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

#include <d3d11.h>
#include <dxgi.h>

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
