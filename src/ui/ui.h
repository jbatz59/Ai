#pragma once
// UI entry point and tool-window registry.
#include <memory>
#include <string>
#include <vector>

namespace cg::ui {

void Init();        // after ImGui + fonts exist (render thread). Registers built-in tool windows.
void DrawFrame();   // overlay calls once per frame between NewFrame and Render
void Shutdown();

// Floating windows (RE toolkit, consoles). Drawn while the menu is open; a pinned window stays
// visible (click-through) when the menu is closed.
class Window {
 public:
  virtual ~Window() = default;
  virtual const char* Id() const = 0;         // stable, config key: "tools.scanner"
  virtual const char* Title() const = 0;      // "Memory Scanner"
  virtual const char* Icon() const;           // CG_ICON_* (default tools)
  virtual const char* Group() const;          // menu grouping: "Reverse Engineering", "Scripting", ...
  virtual void Draw() = 0;                    // body only; Begin/End handled by the registry
  virtual float DefaultWidth() const { return 720; }
  virtual float DefaultHeight() const { return 480; }
  bool open = false;
  bool pinned = false;
};

void RegisterWindow(std::unique_ptr<Window> w);
// <windows.h> #defines FindWindow as FindWindowA/FindWindowW. The declaration is shielded from the
// macro, and the A/W aliases keep `ui::FindWindow(id)` working in files that include <windows.h>
// before or after this header.
#pragma push_macro("FindWindow")
#undef FindWindow
Window* FindWindow(const std::string& id);
inline Window* FindWindowA(const std::string& id) { return FindWindow(id); }
inline Window* FindWindowW(const std::string& id) { return FindWindow(id); }
#pragma pop_macro("FindWindow")
const std::vector<std::unique_ptr<Window>>& Windows();
void OpenWindow(const std::string& id);   // also focuses it

// Cross-tool navigation (e.g. scanner result -> hex view / breakpoint / address table).
void GotoHexView(uintptr_t address);
void AddToAddressTable(uintptr_t address, const std::string& typeName, const std::string& description);
void SetBreakpointOn(uintptr_t address, int kind /*hwbp::Kind*/, int size);
void InspectStruct(uintptr_t address);

// Factories implemented in ui/tools/*.cpp
std::unique_ptr<Window> MakeScannerWindow();
std::unique_ptr<Window> MakeHexViewWindow();
std::unique_ptr<Window> MakeRttiBrowserWindow();
std::unique_ptr<Window> MakeStructDissectorWindow();
std::unique_ptr<Window> MakeBreakpointWindow();
std::unique_ptr<Window> MakeAddressTableWindow();
std::unique_ptr<Window> MakeBindingsWindow();
std::unique_ptr<Window> MakeHooksPatchesWindow();
std::unique_ptr<Window> MakeSignatureWindow();
std::unique_ptr<Window> MakeLogConsoleWindow();
std::unique_ptr<Window> MakeScriptConsoleWindow();
std::unique_ptr<Window> MakeGameLuaConsoleWindow();
std::unique_ptr<Window> MakeModulesWindow();
std::unique_ptr<Window> MakeGameApiExplorerWindow();   // tools.gameapi (game/script VM discovery)

// Tool windows that receive navigation requests implement these (registry dispatches by Id()).
class HexViewTarget { public: virtual ~HexViewTarget() = default; virtual void Goto(uintptr_t a) = 0; };
class AddressTableTarget { public: virtual ~AddressTableTarget() = default; virtual void AddEntry(uintptr_t a, const std::string& type, const std::string& desc) = 0; };
class BreakpointTarget { public: virtual ~BreakpointTarget() = default; virtual void SetOn(uintptr_t a, int kind, int size) = 0; };
class StructTarget { public: virtual ~StructTarget() = default; virtual void Inspect(uintptr_t a) = 0; };

}  // namespace cg::ui
