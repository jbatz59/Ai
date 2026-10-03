#pragma once
// Input capture while the menu is open.
//  - Subclasses the game WndProc. Input messages are queued and replayed into ImGui on the render
//    thread (PumpToImGui) because ImGui's input queue is not thread-safe.
//  - While captured the game stops seeing *presses and movement* but still sees *releases*, so no
//    key stays stuck when the menu closes. Covered paths: window messages, GetRawInputData,
//    GetRawInputBuffer, GetAsyncKeyState/GetKeyState/GetKeyboardState, GetCursorPos/SetCursorPos,
//    ClipCursor, XInputGetState and DirectInput8 GetDeviceState/GetDeviceData.
//  - Calls made from inside Chroma (ImGui backend, hotkeys) bypass the filters (detected by
//    return address).
//  - If the game only delivers raw input (RIDEV_NOLEGACY), ImGui events are synthesised from it.
#include <windows.h>

namespace cg::render::input {

bool Install(HWND gameWindow);
void Uninstall();
bool SafeToFree();   // false if the WndProc could not be restored or a thread is still inside a detour

void SetCaptured(bool captured);
bool Captured();

// Overlay publishes ImGui's wishes each frame (used when "input.block_all" is false).
void SetImGuiWants(bool mouse, bool keyboard);

// Render thread, right before ImGui_ImplWin32_NewFrame().
void PumpToImGui();

}  // namespace cg::render::input
