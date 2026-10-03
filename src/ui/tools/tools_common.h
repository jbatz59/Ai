#pragma once
// Shared helpers for the reverse-engineering tool windows (src/ui/tools/*.cpp).
//
// Header-only; everything here runs on the render thread unless a comment says otherwise.
// Other tool windows include this file, so keep it small and its signatures stable.
//
//   AddressTableTick()                 freeze + hotkey processing for the address table
//   kReGroup, k*Id                     menu group and the fixed tool-window ids
//   FindTool()                         registered window by id
//   SignatureTarget / DisassemblyTarget  extra navigation targets implemented by tool windows
//   GenerateSignatureFor(), GotoDisassembly()
//   BindingsResolver(), Eval()         address expressions that understand bindings symbols
//   FormatAddress(), AddressToExpr(), FormatCount()
//   CopyToClipboard()
//   AddressMenuItems(), AddressContextMenu()   the standard right-click menu for "an address"
//   AddressBar()                       ui::AddressInput that reports Enter-commits
//   InputString(), InputStringMultiline()      ImGui text input over std::string
//   ValueTypeCombo(), MonoFontScope, AsyncJob<R>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <imgui.h>

#include "ui/ui.h"
#include "ui/notify.h"
#include "ui/widgets.h"
#include "core/tasks.h"
#include "game/bindings.h"
#include "mem/expr.h"
#include "mem/hwbp.h"
#include "mem/module.h"
#include "mem/safe.h"
#include "mem/value.h"
#include "render/overlay.h"

namespace cg::ui {

// Address table per-frame work: re-resolves frozen entries, writes their frozen values and
// handles per-entry hotkeys. Implemented in address_table_window.cpp. Call once per frame on the
// render thread whether or not any window is open (it is cheap with an empty table and runs at
// most once per render::FrameCount(), so extra calls are harmless). The address table window
// also calls it from Draw() so freezing works while the window is open even if not wired up.
void AddressTableTick();

namespace tools {

inline constexpr const char* kReGroup = "Reverse Engineering";

inline constexpr const char* kScannerId = "tools.scanner";
inline constexpr const char* kHexViewId = "tools.hexview";
inline constexpr const char* kRttiId = "tools.rtti";
inline constexpr const char* kStructId = "tools.struct";
inline constexpr const char* kBreakpointsId = "tools.breakpoints";
inline constexpr const char* kAddressTableId = "tools.addresstable";
inline constexpr const char* kBindingsId = "tools.bindings";
inline constexpr const char* kHooksId = "tools.hooks";
inline constexpr const char* kSignatureId = "tools.signature";
inline constexpr const char* kLogId = "tools.log";
inline constexpr const char* kScriptConsoleId = "tools.scriptconsole";
inline constexpr const char* kGameLuaId = "tools.gamelua";
inline constexpr const char* kModulesId = "tools.modules";

// Registered window with the given id, or nullptr (string_view overload of ui::FindWindow).
inline Window* FindTool(std::string_view id) {
  for (const auto& w : Windows()) {
    if (w && id == w->Id()) return w.get();
  }
  return nullptr;
}

// Navigation targets beyond the ones in ui.h (implemented by the signature and hex view windows).
class SignatureTarget {
 public:
  virtual ~SignatureTarget() = default;
  virtual void GenerateFor(uintptr_t address) = 0;
};
class DisassemblyTarget {
 public:
  virtual ~DisassemblyTarget() = default;
  virtual void Disassemble(uintptr_t address) = 0;
};

// Opens the signature tool and starts generating a signature for `address`.
inline void GenerateSignatureFor(uintptr_t address) {
  OpenWindow(kSignatureId);
  if (auto* t = dynamic_cast<SignatureTarget*>(FindTool(kSignatureId))) t->GenerateFor(address);
}

// Opens the hex view on its Disassembly tab at `address` (plain hex view if unavailable).
inline void GotoDisassembly(uintptr_t address) {
  if (auto* t = dynamic_cast<DisassemblyTarget*>(FindTool(kHexViewId))) {
    OpenWindow(kHexViewId);
    t->Disassemble(address);
  } else {
    GotoHexView(address);
  }
}

// Expression identifiers resolve to bindings symbols: address/pointer/function symbols give
// their address, offset/constant symbols their value. Safe from any thread.
inline std::optional<uintptr_t> ResolveBindingSymbol(std::string_view name) {
  const game::Bindings& b = game::Bindings::Get();
  if (auto a = b.Addr(name)) return a;
  if (auto o = b.Offset(name)) return static_cast<uintptr_t>(*o);
  return std::nullopt;
}
inline mem::SymbolResolver BindingsResolver() { return &ResolveBindingSymbol; }

// mem::EvalAddress with the bindings resolver. Safe from any thread.
inline mem::ExprResult Eval(std::string_view expr) {
  static const mem::SymbolResolver resolver = BindingsResolver();
  return mem::EvalAddress(expr, resolver);
}

// "7FF6A1B20000": upper-case hex, at least 12 digits (every user-mode address lines up), no
// prefix. EvalAddress reads bare numbers as hex, so the text round-trips.
inline std::string FormatAddress(uintptr_t a) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%012llX", static_cast<unsigned long long>(a));
  return buf;
}

// Expression that finds `a` again after a restart: "module.dll+0x1A2B" inside a module (quoted
// when the name needs it), otherwise FormatAddress(a). Verified by re-evaluating it.
inline std::string AddressToExpr(uintptr_t a) {
  const std::string d = mem::Describe(a);
  const size_t plus = d.rfind('+');
  if (plus != std::string::npos && plus > 0 && plus + 1 < d.size()) {
    const std::string mod = d.substr(0, plus);
    const std::string off = d.substr(plus + 1);
    const bool plain = mod.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.") ==
                       std::string::npos;
    const std::string expr = plain ? mod + "+" + off : "\"" + mod + "\"+" + off;
    const mem::ExprResult r = mem::EvalAddress(expr);
    if (r.ok && r.value == a) return expr;
  }
  return FormatAddress(a);
}

// 1234567 -> "1,234,567".
inline std::string FormatCount(uint64_t n) {
  std::string digits = std::to_string(n);
  std::string out;
  out.reserve(digits.size() + digits.size() / 3);
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i && (digits.size() - i) % 3 == 0) out.push_back(',');
    out.push_back(digits[i]);
  }
  return out;
}

inline void CopyToClipboard(const std::string& text, const char* title = "Copied to clipboard") {
  ImGui::SetClipboardText(text.c_str());
  notify::Push(notify::Kind::Success, title, text.size() > 96 ? text.substr(0, 93) + "..." : text, 2.0f);
}

// Largest hardware-breakpoint width <= wanted that `a` is aligned to (DR7 requires alignment).
inline int DataBreakpointSize(uintptr_t a, int wanted) {
  for (int s : {8, 4, 2, 1}) {
    if (s <= wanted && a % static_cast<uintptr_t>(s) == 0) return s;
  }
  return 1;
}

struct AddressMenuOptions {
  const char* valueType = "i32";   // ValueTypeName for "Add to address table"
  int size = 4;                    // width for data breakpoints (clamped to alignment)
  const char* description = "";    // description for "Add to address table"
};

// Menu items for an address; call between BeginPopup*/EndPopup (or inside a BeginMenu).
inline void AddressMenuItems(uintptr_t a, const AddressMenuOptions& o = {}) {
  ImGui::TextDisabled("%s", mem::Describe(a).c_str());
  ImGui::Separator();
  if (ImGui::BeginMenu("Copy")) {
    if (ImGui::MenuItem("Address")) CopyToClipboard(FormatAddress(a), "Address copied");
    if (ImGui::MenuItem("Module-relative expression")) CopyToClipboard(AddressToExpr(a), "Expression copied");
    if (ImGui::MenuItem("Description")) CopyToClipboard(mem::Describe(a), "Description copied");
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Open in hex view")) GotoHexView(a);
  const bool exec = mem::IsExecutable(a);
  if (ImGui::MenuItem("Disassemble", nullptr, false, exec)) GotoDisassembly(a);
  if (ImGui::MenuItem("Add to address table")) AddToAddressTable(a, o.valueType ? o.valueType : "i32", o.description ? o.description : "");
  if (ImGui::BeginMenu("Breakpoint")) {
    const int sz = DataBreakpointSize(a, o.size < 1 ? 1 : o.size);
    char label[64];
    std::snprintf(label, sizeof label, "Find what writes (%d byte%s)", sz, sz == 1 ? "" : "s");
    if (ImGui::MenuItem(label)) SetBreakpointOn(a, static_cast<int>(mem::hwbp::Kind::Write), sz);
    std::snprintf(label, sizeof label, "Find what accesses (%d byte%s)", sz, sz == 1 ? "" : "s");
    if (ImGui::MenuItem(label)) SetBreakpointOn(a, static_cast<int>(mem::hwbp::Kind::ReadWrite), sz);
    if (ImGui::MenuItem("Break on execute", nullptr, false, exec)) SetBreakpointOn(a, static_cast<int>(mem::hwbp::Kind::Execute), 1);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem("Inspect struct")) InspectStruct(a);
  if (ImGui::MenuItem("Generate signature", nullptr, false, exec)) GenerateSignatureFor(a);
}

// Right-click popup on the last submitted item. Returns true while the popup is open.
inline bool AddressContextMenu(const char* strId, uintptr_t a, const AddressMenuOptions& o = {}) {
  if (!ImGui::BeginPopupContextItem(strId)) return false;
  AddressMenuItems(a, o);
  ImGui::EndPopup();
  return true;
}

// std::string-backed text inputs (imgui_stdlib is not vendored).
inline int InputStringResize(ImGuiInputTextCallbackData* d) {
  if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* s = static_cast<std::string*>(d->UserData);
    s->resize(static_cast<size_t>(d->BufTextLen));
    d->Buf = s->data();
  }
  return 0;
}
inline bool InputString(const char* label, std::string* s, ImGuiInputTextFlags flags = 0, const char* hint = nullptr) {
  flags |= ImGuiInputTextFlags_CallbackResize;
  if (hint) return ImGui::InputTextWithHint(label, hint, s->data(), s->capacity() + 1, flags, &InputStringResize, s);
  return ImGui::InputText(label, s->data(), s->capacity() + 1, flags, &InputStringResize, s);
}
inline bool InputStringMultiline(const char* label, std::string* s, const ImVec2& size = {0, 0}, ImGuiInputTextFlags flags = 0) {
  flags |= ImGuiInputTextFlags_CallbackResize;
  return ImGui::InputTextMultiline(label, s->data(), s->capacity() + 1, size, flags, &InputStringResize, s);
}

// ui::AddressInput wrapped so callers learn when the user committed with Enter. Returns true (and
// sets *out) only on Enter with an expression that evaluates; *out is untouched otherwise.
inline bool AddressBar(const char* id, std::string* expr, uintptr_t* out) {
  ImGui::BeginGroup();
  uintptr_t live = 0;
  AddressInput(id, expr, &live);
  ImGui::EndGroup();
  if (!ImGui::IsItemDeactivated()) return false;
  if (!ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) return false;
  const mem::ExprResult r = Eval(*expr);
  if (!r.ok) return false;
  *out = r.value;
  return true;
}

inline bool ValueTypeCombo(const char* label, mem::ValueType* t, bool numericOnly = false) {
  bool changed = false;
  if (ImGui::BeginCombo(label, mem::ValueTypeName(*t))) {
    for (mem::ValueType v : mem::kAllValueTypes) {
      if (numericOnly && !mem::IsNumeric(v)) continue;
      const bool selected = v == *t;
      if (ImGui::Selectable(mem::ValueTypeName(v), selected)) {
        *t = v;
        changed = true;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return changed;
}

// Pushes the monospace font (at the current size) for the scope when one is loaded.
class MonoFontScope {
 public:
  MonoFontScope() {
    if (ImFont* f = render::GetFonts().mono) {
      ImGui::PushFont(f, 0.0f);
      pushed_ = true;
    }
  }
  ~MonoFontScope() {
    if (pushed_) ImGui::PopFont();
  }
  MonoFontScope(const MonoFontScope&) = delete;
  MonoFontScope& operator=(const MonoFontScope&) = delete;

 private:
  bool pushed_ = false;
};

// One background computation on tasks::RunAsync whose result the UI polls each frame.
// The work function receives the shared State and must not capture the window (`this`): the
// window may be destroyed while the job runs. Starting a new run abandons the previous one
// (its cancel flag is set and its result is dropped).
template <class R>
class AsyncJob {
 public:
  struct State {
    std::atomic<bool> done{false};
    std::atomic<bool> cancel{false};
    std::atomic<float> progress{0.0f};
    R result{};
    std::string error;   // set by the work function (or from an escaped exception)
  };

  void Start(std::function<void(State&)> work) {
    Cancel();
    auto st = std::make_shared<State>();
    state_ = st;
    tasks::RunAsync([st, work = std::move(work)]() {
      try {
        work(*st);
      } catch (const std::exception& e) {
        st->error = e.what();
      } catch (...) {
        st->error = "unexpected error";
      }
      st->done.store(true, std::memory_order_release);
    });
  }
  bool Idle() const { return !state_; }
  bool Running() const { return state_ && !state_->done.load(std::memory_order_acquire); }
  bool Ready() const { return state_ && state_->done.load(std::memory_order_acquire); }
  float Progress() const { return state_ ? state_->progress.load(std::memory_order_relaxed) : 0.0f; }
  // Valid only while Ready().
  State& Get() { return *state_; }
  const State& Get() const { return *state_; }
  // Drops the current run (raising its cancel flag) or a consumed result; the job becomes Idle().
  void Cancel() {
    if (state_) state_->cancel.store(true, std::memory_order_relaxed);
    state_.reset();
  }

 private:
  std::shared_ptr<State> state_;
};

}  // namespace tools
}  // namespace cg::ui
