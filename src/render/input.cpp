#include "render/input.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <xinput.h>
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define CG_RETURN_ADDRESS() _ReturnAddress()
#else
#define CG_RETURN_ADDRESS() __builtin_return_address(0)
#endif

#include <imgui.h>
#include <imgui_impl_win32.h>

#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"
#include "mem/hook.h"
#include "mem/module.h"

// imgui_impl_win32.h deliberately leaves this declaration to the user (it needs <windows.h>).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace cg::render::input {
namespace {

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
HWND g_hwnd = nullptr;
WNDPROC g_origWndProc = nullptr;
bool g_unicodeWindow = true;
bool g_wndProcRestoreFailed = false;
uintptr_t g_selfBase = 0, g_selfEnd = 0;

std::atomic<bool> g_captured{false};
std::atomic<bool> g_wantMouse{false}, g_wantKeyboard{false};
std::atomic<bool> g_blockAll{true};   // "input.block_all", refreshed by SetImGuiWants each frame
std::atomic<int> g_inside{0};
std::atomic<uint64_t> g_lastLegacyKeyMs{0}, g_lastLegacyMouseMs{0};

std::mutex g_clipMutex;
bool g_gameClipActive = false;
RECT g_gameClipRect{};
POINT g_frozenCursor{};

struct QueuedMsg {
  UINT msg;
  WPARAM w;
  LPARAM l;
};
std::mutex g_queueMutex;
std::vector<QueuedMsg> g_queue;

struct InsideGuard {
  InsideGuard() { g_inside.fetch_add(1, std::memory_order_acq_rel); }
  ~InsideGuard() { g_inside.fetch_sub(1, std::memory_order_acq_rel); }
};

bool FromUs(void* returnAddress) {
  const auto a = reinterpret_cast<uintptr_t>(returnAddress);
  return a >= g_selfBase && a < g_selfEnd;
}

bool BlockAll() { return g_blockAll.load(std::memory_order_relaxed); }
bool BlockMouseNow() { return g_captured.load() && (g_wantMouse.load() || BlockAll()); }
bool BlockKeyboardNow() { return g_captured.load() && (g_wantKeyboard.load() || BlockAll()); }

void Enqueue(UINT msg, WPARAM w, LPARAM l) {
  std::lock_guard lock(g_queueMutex);
  if (g_queue.size() < 4096) g_queue.push_back({msg, w, l});
}

LPARAM CursorClientLParam() {
  POINT p{};
  if (!GetCursorPos(&p) || !ScreenToClient(g_hwnd, &p)) return 0;
  return MAKELPARAM(p.x, p.y);
}

// ---------------------------------------------------------------------------------------------
// Original function pointers (MinHook trampolines)
// ---------------------------------------------------------------------------------------------
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBufferFn = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
using GetKeyStateFn = SHORT(WINAPI*)(int);
using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using DIGetDeviceStateFn = HRESULT(WINAPI*)(IDirectInputDevice8W*, DWORD, LPVOID);
using DIGetDeviceDataFn = HRESULT(WINAPI*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

GetRawInputDataFn o_GetRawInputData = nullptr;
GetRawInputBufferFn o_GetRawInputBuffer = nullptr;
ClipCursorFn o_ClipCursor = nullptr;
SetCursorPosFn o_SetCursorPos = nullptr;
GetCursorPosFn o_GetCursorPos = nullptr;
GetAsyncKeyStateFn o_GetAsyncKeyState = nullptr;
GetKeyStateFn o_GetKeyState = nullptr;
GetKeyboardStateFn o_GetKeyboardState = nullptr;

struct XInputHook {
  std::string name;
  XInputGetStateFn original = nullptr;
};
XInputHook g_xinput[3];   // xinput1_4, xinput1_3, xinput9_1_0

struct DIHook {
  std::string name;
  DIGetDeviceStateFn getState = nullptr;
  DIGetDeviceDataFn getData = nullptr;
};
DIHook g_di[2];   // A and W device vtables (may share code)
std::mutex g_diTypeMutex;
std::unordered_map<void*, BYTE> g_diDevType;   // device -> DI8DEVTYPE_*

std::vector<std::string> g_hookNames;

// ---------------------------------------------------------------------------------------------
// Raw input: filtering for the game, synthesis for ImGui
// ---------------------------------------------------------------------------------------------
constexpr USHORT kMouseReleaseFlags = RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP |
                                      RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;

void FilterRawForGame(RAWINPUT* ri) {
  if (ri->header.dwType == RIM_TYPEMOUSE && BlockMouseNow()) {
    RAWMOUSE& m = ri->data.mouse;
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
      m.lLastX = 0;
      m.lLastY = 0;
    }
    m.usButtonFlags &= kMouseReleaseFlags;
    m.usButtonData = 0;
  } else if (ri->header.dwType == RIM_TYPEKEYBOARD && BlockKeyboardNow()) {
    RAWKEYBOARD& k = ri->data.keyboard;
    if (!(k.Flags & RI_KEY_BREAK)) {
      k.MakeCode = KEYBOARD_OVERRUN_MAKE_CODE;
      k.VKey = 0xFF;
    }
  }
}

void SynthesizeFromRaw(const RAWINPUT* ri) {
  const uint64_t now = util::NowMs();
  if (ri->header.dwType == RIM_TYPEMOUSE) {
    if (now - g_lastLegacyMouseMs.load() < 1500) return;
    const RAWMOUSE& m = ri->data.mouse;
    const LPARAM pos = CursorClientLParam();
    struct Map {
      USHORT flag;
      UINT msg;
      WPARAM w;
    };
    static constexpr Map kButtons[] = {
        {RI_MOUSE_LEFT_BUTTON_DOWN, WM_LBUTTONDOWN, 0},    {RI_MOUSE_LEFT_BUTTON_UP, WM_LBUTTONUP, 0},
        {RI_MOUSE_RIGHT_BUTTON_DOWN, WM_RBUTTONDOWN, 0},   {RI_MOUSE_RIGHT_BUTTON_UP, WM_RBUTTONUP, 0},
        {RI_MOUSE_MIDDLE_BUTTON_DOWN, WM_MBUTTONDOWN, 0},  {RI_MOUSE_MIDDLE_BUTTON_UP, WM_MBUTTONUP, 0},
        {RI_MOUSE_BUTTON_4_DOWN, WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON1)}, {RI_MOUSE_BUTTON_4_UP, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1)},
        {RI_MOUSE_BUTTON_5_DOWN, WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON2)}, {RI_MOUSE_BUTTON_5_UP, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2)},
    };
    for (const Map& b : kButtons)
      if (m.usButtonFlags & b.flag) Enqueue(b.msg, b.w, pos);
    if (m.usButtonFlags & RI_MOUSE_WHEEL) Enqueue(WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<SHORT>(m.usButtonData)), pos);
    if (m.usButtonFlags & RI_MOUSE_HWHEEL) Enqueue(WM_MOUSEHWHEEL, MAKEWPARAM(0, static_cast<SHORT>(m.usButtonData)), pos);
  } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
    if (now - g_lastLegacyKeyMs.load() < 1500) return;
    const RAWKEYBOARD& k = ri->data.keyboard;
    if (k.VKey == 0 || k.VKey >= 0xFF || k.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE) return;
    const bool up = k.Flags & RI_KEY_BREAK;
    LPARAM l = static_cast<LPARAM>(k.MakeCode & 0xFF) << 16;
    if (k.Flags & RI_KEY_E0) l |= 1 << 24;
    if (up) l |= (1u << 30) | (1u << 31);
    Enqueue(up ? WM_KEYUP : WM_KEYDOWN, k.VKey, l);
    if (!up) {
      BYTE state[256]{};
      if (o_GetKeyboardState ? o_GetKeyboardState(state) : GetKeyboardState(state)) {
        wchar_t chars[8]{};
        // Flag 0x4: do not change the keyboard (dead key) state (Windows 10 1607+).
        const int n = ToUnicodeEx(k.VKey, k.MakeCode, state, chars, 8, 0x4, GetKeyboardLayout(0));
        for (int i = 0; i < n; ++i)
          if (chars[i] >= 0x20) Enqueue(WM_CHAR, chars[i], l);
      }
    }
  }
}

UINT WINAPI HkGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT headerSize) {
  InsideGuard guard;
  const UINT r = o_GetRawInputData(h, cmd, data, size, headerSize);
  if (FromUs(CG_RETURN_ADDRESS()) || !g_captured.load()) return r;
  if (cmd == RID_INPUT && data && r != static_cast<UINT>(-1) && r >= sizeof(RAWINPUTHEADER)) {
    FilterRawForGame(static_cast<RAWINPUT*>(data));
  }
  return r;
}

UINT WINAPI HkGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT headerSize) {
  InsideGuard guard;
  const UINT count = o_GetRawInputBuffer(data, size, headerSize);
  if (FromUs(CG_RETURN_ADDRESS()) || !data || count == 0 || count == static_cast<UINT>(-1)) return count;
  // Buffered reads drain WM_INPUT, so this is the only place we can see these events.
  RAWINPUT* ri = data;
  for (UINT i = 0; i < count; ++i) {
    SynthesizeFromRaw(ri);
    if (g_captured.load()) FilterRawForGame(ri);
    // NEXTRAWINPUTBLOCK, spelled out: MinGW's header references an undefined QWORD typedef.
    ri = reinterpret_cast<RAWINPUT*>((reinterpret_cast<uintptr_t>(ri) + ri->header.dwSize + 7) & ~uintptr_t{7});
  }
  return count;
}

// ---------------------------------------------------------------------------------------------
// Cursor and key-state APIs
// ---------------------------------------------------------------------------------------------
BOOL WINAPI HkClipCursor(const RECT* rect) {
  InsideGuard guard;
  if (FromUs(CG_RETURN_ADDRESS())) return o_ClipCursor(rect);
  {
    std::lock_guard lock(g_clipMutex);
    g_gameClipActive = rect != nullptr;
    if (rect) g_gameClipRect = *rect;
  }
  if (g_captured.load()) {
    o_ClipCursor(nullptr);   // keep the cursor free while the menu is open; re-applied on close
    return TRUE;
  }
  return o_ClipCursor(rect);
}

BOOL WINAPI HkSetCursorPos(int x, int y) {
  InsideGuard guard;
  if (!FromUs(CG_RETURN_ADDRESS()) && g_captured.load()) return TRUE;
  return o_SetCursorPos(x, y);
}

BOOL WINAPI HkGetCursorPos(LPPOINT p) {
  InsideGuard guard;
  if (!FromUs(CG_RETURN_ADDRESS()) && BlockMouseNow() && p) {
    *p = g_frozenCursor;
    return TRUE;
  }
  return o_GetCursorPos(p);
}

SHORT WINAPI HkGetAsyncKeyState(int vk) {
  InsideGuard guard;
  if (!FromUs(CG_RETURN_ADDRESS()) && g_captured.load()) {
    const bool mouseKey = vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
    if (mouseKey ? BlockMouseNow() : BlockKeyboardNow()) return 0;
  }
  return o_GetAsyncKeyState(vk);
}

SHORT WINAPI HkGetKeyState(int vk) {
  InsideGuard guard;
  if (!FromUs(CG_RETURN_ADDRESS()) && BlockKeyboardNow()) return 0;
  return o_GetKeyState(vk);
}

BOOL WINAPI HkGetKeyboardState(PBYTE state) {
  InsideGuard guard;
  const BOOL r = o_GetKeyboardState(state);
  if (r && state && !FromUs(CG_RETURN_ADDRESS()) && BlockKeyboardNow()) ZeroMemory(state, 256);
  return r;
}

template <int I> DWORD WINAPI HkXInputGetState(DWORD user, XINPUT_STATE* st) {
  InsideGuard guard;
  const DWORD r = g_xinput[I].original(user, st);
  if (r == ERROR_SUCCESS && st && !FromUs(CG_RETURN_ADDRESS()) && g_captured.load()) st->Gamepad = XINPUT_GAMEPAD{};
  return r;
}
constexpr XInputGetStateFn kXInputDetours[3] = {&HkXInputGetState<0>, &HkXInputGetState<1>, &HkXInputGetState<2>};
constexpr const char* kXInputDlls[3] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};

// ---------------------------------------------------------------------------------------------
// DirectInput 8
// ---------------------------------------------------------------------------------------------
BYTE DeviceType(IDirectInputDevice8W* dev) {
  {
    std::lock_guard lock(g_diTypeMutex);
    auto it = g_diDevType.find(dev);
    if (it != g_diDevType.end()) return it->second;
  }
  DIDEVICEINSTANCEW info{};
  info.dwSize = sizeof(info);
  BYTE type = 0;
  if (SUCCEEDED(dev->GetDeviceInfo(&info))) type = static_cast<BYTE>(GET_DIDEVICE_TYPE(info.dwDevType));
  std::lock_guard lock(g_diTypeMutex);
  if (g_diDevType.size() > 64) g_diDevType.clear();
  g_diDevType[dev] = type;
  return type;
}

bool BlockDevice(BYTE type) {
  if (type == DI8DEVTYPE_MOUSE) return BlockMouseNow();
  if (type == DI8DEVTYPE_KEYBOARD) return BlockKeyboardNow();
  return g_captured.load();   // gamepads / joysticks
}

template <int I> HRESULT WINAPI HkDIGetDeviceState(IDirectInputDevice8W* dev, DWORD size, LPVOID data) {
  InsideGuard guard;
  const HRESULT hr = g_di[I].getState(dev, size, data);
  if (SUCCEEDED(hr) && data && size && g_captured.load() && !FromUs(CG_RETURN_ADDRESS()) && BlockDevice(DeviceType(dev))) {
    ZeroMemory(data, size);
  }
  return hr;
}

template <int I>
HRESULT WINAPI HkDIGetDeviceData(IDirectInputDevice8W* dev, DWORD objSize, LPDIDEVICEOBJECTDATA data, LPDWORD inOut, DWORD flags) {
  InsideGuard guard;
  const HRESULT hr = g_di[I].getData(dev, objSize, data, inOut, flags);
  if (FAILED(hr) || !data || !inOut || !g_captured.load() || FromUs(CG_RETURN_ADDRESS())) return hr;
  const BYTE type = DeviceType(dev);
  if (!BlockDevice(type) || objSize < sizeof(DIDEVICEOBJECTDATA)) return hr;
  // Keep releases only (keyboard keys / mouse buttons with the high bit clear), drop the rest.
  auto* bytes = reinterpret_cast<uint8_t*>(data);
  DWORD kept = 0;
  for (DWORD i = 0; i < *inOut; ++i) {
    auto* e = reinterpret_cast<DIDEVICEOBJECTDATA*>(bytes + static_cast<size_t>(i) * objSize);
    const bool isButton = type == DI8DEVTYPE_KEYBOARD || (type == DI8DEVTYPE_MOUSE && e->dwOfs >= DIMOFS_BUTTON0);
    if (isButton && !(e->dwData & 0x80)) {
      if (kept != i) memmove(bytes + static_cast<size_t>(kept) * objSize, e, objSize);
      ++kept;
    }
  }
  *inOut = kept;
  return hr;
}

void HookDirectInput() {
  HMODULE di = GetModuleHandleW(L"dinput8.dll");
  if (!di) return;
  using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
  auto create = reinterpret_cast<CreateFn>(GetProcAddress(di, "DirectInput8Create"));
  if (!create) return;
  const IID* iids[2] = {&IID_IDirectInput8A, &IID_IDirectInput8W};
  void* hooked[4]{};
  int hookedCount = 0;
  for (int i = 0; i < 2; ++i) {
    IDirectInput8W* dinput = nullptr;
    if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, *iids[i], reinterpret_cast<void**>(&dinput), nullptr)) || !dinput) continue;
    IDirectInputDevice8W* dev = nullptr;
    if (SUCCEEDED(dinput->CreateDevice(GUID_SysKeyboard, &dev, nullptr)) && dev) {
      void** vtbl = *reinterpret_cast<void***>(dev);
      void* getState = vtbl[9];
      void* getData = vtbl[10];
      auto already = [&](void* p) {
        for (int k = 0; k < hookedCount; ++k)
          if (hooked[k] == p) return true;
        return false;
      };
      const std::string suffix = i == 0 ? "A" : "W";
      if (!already(getState)) {
        g_di[i].name = "input.dinput8.GetDeviceState" + suffix;
        void* detour = i == 0 ? reinterpret_cast<void*>(&HkDIGetDeviceState<0>) : reinterpret_cast<void*>(&HkDIGetDeviceState<1>);
        if (mem::Hooks::Get().Install(g_di[i].name, getState, detour, reinterpret_cast<void**>(&g_di[i].getState))) {
          g_hookNames.push_back(g_di[i].name);
          hooked[hookedCount++] = getState;
        }
      }
      if (!already(getData)) {
        const std::string name = "input.dinput8.GetDeviceData" + suffix;
        void* detour = i == 0 ? reinterpret_cast<void*>(&HkDIGetDeviceData<0>) : reinterpret_cast<void*>(&HkDIGetDeviceData<1>);
        if (mem::Hooks::Get().Install(name, getData, detour, reinterpret_cast<void**>(&g_di[i].getData))) {
          g_hookNames.push_back(name);
          hooked[hookedCount++] = getData;
        }
      }
      dev->Release();
    }
    dinput->Release();
  }
  // If the A and W vtables share code, only one set of detours exists; route both indices to it.
  for (int i = 0; i < 2; ++i) {
    if (!g_di[i].getState) g_di[i].getState = g_di[1 - i].getState;
    if (!g_di[i].getData) g_di[i].getData = g_di[1 - i].getData;
  }
  if (hookedCount) log::Info("input", "DirectInput8 hooks installed ({})", hookedCount);
}

void HookXInput() {
  for (int i = 0; i < 3; ++i) {
    if (g_xinput[i].original) continue;
    HMODULE m = GetModuleHandleA(kXInputDlls[i]);
    if (!m) continue;
    void* fn = reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState"));
    if (!fn) continue;
    g_xinput[i].name = std::string("input.") + kXInputDlls[i] + ".XInputGetState";
    if (mem::Hooks::Get().Install(g_xinput[i].name, fn, reinterpret_cast<void*>(kXInputDetours[i]),
                                  reinterpret_cast<void**>(&g_xinput[i].original))) {
      g_hookNames.push_back(g_xinput[i].name);
    }
  }
}

template <class F> void HookUser32(const char* name, F* detour, F** original) {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  void* target = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, name)) : nullptr;
  if (!target) return;
  std::string hookName = std::string("input.user32.") + name;
  if (mem::Hooks::Get().Install(hookName, target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original))) {
    g_hookNames.push_back(std::move(hookName));
  } else {
    log::Warn("input", "Could not hook {}", name);
  }
}

// ---------------------------------------------------------------------------------------------
// WndProc
// ---------------------------------------------------------------------------------------------
bool IsLegacyKeyMsg(UINT m) { return (m >= WM_KEYFIRST && m <= WM_KEYLAST); }
bool IsLegacyMouseMsg(UINT m) { return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST); }

bool IsReleaseMsg(UINT m) {
  return m == WM_KEYUP || m == WM_SYSKEYUP || m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP || m == WM_XBUTTONUP;
}

LRESULT CallOriginal(HWND h, UINT m, WPARAM w, LPARAM l) {
  return g_unicodeWindow ? CallWindowProcW(g_origWndProc, h, m, w, l) : CallWindowProcA(g_origWndProc, h, m, w, l);
}

LRESULT CALLBACK HkWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  InsideGuard guard;
  const bool key = IsLegacyKeyMsg(m);
  const bool mouse = IsLegacyMouseMsg(m);
  if (key) g_lastLegacyKeyMs.store(util::NowMs());
  if (mouse) g_lastLegacyMouseMs.store(util::NowMs());

  if (key || mouse || m == WM_SETFOCUS || m == WM_KILLFOCUS || m == WM_ACTIVATEAPP || m == WM_MOUSELEAVE || m == WM_INPUTLANGCHANGE) {
    Enqueue(m, w, l);
  } else if (m == WM_INPUT) {
    RAWINPUT ri{};
    UINT size = sizeof(ri);
    // Called from our module: bypasses the filter, sees the unmodified event.
    if (o_GetRawInputData &&
        o_GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)) {
      SynthesizeFromRaw(&ri);
    }
  }

  if (g_captured.load()) {
    if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT) {
      // ImGui draws its own cursor when the game hides the OS one; otherwise show an arrow.
      if (!ImGui::GetCurrentContext() || !ImGui::GetIO().MouseDrawCursor) SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /* IDC_ARROW */));
      else SetCursor(nullptr);
      return TRUE;
    }
    // A lone Alt tap would open the system menu and stall the game's message loop.
    if (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_KEYMENU) return 0;
    const bool altF4 = m == WM_SYSKEYDOWN && w == VK_F4;
    if (key && !IsReleaseMsg(m) && !altF4 && BlockKeyboardNow()) return 0;
    if (mouse && !IsReleaseMsg(m) && BlockMouseNow()) return 0;
  }
  return CallOriginal(h, m, w, l);
}

}  // namespace

bool Install(HWND hwnd) {
  if (g_hwnd) return true;
  const mem::Module self = mem::Module::FromHandle(paths::Self());
  g_selfBase = self.base;
  g_selfEnd = self.base + self.size;

  g_hwnd = hwnd;
  g_unicodeWindow = IsWindowUnicode(hwnd);
  SetLastError(0);
  const LONG_PTR prev = g_unicodeWindow ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HkWndProc))
                                        : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HkWndProc));
  if (!prev && GetLastError() != 0) {
    log::Error("input", "SetWindowLongPtr(GWLP_WNDPROC) failed ({})", GetLastError());
    g_hwnd = nullptr;
    return false;
  }
  g_origWndProc = reinterpret_cast<WNDPROC>(prev);

  HookUser32("GetRawInputData", &HkGetRawInputData, &o_GetRawInputData);
  HookUser32("GetRawInputBuffer", &HkGetRawInputBuffer, &o_GetRawInputBuffer);
  HookUser32("ClipCursor", &HkClipCursor, &o_ClipCursor);
  HookUser32("SetCursorPos", &HkSetCursorPos, &o_SetCursorPos);
  HookUser32("GetCursorPos", &HkGetCursorPos, &o_GetCursorPos);
  HookUser32("GetAsyncKeyState", &HkGetAsyncKeyState, &o_GetAsyncKeyState);
  HookUser32("GetKeyState", &HkGetKeyState, &o_GetKeyState);
  HookUser32("GetKeyboardState", &HkGetKeyboardState, &o_GetKeyboardState);
  HookXInput();
  HookDirectInput();
  log::Info("input", "Input capture installed ({} hooks, {} window)", g_hookNames.size(), g_unicodeWindow ? "unicode" : "ansi");
  return true;
}

void Uninstall() {
  if (!g_hwnd) return;
  SetCaptured(false);
  for (const std::string& name : g_hookNames) mem::Hooks::Get().Remove(name);
  g_hookNames.clear();

  const auto current = g_unicodeWindow ? GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC) : GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
  if (!IsWindow(g_hwnd)) {
    // Window already destroyed: nothing references our WndProc any more.
  } else if (current == reinterpret_cast<LONG_PTR>(&HkWndProc)) {
    if (g_unicodeWindow) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
    else SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
  } else {
    // Something subclassed the window after us and will keep calling our WndProc.
    g_wndProcRestoreFailed = true;
    log::Warn("input", "WndProc was re-subclassed by another module; Consigliere will stay resident after unload");
  }
  for (int i = 0; i < 400 && g_inside.load() > 0; ++i) Sleep(5);
  {
    std::lock_guard lock(g_queueMutex);
    g_queue.clear();
  }
  g_hwnd = nullptr;
}

bool SafeToFree() { return !g_wndProcRestoreFailed && g_inside.load() == 0; }

void SetCaptured(bool captured) {
  const bool was = g_captured.exchange(captured);
  if (was == captured || !g_hwnd) return;
  if (captured) {
    HookXInput();   // the game may have loaded XInput after we installed
    if (o_GetCursorPos) o_GetCursorPos(&g_frozenCursor);
    if (o_ClipCursor) o_ClipCursor(nullptr);
  } else {
    std::lock_guard lock(g_clipMutex);
    if (o_ClipCursor && g_gameClipActive) o_ClipCursor(&g_gameClipRect);
    if (o_SetCursorPos) o_SetCursorPos(g_frozenCursor.x, g_frozenCursor.y);
  }
}

bool Captured() { return g_captured.load(); }

void SetImGuiWants(bool mouse, bool keyboard) {
  g_wantMouse.store(mouse);
  g_wantKeyboard.store(keyboard);
  g_blockAll.store(Config::Get().ReadBool("input.block_all", true), std::memory_order_relaxed);
}

void PumpToImGui() {
  std::vector<QueuedMsg> pending;
  {
    std::lock_guard lock(g_queueMutex);
    pending.swap(g_queue);
  }
  if (!g_hwnd || !ImGui::GetCurrentContext()) return;
  for (const QueuedMsg& q : pending) ImGui_ImplWin32_WndProcHandler(g_hwnd, q.msg, q.w, q.l);
}

}  // namespace cg::render::input
