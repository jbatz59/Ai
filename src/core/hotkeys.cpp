#include "core/hotkeys.h"

#include <array>
#include <atomic>
#include <bitset>
#include <cstdio>
#include <mutex>

#include <windows.h>

#include <imgui.h>

#include "core/util.h"

namespace cg {
namespace {

constexpr uint16_t kMaxVk = 255;

// bit0 = down this frame, bit1 = down previous frame. One atomic per key keeps each key's pair
// consistent for readers on other threads without a lock.
std::array<std::atomic<uint8_t>, 256> g_keys{};
std::atomic<bool> g_wasActive{false};

struct Capture {
  std::mutex mutex;
  bool active = false;
  std::bitset<256> ignore;   // keys held when capture began; ignored until released
};

Capture& Cap() {
  static Capture* c = new Capture();
  return *c;
}

std::atomic<bool> g_capturing{false};

bool IsModifierVk(uint16_t vk) {
  switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
      return true;
    default:
      return false;
  }
}
bool IsCtrlVk(uint16_t vk) { return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL; }
bool IsShiftVk(uint16_t vk) { return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT; }
bool IsAltVk(uint16_t vk) { return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU; }

// Keys a capture can produce. Left mouse is excluded because it is how the user operates the menu
// (clicking anywhere would rebind); Windows keys open the Start menu and steal focus from the game.
bool IsCapturable(uint16_t vk) {
  if (vk == 0 || vk > 254) return false;
  if (IsModifierVk(vk)) return false;
  if (vk == VK_LBUTTON || vk == VK_LWIN || vk == VK_RWIN) return false;
  return true;
}

bool ForegroundIsThisProcess(HWND gameHwnd) {
  const HWND fg = GetForegroundWindow();
  if (!fg) return false;
  if (gameHwnd && fg == gameHwnd) return true;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
}

struct NameTable {
  std::array<const char*, 256> names{};
  char unknown[256][8]{};
  NameTable() {
    static constexpr const char* kFunctionKeys[24] = {"F1",  "F2",  "F3",  "F4",  "F5",  "F6",  "F7",  "F8",
                                                      "F9",  "F10", "F11", "F12", "F13", "F14", "F15", "F16",
                                                      "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24"};
    static constexpr const char* kLetters[26] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                                 "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
    static constexpr const char* kDigits[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    static constexpr const char* kNumpad[10] = {"Num 0", "Num 1", "Num 2", "Num 3", "Num 4",
                                                "Num 5", "Num 6", "Num 7", "Num 8", "Num 9"};
    for (int i = 0; i < 24; ++i) names[VK_F1 + i] = kFunctionKeys[i];
    for (int i = 0; i < 26; ++i) names['A' + i] = kLetters[i];
    for (int i = 0; i < 10; ++i) names['0' + i] = kDigits[i];
    for (int i = 0; i < 10; ++i) names[VK_NUMPAD0 + i] = kNumpad[i];

    names[VK_LBUTTON] = "Mouse1";
    names[VK_RBUTTON] = "Mouse2";
    names[VK_CANCEL] = "Break";
    names[VK_MBUTTON] = "Mouse3";
    names[VK_XBUTTON1] = "Mouse4";
    names[VK_XBUTTON2] = "Mouse5";
    names[VK_BACK] = "Backspace";
    names[VK_TAB] = "Tab";
    names[VK_CLEAR] = "Clear";
    names[VK_RETURN] = "Enter";
    names[VK_SHIFT] = "Shift";
    names[VK_CONTROL] = "Ctrl";
    names[VK_MENU] = "Alt";
    names[VK_PAUSE] = "Pause";
    names[VK_CAPITAL] = "CapsLock";
    names[VK_ESCAPE] = "Escape";
    names[VK_SPACE] = "Space";
    names[VK_PRIOR] = "PageUp";
    names[VK_NEXT] = "PageDown";
    names[VK_END] = "End";
    names[VK_HOME] = "Home";
    names[VK_LEFT] = "Left";
    names[VK_UP] = "Up";
    names[VK_RIGHT] = "Right";
    names[VK_DOWN] = "Down";
    names[VK_SELECT] = "Select";
    names[VK_PRINT] = "Print";
    names[VK_EXECUTE] = "Execute";
    names[VK_SNAPSHOT] = "PrintScreen";
    names[VK_INSERT] = "Insert";
    names[VK_DELETE] = "Delete";
    names[VK_HELP] = "Help";
    names[VK_LWIN] = "LWin";
    names[VK_RWIN] = "RWin";
    names[VK_APPS] = "Menu";
    names[VK_SLEEP] = "Sleep";
    names[VK_MULTIPLY] = "Num *";
    names[VK_ADD] = "Num +";
    names[VK_SEPARATOR] = "Num Sep";
    names[VK_SUBTRACT] = "Num -";
    names[VK_DECIMAL] = "Num .";
    names[VK_DIVIDE] = "Num /";
    names[VK_NUMLOCK] = "NumLock";
    names[VK_SCROLL] = "ScrollLock";
    names[VK_LSHIFT] = "LShift";
    names[VK_RSHIFT] = "RShift";
    names[VK_LCONTROL] = "LCtrl";
    names[VK_RCONTROL] = "RCtrl";
    names[VK_LMENU] = "LAlt";
    names[VK_RMENU] = "RAlt";
    names[VK_BROWSER_BACK] = "BrowserBack";
    names[VK_BROWSER_FORWARD] = "BrowserForward";
    names[VK_BROWSER_REFRESH] = "BrowserRefresh";
    names[VK_BROWSER_STOP] = "BrowserStop";
    names[VK_BROWSER_SEARCH] = "BrowserSearch";
    names[VK_BROWSER_FAVORITES] = "BrowserFavorites";
    names[VK_BROWSER_HOME] = "BrowserHome";
    names[VK_VOLUME_MUTE] = "VolumeMute";
    names[VK_VOLUME_DOWN] = "VolumeDown";
    names[VK_VOLUME_UP] = "VolumeUp";
    names[VK_MEDIA_NEXT_TRACK] = "MediaNext";
    names[VK_MEDIA_PREV_TRACK] = "MediaPrev";
    names[VK_MEDIA_STOP] = "MediaStop";
    names[VK_MEDIA_PLAY_PAUSE] = "MediaPlay";
    names[VK_LAUNCH_MAIL] = "Mail";
    names[VK_LAUNCH_MEDIA_SELECT] = "MediaSelect";
    names[VK_LAUNCH_APP1] = "App1";
    names[VK_LAUNCH_APP2] = "App2";
    // OEM keys use their US-layout legends so saved bindings mean the same thing on every machine.
    names[VK_OEM_1] = ";";
    names[VK_OEM_PLUS] = "=";
    names[VK_OEM_COMMA] = ",";
    names[VK_OEM_MINUS] = "-";
    names[VK_OEM_PERIOD] = ".";
    names[VK_OEM_2] = "/";
    names[VK_OEM_3] = "`";
    names[VK_OEM_4] = "[";
    names[VK_OEM_5] = "\\";
    names[VK_OEM_6] = "]";
    names[VK_OEM_7] = "'";
    names[VK_OEM_8] = "OEM8";
    names[VK_OEM_102] = "OEM102";

    for (int vk = 0; vk < 256; ++vk) {
      if (names[vk]) continue;
      std::snprintf(unknown[vk], sizeof(unknown[vk]), "0x%02X", vk);
      names[vk] = unknown[vk];
    }
  }
};

const NameTable& Names() {
  static const NameTable* t = new NameTable();
  return *t;
}

bool ImGuiWantsText() {
  ImGuiContext* ctx = ImGui::GetCurrentContext();
  return ctx && ImGui::GetIO().WantTextInput;
}

bool ConsumePrefix(std::string_view& s, std::string_view prefix) {
  if (s.size() <= prefix.size() || !util::IEquals(s.substr(0, prefix.size()), prefix)) return false;
  s.remove_prefix(prefix.size());
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  return true;
}

}  // namespace

std::string Hotkey::ToString() const {
  if (!Valid()) return "Unbound";
  std::string out;
  if (ctrl) out += "Ctrl+";
  if (shift) out += "Shift+";
  if (alt) out += "Alt+";
  out += hotkeys::VkName(vk);
  return out;
}

Hotkey Hotkey::Parse(std::string_view text) {
  try {
    std::string trimmed = util::Trim(text);
    std::string_view s = trimmed;
    if (s.empty() || util::IEquals(s, "unbound") || util::IEquals(s, "none")) return {};
    Hotkey hk;
    for (bool progress = true; progress;) {
      progress = false;
      if (ConsumePrefix(s, "ctrl+") || ConsumePrefix(s, "control+")) hk.ctrl = progress = true;
      else if (ConsumePrefix(s, "shift+")) hk.shift = progress = true;
      else if (ConsumePrefix(s, "alt+")) hk.alt = progress = true;
    }
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    if (s.empty()) return {};
    const NameTable& t = Names();
    for (int vk = 1; vk <= kMaxVk; ++vk) {
      if (util::IEquals(s, t.names[vk])) {
        hk.vk = static_cast<uint16_t>(vk);
        return hk;
      }
    }
    if (const auto v = util::ParseUInt(s); v && *v >= 1 && *v <= kMaxVk && util::StartsWith(util::ToLower(s), "0x")) {
      hk.vk = static_cast<uint16_t>(*v);
      return hk;
    }
    return {};
  } catch (...) {
    return {};
  }
}

namespace hotkeys {

void BeginFrame(void* gameHwnd) {
  const bool active = ForegroundIsThisProcess(static_cast<HWND>(gameHwnd));
  const bool wasActive = g_wasActive.exchange(active, std::memory_order_relaxed);
  const bool justActivated = active && !wasActive;
  for (int vk = 1; vk <= 254; ++vk) {
    const bool down = active && (GetAsyncKeyState(vk) & 0x8000) != 0;
    const uint8_t old = g_keys[vk].load(std::memory_order_relaxed);
    // On the frame focus returns, keys already held (e.g. the Tab of Alt+Tab) must not fire edges.
    const bool prevDown = justActivated ? down : (old & 1) != 0;
    g_keys[vk].store(static_cast<uint8_t>((down ? 1 : 0) | (prevDown ? 2 : 0)), std::memory_order_relaxed);
  }
}

bool IsDown(uint16_t vk) { return vk > 0 && vk <= kMaxVk && (g_keys[vk].load(std::memory_order_relaxed) & 1) != 0; }

bool Pressed(uint16_t vk) { return vk > 0 && vk <= kMaxVk && g_keys[vk].load(std::memory_order_relaxed) == 1; }

bool Released(uint16_t vk) { return vk > 0 && vk <= kMaxVk && g_keys[vk].load(std::memory_order_relaxed) == 2; }

bool Pressed(const Hotkey& hk) {
  if (!hk.Valid() || hk.vk > kMaxVk) return false;
  if (g_capturing.load(std::memory_order_relaxed)) return false;
  if (!Pressed(hk.vk)) return false;
  if (ImGuiWantsText()) return false;
  // A binding whose key is itself a modifier ("LShift") must not demand that modifier be up.
  const bool ctrl = IsCtrlVk(hk.vk) ? hk.ctrl : IsDown(VK_CONTROL);
  const bool shift = IsShiftVk(hk.vk) ? hk.shift : IsDown(VK_SHIFT);
  const bool alt = IsAltVk(hk.vk) ? hk.alt : IsDown(VK_MENU);
  return ctrl == hk.ctrl && shift == hk.shift && alt == hk.alt;
}

void BeginCapture() {
  try {
    Capture& c = Cap();
    std::lock_guard lock(c.mutex);
    c.active = true;
    c.ignore.reset();
    for (int vk = 1; vk <= 254; ++vk)
      if (IsDown(static_cast<uint16_t>(vk))) c.ignore.set(static_cast<size_t>(vk));
    g_capturing.store(true, std::memory_order_relaxed);
  } catch (...) {
  }
}

bool Capturing() { return g_capturing.load(std::memory_order_relaxed); }

bool PollCapture(Hotkey& out) {
  try {
    Capture& c = Cap();
    std::lock_guard lock(c.mutex);
    if (!c.active) return false;
    for (int vk = 1; vk <= 254; ++vk)
      if (c.ignore.test(static_cast<size_t>(vk)) && !IsDown(static_cast<uint16_t>(vk))) c.ignore.reset(static_cast<size_t>(vk));

    auto fresh = [&](uint16_t vk) { return !c.ignore.test(vk) && Pressed(vk); };
    auto finish = [&]() {
      c.active = false;
      g_capturing.store(false, std::memory_order_relaxed);
      return true;
    };
    if (fresh(VK_ESCAPE)) return finish();   // cancelled: `out` keeps the previous binding
    if (fresh(VK_BACK) || fresh(VK_DELETE)) {
      out = Hotkey{};
      return finish();
    }
    for (int vk = 1; vk <= 254; ++vk) {
      const auto key = static_cast<uint16_t>(vk);
      if (!IsCapturable(key) || !fresh(key)) continue;
      out = Hotkey{key, IsDown(VK_CONTROL), IsDown(VK_SHIFT), IsDown(VK_MENU)};
      return finish();
    }
    return false;
  } catch (...) {
    return false;
  }
}

void CancelCapture() {
  try {
    Capture& c = Cap();
    std::lock_guard lock(c.mutex);
    c.active = false;
    c.ignore.reset();
  } catch (...) {
  }
  g_capturing.store(false, std::memory_order_relaxed);
}

const char* VkName(uint16_t vk) {
  if (vk == 0) return "Unbound";
  if (vk > kMaxVk) return "?";
  return Names().names[vk];
}

}  // namespace hotkeys
}  // namespace cg
