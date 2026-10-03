#pragma once
// Polled keyboard state with per-frame edge detection. Polling GetAsyncKeyState works no matter
// which input API the game uses (raw input, DirectInput, messages). Only active while the game
// window is the foreground window.
#include <cstdint>
#include <string>
#include <string_view>

namespace cg {

struct Hotkey {
  uint16_t vk = 0;   // Win32 virtual-key code; 0 = unbound
  bool ctrl = false, shift = false, alt = false;

  bool Valid() const { return vk != 0; }
  std::string ToString() const;                   // "Ctrl+Shift+F5", "Insert", "Unbound"
  static Hotkey Parse(std::string_view text);     // inverse of ToString(); invalid -> unbound
  bool operator==(const Hotkey&) const = default;
};

namespace hotkeys {

void BeginFrame(void* gameHwnd);   // called once per frame by the overlay before anything reads keys
bool IsDown(uint16_t vk);
bool Pressed(uint16_t vk);         // went down this frame
bool Released(uint16_t vk);
bool Pressed(const Hotkey& hk);    // key edge + exact modifier match; false while capturing or typing in ImGui

// Rebinding UX: BeginCapture(), then each frame PollCapture() returns true once a key was chosen.
// Escape cancels (out = previous binding), Backspace/Delete unbinds (out.vk = 0).
void BeginCapture();
bool Capturing();
bool PollCapture(Hotkey& out);
void CancelCapture();

const char* VkName(uint16_t vk);   // "F5", "Insert", "Mouse4", "Num 7", ...

}  // namespace hotkeys
}  // namespace cg
