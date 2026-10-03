#pragma once
// Finds IDXGISwapChain::Present / ResizeBuffers (+ IDXGISwapChain1::Present1) by creating a
// throwaway device + swap chain on a hidden window, then detours the *functions* (not the vtable)
// with MinHook so we coexist with Steam overlay / RTSS / ReShade which detour the same code.
#include <windows.h>

namespace cg::render {

bool InstallD3D11Hooks();   // returns false if D3D11/DXGI is unavailable
void RemoveD3D11Hooks();    // disables detours, waits until no thread is inside them
int ThreadsInsideDetours();  // > 0 means freeing the DLL now would be unsafe

}  // namespace cg::render
