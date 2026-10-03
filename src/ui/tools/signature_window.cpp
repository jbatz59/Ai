// Signature workbench: generate unique IDA-style signatures, test patterns against a module,
// walk string literals to the code that references them, and turn the result into a bindings
// symbol (game::Bindings::Define) without leaving the game.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "core/util.h"
#include "game/bindings.h"
#include "mem/module.h"
#include "mem/pattern.h"
#include "mem/siggen.h"
#include "mem/value.h"
#include "render/fonts.h"
#include "render/theme.h"
#include "ui/notify.h"
#include "ui/tools/tools_common.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace cg::ui {
namespace {

using nlohmann::json;
namespace theme = render::theme;

constexpr size_t kMaxTestMatches = 1000;
constexpr size_t kListedMatches = 100;
constexpr size_t kMaxXrefs = 256;

enum Tab { kTabGenerate, kTabTest, kTabStrings, kTabBinding };

std::string Working() {
  static const char* kDots[] = {"", ".", "..", "..."};
  return std::string("Working") + kDots[static_cast<int>(ImGui::GetTime() * 3.0) % 4];
}

std::optional<mem::Module> FindModule(const std::string& name) {
  if (name.empty()) return mem::Module::Main();
  return mem::Module::Find(name);
}

bool IsMainModule(const std::string& name) { return name.empty() || util::IEquals(name, mem::Module::Main().name); }

// ------------------------------------------------------------------------------------------------
// Binding step model (mirrors the step grammar documented in game/bindings.h)
// ------------------------------------------------------------------------------------------------
enum class StepKind : uint8_t { Module, Pattern, String, Xref, FunctionStart, Rip, Call, Add, Sub, Deref, Vfunc, Symbol, Export, RttiVtable, RttiInstance };

struct StepSpec {
  StepKind kind;
  const char* label;
  const char* help;
};
constexpr StepSpec kStepSpecs[] = {
    {StepKind::Module, "module", "cursor = module base (empty = game exe)"},
    {StepKind::Pattern, "pattern", "cursor = index-th match of an IDA pattern in the module"},
    {StepKind::String, "string", "cursor = address of a string literal in the module"},
    {StepKind::Xref, "xref", "cursor = n-th instruction referencing the cursor"},
    {StepKind::FunctionStart, "function_start", "cursor = start of the function containing the cursor"},
    {StepKind::Rip, "rip", "cursor = cursor + len + int32 at (cursor + disp)"},
    {StepKind::Call, "call", "cursor = target of the E8/E9 rel32 at the cursor"},
    {StepKind::Add, "add", "cursor += value"},
    {StepKind::Sub, "sub", "cursor -= value"},
    {StepKind::Deref, "deref", "cursor = *(u64*)cursor (makes the symbol dynamic)"},
    {StepKind::Vfunc, "vfunc", "cursor = vtable[index] of the object at the cursor (dynamic)"},
    {StepKind::Symbol, "symbol", "cursor = value of another bindings symbol"},
    {StepKind::Export, "export", "cursor = exported function, \"dll!Function\""},
    {StepKind::RttiVtable, "rtti_vtable", "cursor = primary vtable of an RTTI class"},
    {StepKind::RttiInstance, "rtti_instance", "cursor = first live object of an RTTI class (slow, cached)"},
};

const StepSpec& SpecOf(StepKind k) {
  for (const StepSpec& s : kStepSpecs) {
    if (s.kind == k) return s;
  }
  return kStepSpecs[0];
}

struct Step {
  StepKind kind = StepKind::Pattern;
  std::string text;      // module / pattern / string / symbol / export / class / add-sub amount
  int a = 0;             // pattern index, xref index, rip disp, vfunc index
  int b = 0;             // rip instruction length
  bool wide = false;     // string
  std::string section;   // pattern (optional)
};

// JSON for one step, or nullopt with `err` set.
std::optional<json> StepToJson(const Step& s, std::string& err) {
  json j = json::object();
  switch (s.kind) {
    case StepKind::Module:
      j["module"] = s.text;
      break;
    case StepKind::Pattern:
      if (!mem::Pattern::Parse(s.text)) {
        err = "pattern step: invalid pattern";
        return std::nullopt;
      }
      j["pattern"] = util::Trim(s.text);
      if (s.a != 0) j["index"] = s.a;
      if (!util::Trim(s.section).empty()) j["section"] = util::Trim(s.section);
      break;
    case StepKind::String:
      if (s.text.empty()) {
        err = "string step: text is empty";
        return std::nullopt;
      }
      j["string"] = s.text;
      j["wide"] = s.wide;
      break;
    case StepKind::Xref:
      j["xref"] = s.a;
      break;
    case StepKind::FunctionStart:
      j["function_start"] = true;
      break;
    case StepKind::Rip:
      if (s.a < 0 || s.b <= s.a || s.b > 15) {
        err = "rip step: need 0 <= disp < len <= 15";
        return std::nullopt;
      }
      j["rip"] = json::array({s.a, s.b});
      break;
    case StepKind::Call:
      j["call"] = true;
      break;
    case StepKind::Add:
    case StepKind::Sub: {
      const auto v = util::ParseInt(s.text, false);
      if (!v) {
        err = std::string(SpecOf(s.kind).label) + " step: \"" + s.text + "\" is not a number (use 16 or 0x10)";
        return std::nullopt;
      }
      j[SpecOf(s.kind).label] = *v < 0 ? "-" + util::Hex(static_cast<uint64_t>(-*v)) : util::Hex(static_cast<uint64_t>(*v));
      break;
    }
    case StepKind::Deref:
      j["deref"] = true;
      break;
    case StepKind::Vfunc:
      if (s.a < 0) {
        err = "vfunc step: index must be >= 0";
        return std::nullopt;
      }
      j["vfunc"] = s.a;
      break;
    case StepKind::Symbol:
    case StepKind::Export:
    case StepKind::RttiVtable:
    case StepKind::RttiInstance:
      if (util::Trim(s.text).empty()) {
        err = std::string(SpecOf(s.kind).label) + " step: value is empty";
        return std::nullopt;
      }
      if (s.kind == StepKind::Export && util::Trim(s.text).find('!') == std::string::npos) {
        err = "export step: use \"module.dll!Function\"";
        return std::nullopt;
      }
      j[SpecOf(s.kind).label] = util::Trim(s.text);
      break;
  }
  return j;
}

bool ValidSymbolName(const std::string& n) {
  if (n.empty() || n.size() > 128) return false;
  for (char c : n) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':';
    if (!ok) return false;
  }
  return true;
}

constexpr const char* kKinds[] = {"address", "pointer", "function"};

// ------------------------------------------------------------------------------------------------
// Job results
// ------------------------------------------------------------------------------------------------
struct GenResult {
  uintptr_t address = 0;
  std::string module;
  uintptr_t rva = 0;
  std::string signature;
  std::optional<uintptr_t> functionStart;
};

struct TestResult {
  std::vector<uintptr_t> matches;
  double ms = 0;
  bool truncated = false;
};

struct Xref {
  uintptr_t at = 0;
  std::optional<uintptr_t> function;
};
struct StringResult {
  uintptr_t string = 0;
  std::vector<Xref> xrefs;
  bool truncated = false;
};

struct SigResult {
  uintptr_t address = 0;
  std::string signature;
};

struct DefineResult {
  bool ok = false;
  std::string name;
};

class SignatureWindow final : public Window, public tools::SignatureTarget {
 public:
  const char* Id() const override { return tools::kSignatureId; }
  const char* Title() const override { return "Signatures"; }
  const char* Icon() const override { return CG_ICON_SCRIPT; }
  const char* Group() const override { return tools::kReGroup; }
  float DefaultWidth() const override { return 820; }
  float DefaultHeight() const override { return 560; }

  void GenerateFor(uintptr_t address) override {
    genExpr_ = tools::FormatAddress(address);
    StartGenerate(address);
    selectTab_ = kTabGenerate;
  }

  void Draw() override {
    PollModules();
    PollJobs();
    if (!ImGui::BeginTabBar("##sigtabs")) return;
    const int forced = selectTab_;
    selectTab_ = -1;
    auto flags = [forced](int tab) { return forced == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None; };
    if (ImGui::BeginTabItem("Generate", nullptr, flags(kTabGenerate))) {
      DrawGenerateTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Test pattern", nullptr, flags(kTabTest))) {
      DrawTestTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Strings & xrefs", nullptr, flags(kTabStrings))) {
      DrawStringsTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Save as binding", nullptr, flags(kTabBinding))) {
      DrawBindingTab();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

 private:
  using GenJob = tools::AsyncJob<GenResult>;
  using TestJob = tools::AsyncJob<TestResult>;
  using StringJob = tools::AsyncJob<StringResult>;
  using SigJob = tools::AsyncJob<SigResult>;
  using DefineJob = tools::AsyncJob<DefineResult>;
  using ModulesJob = tools::AsyncJob<std::vector<mem::Module>>;

  // ------------------------------------------------------------------------------------------
  // Module list (shared by the tabs)
  // ------------------------------------------------------------------------------------------
  void RefreshModules() {
    modulesJob_.Start([](ModulesJob::State& st) { st.result = mem::Module::All(); });
  }

  void PollModules() {
    if (!modulesRequested_) {
      modulesRequested_ = true;
      RefreshModules();
    }
    if (modulesJob_.Ready()) {
      modules_ = std::move(modulesJob_.Get().result);
      std::sort(modules_.begin(), modules_.end(), [](const mem::Module& a, const mem::Module& b) { return a.name < b.name; });
      modulesJob_.Cancel();
    }
  }

  bool ModuleCombo(const char* id, std::string* name) {
    bool changed = false;
    const std::string shown = name->empty() ? mem::Module::Main().name : *name;
    if (ImGui::BeginCombo(id, shown.c_str(), ImGuiComboFlags_HeightLarge)) {
      if (ImGui::IsWindowAppearing()) {
        moduleFilter_.clear();
        ImGui::SetKeyboardFocusHere();
      }
      ImGui::SetNextItemWidth(-FLT_MIN);
      tools::InputString("##modfilter", &moduleFilter_, 0, "Filter modules...");
      if (modules_.empty()) ImGui::TextColored(theme::Colors().textDim, "%s", modulesJob_.Running() ? Working().c_str() : "No modules");
      for (const mem::Module& m : modules_) {
        if (!moduleFilter_.empty() && !util::IContains(m.name, moduleFilter_)) continue;
        const bool sel = util::IEquals(m.name, shown);
        if (ImGui::Selectable(m.name.c_str(), sel)) {
          *name = m.name;
          changed = true;
        }
        if (sel && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) RefreshModules();
    ImGui::SetItemTooltip("Re-enumerate loaded modules");
    return changed;
  }

  // ------------------------------------------------------------------------------------------
  // Job polling
  // ------------------------------------------------------------------------------------------
  void PollJobs() {
    if (sigJob_.Ready()) {
      SigJob::State& st = sigJob_.Get();
      if (st.error.empty()) tools::CopyToClipboard(st.result.signature, "Signature copied");
      else notify::Push(notify::Kind::Warning, "No signature", st.error);
      sigJob_.Cancel();
      sigFor_ = 0;
    }
    if (defineJob_.Ready()) {
      DefineJob::State& st = defineJob_.Get();
      defineError_ = st.error;
      defineDone_ = st.result.ok;
      if (st.result.ok) notify::Push(notify::Kind::Success, "Binding saved", st.result.name);
      definedName_ = st.result.name;
      defineJob_.Cancel();
    }
  }

  // Background signature for a single address (used by the xref and match lists).
  void StartRowSignature(uintptr_t address) {
    sigFor_ = address;
    sigJob_.Start([address](SigJob::State& st) {
      st.result.address = address;
      const std::optional<mem::Module> m = mem::Module::Containing(address);
      if (!m) {
        st.error = mem::Describe(address) + " is not inside a module";
        return;
      }
      std::optional<std::string> sig = mem::GenerateSignature(address, *m);
      if (!sig) {
        st.error = "No unique signature within 96 bytes at " + mem::Describe(address);
        return;
      }
      st.result.signature = std::move(*sig);
    });
  }

  void SigButton(const char* label, uintptr_t address, const char* tooltip) {
    if (sigJob_.Running() && sigFor_ == address) {
      ImGui::TextColored(theme::Colors().textDim, "%s", Working().c_str());
      return;
    }
    ImGui::BeginDisabled(sigJob_.Running());
    if (ImGui::SmallButton(label)) StartRowSignature(address);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", tooltip);
  }

  // ------------------------------------------------------------------------------------------
  // Generate
  // ------------------------------------------------------------------------------------------
  void StartGenerate(uintptr_t address) {
    genSig_.clear();
    const size_t maxBytes = static_cast<size_t>(genMaxBytes_);
    genJob_.Start([address, maxBytes](GenJob::State& st) {
      GenResult& r = st.result;
      r.address = address;
      const std::optional<mem::Module> m = mem::Module::Containing(address);
      if (!m) {
        st.error = mem::Describe(address) + " is not inside a loaded module";
        return;
      }
      r.module = m->name;
      r.rva = m->Rva(address);
      r.functionStart = mem::FunctionStart(address);
      std::optional<std::string> sig = mem::GenerateSignature(address, *m, maxBytes);
      if (!sig) {
        st.error = "No unique signature within " + std::to_string(maxBytes) +
                   " bytes. Try the function start, a nearby instruction, or a larger limit.";
        return;
      }
      r.signature = std::move(*sig);
    });
  }

  void DrawGenerateTab() {
    const auto& c = theme::Colors();
    SectionHeader("Address");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
    uintptr_t a = 0;
    bool go = tools::AddressBar("##genaddr", &genExpr_, &a);
    ImGui::SameLine();
    ImGui::BeginDisabled(genJob_.Running());
    if (ImGui::Button("Generate")) {
      const mem::ExprResult r = tools::Eval(genExpr_);
      if (r.ok) {
        a = r.value;
        go = true;
      } else {
        notify::Push(notify::Kind::Error, "Invalid address", r.error);
      }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    ImGui::SliderInt("Max bytes", &genMaxBytes_, 16, 256);
    if (go && !genJob_.Running()) StartGenerate(a);

    ImGui::TextColored(c.textFaint, "Builds the shortest pattern starting at the address that is unique in the module's code.");
    ImGui::Spacing();

    if (genJob_.Idle()) {
      ImGui::TextColored(c.textFaint, "Enter an address (any expression) and press Generate.");
      return;
    }
    if (genJob_.Running()) {
      ImGui::TextColored(c.textDim, "%s", Working().c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Cancel")) genJob_.Cancel();
      return;
    }
    const GenJob::State& st = genJob_.Get();
    const GenResult& r = st.result;
    SectionHeader("Result");
    ImGui::Text("%s", mem::Describe(r.address).c_str());
    if (!r.module.empty()) {
      ImGui::SameLine();
      ImGui::TextColored(c.textDim, "(%s, RVA %s)", r.module.c_str(), util::Hex(r.rva).c_str());
    }
    if (r.functionStart) {
      ImGui::TextColored(c.textDim, "Function start %s (+%s)", mem::Describe(*r.functionStart).c_str(),
                         util::Hex(r.address - *r.functionStart).c_str());
      if (*r.functionStart != r.address) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Generate for function start")) {
          genExpr_ = tools::FormatAddress(*r.functionStart);
          StartGenerate(*r.functionStart);
          return;
        }
      }
    }
    if (!st.error.empty()) {
      ImGui::TextColored(c.danger, "%s", st.error.c_str());
      return;
    }
    if (genSig_.empty()) genSig_ = r.signature;
    {
      tools::MonoFontScope mono;
      ImGui::SetNextItemWidth(-FLT_MIN);
      tools::InputString("##sigout", &genSig_, ImGuiInputTextFlags_ReadOnly);
    }
    const size_t bytes = mem::Pattern::Parse(r.signature) ? mem::Pattern::Parse(r.signature)->Size() : 0;
    ImGui::TextColored(c.success, "Unique in %s (%zu bytes)", r.module.c_str(), bytes);
    if (ImGui::Button("Copy")) tools::CopyToClipboard(r.signature, "Signature copied");
    ImGui::SameLine();
    if (ImGui::Button("Test")) {
      testPattern_ = r.signature;
      testModule_ = r.module;
      testExecOnly_ = true;
      StartTest();
      selectTab_ = kTabTest;
    }
    ImGui::SameLine();
    if (ImGui::Button("Use for binding")) {
      steps_.clear();
      if (!IsMainModule(r.module)) steps_.push_back(Step{StepKind::Module, r.module});
      steps_.push_back(Step{StepKind::Pattern, r.signature});
      selectTab_ = kTabBinding;
    }
    ImGui::SameLine();
    if (ImGui::Button("Open in hex view")) tools::GotoDisassembly(r.address);
  }

  // ------------------------------------------------------------------------------------------
  // Test pattern
  // ------------------------------------------------------------------------------------------
  void StartTest() {
    const std::optional<mem::Pattern> p = mem::Pattern::Parse(testPattern_);
    if (!p) return;
    const std::string module = testModule_;
    const bool execOnly = testExecOnly_;
    const mem::Pattern pattern = *p;
    testJob_.Start([pattern, module, execOnly](TestJob::State& st) {
      const std::optional<mem::Module> m = FindModule(module);
      if (!m) {
        st.error = "Module " + module + " is not loaded";
        return;
      }
      const auto t0 = std::chrono::steady_clock::now();
      st.result.matches = mem::FindAllInModule(pattern, *m, execOnly, kMaxTestMatches);
      st.result.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      st.result.truncated = st.result.matches.size() >= kMaxTestMatches;
    });
  }

  std::optional<uintptr_t> ResolveMatch(uintptr_t match) const {
    const uintptr_t at = match + static_cast<uintptr_t>(static_cast<intptr_t>(ripOffset_));
    if (ripMode_ == 1) return mem::ResolveRip(at, ripDisp_, ripLen_);
    if (ripMode_ == 2) return mem::ResolveCall(at);
    return at;
  }

  void DrawTestTab() {
    const auto& c = theme::Colors();
    const float fs = ImGui::GetFontSize();
    SectionHeader("Pattern");
    bool submit;
    {
      tools::MonoFontScope mono;
      ImGui::SetNextItemWidth(-FLT_MIN);
      submit = tools::InputString("##pattern", &testPattern_, ImGuiInputTextFlags_EnterReturnsTrue, "48 8B 05 ? ? ? ? 48 85 C0");
    }
    const std::optional<mem::Pattern> parsed = mem::Pattern::Parse(testPattern_);
    if (util::Trim(testPattern_).empty()) {
      ImGui::TextColored(c.textFaint, "IDA style: hex bytes, ? or ?? for any byte, 4? for a nibble.");
    } else if (!parsed) {
      ImGui::TextColored(c.danger, "Invalid pattern");
    } else {
      size_t wild = 0;
      for (uint8_t m : parsed->mask) wild += m != 0xFF ? 1 : 0;
      ImGui::TextColored(c.textDim, "%zu bytes, %zu wildcard%s", parsed->Size(), wild, wild == 1 ? "" : "s");
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Module");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 16.0f);
    ModuleCombo("##testmod", &testModule_);
    ImGui::SameLine();
    ImGui::Checkbox("Executable sections only", &testExecOnly_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!parsed || testJob_.Running());
    if (ImGui::Button("Scan") || (submit && parsed && !testJob_.Running())) StartTest();
    ImGui::EndDisabled();

    SectionHeader("Resolve each match");
    ImGui::SetNextItemWidth(fs * 9.0f);
    ImGui::Combo("##ripmode", &ripMode_, "none\0rip-relative\0call/jmp rel32\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 5.5f);
    ImGui::InputInt("offset", &ripOffset_);
    ImGui::SetItemTooltip("Instruction offset from the start of the match");
    if (ripMode_ == 1) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(fs * 5.5f);
      ImGui::InputInt("disp", &ripDisp_);
      ImGui::SetItemTooltip("Byte offset of the rel32 displacement inside the instruction (3 for 48 8B 05)");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(fs * 5.5f);
      ImGui::InputInt("len", &ripLen_);
      ImGui::SetItemTooltip("Instruction length (7 for 48 8B 05 xx xx xx xx)");
      ripDisp_ = std::clamp(ripDisp_, 0, 14);
      ripLen_ = std::clamp(ripLen_, ripDisp_ + 1, 15);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!parsed);
    if (ImGui::Button("Use for binding")) {
      steps_.clear();
      if (!IsMainModule(testModule_)) steps_.push_back(Step{StepKind::Module, testModule_});
      steps_.push_back(Step{StepKind::Pattern, util::Trim(testPattern_)});
      if (ripOffset_ > 0) steps_.push_back(Step{StepKind::Add, std::to_string(ripOffset_)});
      if (ripOffset_ < 0) steps_.push_back(Step{StepKind::Sub, std::to_string(-ripOffset_)});
      if (ripMode_ == 1) {
        Step s{StepKind::Rip};
        s.a = ripDisp_;
        s.b = ripLen_;
        steps_.push_back(s);
      } else if (ripMode_ == 2) {
        steps_.push_back(Step{StepKind::Call});
      }
      selectTab_ = kTabBinding;
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (testJob_.Idle()) return;
    if (testJob_.Running()) {
      ImGui::TextColored(c.textDim, "%s", Working().c_str());
      return;
    }
    const TestJob::State& st = testJob_.Get();
    if (!st.error.empty()) {
      ImGui::TextColored(c.danger, "%s", st.error.c_str());
      return;
    }
    const TestResult& r = st.result;
    const size_t n = r.matches.size();
    if (n == 0) Badge("NO MATCH", theme::U32(c.danger), theme::U32(c.text));
    else if (n == 1) Badge("UNIQUE", theme::U32(c.success), theme::U32(c.bg));
    else Badge("NOT UNIQUE", theme::U32(c.warning), theme::U32(c.bg));
    ImGui::SameLine();
    ImGui::Text("%s%s match%s in %.1f ms", tools::FormatCount(n).c_str(), r.truncated ? "+" : "", n == 1 ? "" : "es", r.ms);
    if (n > kListedMatches) {
      ImGui::SameLine();
      ImGui::TextColored(c.textFaint, "(first %zu listed)", kListedMatches);
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##matches", 4, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, fs * 2.5f);
    ImGui::TableSetupColumn("Match", ImGuiTableColumnFlags_WidthStretch, 1.5f);
    ImGui::TableSetupColumn(ripMode_ == 0 ? "Address" : "Resolved", ImGuiTableColumnFlags_WidthStretch, 1.5f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fs * 4.0f);
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(std::min(n, kListedMatches)));
    while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const uintptr_t m = r.matches[static_cast<size_t>(i)];
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(c.textDim, "%d", i);
        ImGui::TableSetColumnIndex(1);
        ImGui::Selectable(mem::Describe(m).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) tools::GotoDisassembly(m);
        tools::AddressContextMenu("##mctx", m);
        ImGui::TableSetColumnIndex(2);
        const std::optional<uintptr_t> t = ResolveMatch(m);
        if (t) {
          ImGui::Selectable(mem::Describe(*t).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
          if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) GotoHexView(*t);
          tools::AddressContextMenu("##tctx", *t);
        } else {
          ImGui::TextColored(c.danger, "??");
        }
        ImGui::TableSetColumnIndex(3);
        SigButton("sig", m, "Generate and copy a unique signature for this match");
        ImGui::PopID();
      }
    }
    ImGui::EndTable();
  }

  // ------------------------------------------------------------------------------------------
  // Strings & xrefs
  // ------------------------------------------------------------------------------------------
  void StartStringSearch() {
    if (strText_.empty()) return;
    const std::string text = strText_;
    const std::string module = strModule_;
    const bool wide = strWide_;
    strSearchedText_ = text;
    strSearchedWide_ = wide;
    strSearchedModule_ = module;
    strJob_.Start([text, module, wide](StringJob::State& st) {
      const std::optional<mem::Module> m = FindModule(module);
      if (!m) {
        st.error = "Module " + module + " is not loaded";
        return;
      }
      const std::optional<uintptr_t> s = mem::FindString(*m, text, wide);
      if (!s) {
        st.error = std::string(wide ? "Wide string" : "String") + " not found in " + m->name + "'s data sections";
        return;
      }
      st.result.string = *s;
      st.progress.store(0.3f);
      const std::vector<uintptr_t> xrefs = mem::FindXrefs(*m, *s, kMaxXrefs, &st.cancel);
      st.result.truncated = xrefs.size() >= kMaxXrefs;
      st.result.xrefs.reserve(xrefs.size());
      for (uintptr_t x : xrefs) {
        if (st.cancel.load(std::memory_order_relaxed)) break;
        st.result.xrefs.push_back(Xref{x, mem::FunctionStart(x)});
      }
    });
  }

  void BindingFromXref(size_t index, bool functionStart) {
    steps_.clear();
    if (!IsMainModule(strSearchedModule_)) steps_.push_back(Step{StepKind::Module, strSearchedModule_});
    Step s{StepKind::String, strSearchedText_};
    s.wide = strSearchedWide_;
    steps_.push_back(s);
    Step x{StepKind::Xref};
    x.a = static_cast<int>(index);
    steps_.push_back(x);
    if (functionStart) steps_.push_back(Step{StepKind::FunctionStart});
    kind_ = functionStart ? 2 : 0;
    selectTab_ = kTabBinding;
  }

  void DrawStringsTab() {
    const auto& c = theme::Colors();
    const float fs = ImGui::GetFontSize();
    SectionHeader("String literal");
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool submit = tools::InputString("##strtext", &strText_, ImGuiInputTextFlags_EnterReturnsTrue, "Exact text, e.g. a log message or asset name");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Module");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 16.0f);
    ModuleCombo("##strmod", &strModule_);
    ImGui::SameLine();
    ImGui::Checkbox("UTF-16", &strWide_);
    ImGui::SameLine();
    const bool running = strJob_.Running();
    ImGui::BeginDisabled(running || strText_.empty());
    if (ImGui::Button("Find references") || (submit && !running && !strText_.empty())) StartStringSearch();
    ImGui::EndDisabled();
    if (running) {
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) strJob_.Cancel();
      ImGui::TextColored(c.textDim, "%s", Working().c_str());
      return;
    }
    if (strJob_.Idle()) {
      ImGui::TextColored(c.textFaint, "Finds the literal, every instruction that references it, and the functions they live in.");
      return;
    }
    const StringJob::State& st = strJob_.Get();
    if (!st.error.empty()) {
      ImGui::TextColored(c.danger, "%s", st.error.c_str());
      return;
    }
    const StringResult& r = st.result;
    ImGui::Spacing();
    ImGui::TextUnformatted("String at");
    ImGui::SameLine();
    if (ImGui::TextLink(mem::Describe(r.string).c_str())) GotoHexView(r.string);
    tools::AddressContextMenu("##strctx", r.string);
    ImGui::SameLine();
    ImGui::TextColored(c.textDim, "- %s reference%s%s", tools::FormatCount(r.xrefs.size()).c_str(), r.xrefs.size() == 1 ? "" : "s",
                       r.truncated ? " (limit reached)" : "");
    if (r.xrefs.empty()) {
      ImGui::TextColored(c.warning, "No code references found (the string may be reached through a pointer table).");
      return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##xrefs", 4, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, fs * 2.5f);
    ImGui::TableSetupColumn("Reference", ImGuiTableColumnFlags_WidthStretch, 1.5f);
    ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch, 1.5f);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, fs * 15.0f);
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(r.xrefs.size()));
    while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const Xref& x = r.xrefs[static_cast<size_t>(i)];
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(c.textDim, "%d", i);
        ImGui::TableSetColumnIndex(1);
        ImGui::AlignTextToFramePadding();
        if (ImGui::TextLink(mem::Describe(x.at).c_str())) tools::GotoDisassembly(x.at);
        tools::AddressContextMenu("##xctx", x.at);
        ImGui::TableSetColumnIndex(2);
        ImGui::AlignTextToFramePadding();
        if (x.function) {
          if (ImGui::TextLink(mem::Describe(*x.function).c_str())) tools::GotoDisassembly(*x.function);
          tools::AddressContextMenu("##fctx", *x.function);
        } else {
          ImGui::TextColored(c.textFaint, "(no unwind info)");
        }
        ImGui::TableSetColumnIndex(3);
        SigButton("Sig", x.at, "Copy a unique signature for the referencing instruction");
        if (x.function) {
          ImGui::SameLine();
          SigButton("Fn sig", *x.function, "Copy a unique signature for the function start");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Bind")) ImGui::OpenPopup("##bindmenu");
        if (ImGui::BeginPopup("##bindmenu")) {
          if (ImGui::MenuItem("Bind the referencing instruction")) BindingFromXref(static_cast<size_t>(i), false);
          if (ImGui::MenuItem("Bind the function start", nullptr, false, x.function.has_value())) BindingFromXref(static_cast<size_t>(i), true);
          ImGui::EndPopup();
        }
        ImGui::PopID();
      }
    }
    ImGui::EndTable();
  }

  // ------------------------------------------------------------------------------------------
  // Save as binding
  // ------------------------------------------------------------------------------------------
  // Definition JSON, or nullopt with every problem listed in `errors`.
  std::optional<json> BuildDefinition(std::vector<std::string>& errors, std::vector<std::string>& warnings) const {
    json steps = json::array();
    bool dynamic = false;
    for (size_t i = 0; i < steps_.size(); ++i) {
      std::string err;
      std::optional<json> j = StepToJson(steps_[i], err);
      if (!j) errors.push_back("Step " + std::to_string(i + 1) + ": " + err);
      else steps.push_back(std::move(*j));
      dynamic |= steps_[i].kind == StepKind::Deref || steps_[i].kind == StepKind::Vfunc;
    }
    if (steps_.empty()) errors.push_back("Add at least one step");
    if (dynamic && kind_ != 1) errors.push_back("deref/vfunc steps are only allowed for pointer symbols");
    if (!dynamic && kind_ == 1) warnings.push_back("A pointer symbol normally has a deref step; without one it behaves like an address");
    json def = json::object();
    def["kind"] = kKinds[kind_];
    def["steps"] = std::move(steps);
    if (typeHint_ > 0) def["type"] = mem::ValueTypeName(mem::kAllValueTypes[typeHint_ - 1]);
    if (!util::Trim(expectClass_).empty()) def["expect_class"] = util::Trim(expectClass_);
    def["verified"] = verified_;
    if (!util::Trim(notes_).empty()) def["notes"] = notes_;
    if (!errors.empty()) return std::nullopt;
    return def;
  }

  bool DrawStepRow(Step& s, int index, int& moveFrom, int& moveDir, int& removeAt) {
    const float fs = ImGui::GetFontSize();
    bool changed = false;
    ImGui::PushID(index);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::Colors().textDim, "%d.", index + 1);
    ImGui::SameLine(fs * 1.8f);
    ImGui::SetNextItemWidth(fs * 8.0f);
    if (ImGui::BeginCombo("##kind", SpecOf(s.kind).label)) {
      for (const StepSpec& spec : kStepSpecs) {
        if (ImGui::Selectable(spec.label, spec.kind == s.kind)) {
          s.kind = spec.kind;
          changed = true;
        }
        ImGui::SetItemTooltip("%s", spec.help);
      }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", SpecOf(s.kind).help);
    ImGui::SameLine();
    const float paramW = std::max(ImGui::GetContentRegionAvail().x - fs * 6.5f, fs * 8.0f);
    switch (s.kind) {
      case StepKind::Module:
        ImGui::SetNextItemWidth(paramW);
        changed |= tools::InputString("##t", &s.text, 0, "module.dll (empty = game exe)");
        break;
      case StepKind::Pattern: {
        tools::MonoFontScope mono;
        ImGui::SetNextItemWidth(paramW - fs * 10.0f);
        changed |= tools::InputString("##t", &s.text, 0, "IDA pattern");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fs * 4.0f);
        changed |= ImGui::InputInt("##idx", &s.a, 0);
        ImGui::SetItemTooltip("Match index (0 = first)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fs * 5.0f);
        changed |= tools::InputString("##sec", &s.section, 0, "section");
        break;
      }
      case StepKind::String:
        ImGui::SetNextItemWidth(paramW - fs * 5.0f);
        changed |= tools::InputString("##t", &s.text, 0, "string literal");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("wide", &s.wide);
        break;
      case StepKind::Xref:
      case StepKind::Vfunc:
        ImGui::SetNextItemWidth(fs * 6.0f);
        changed |= ImGui::InputInt("##a", &s.a);
        if (s.a < 0) s.a = 0;
        break;
      case StepKind::Rip:
        ImGui::SetNextItemWidth(fs * 5.0f);
        changed |= ImGui::InputInt("disp##a", &s.a, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fs * 5.0f);
        changed |= ImGui::InputInt("len##b", &s.b, 0);
        break;
      case StepKind::Add:
      case StepKind::Sub:
        ImGui::SetNextItemWidth(fs * 8.0f);
        changed |= tools::InputString("##t", &s.text, 0, "0x10 or 16");
        break;
      case StepKind::Symbol:
        ImGui::SetNextItemWidth(paramW);
        changed |= tools::InputString("##t", &s.text, 0, "Other.Symbol");
        break;
      case StepKind::Export:
        ImGui::SetNextItemWidth(paramW);
        changed |= tools::InputString("##t", &s.text, 0, "kernel32.dll!Sleep");
        break;
      case StepKind::RttiVtable:
      case StepKind::RttiInstance:
        ImGui::SetNextItemWidth(paramW);
        changed |= tools::InputString("##t", &s.text, 0, "C_Player2");
        break;
      case StepKind::FunctionStart:
      case StepKind::Call:
      case StepKind::Deref:
        ImGui::TextColored(theme::Colors().textFaint, "(no parameters)");
        break;
    }
    // Right-align the row buttons without overlapping the parameter widgets.
    const float lineRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float prevEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX() + ImGui::GetStyle().ItemSpacing.x;
    const float buttonsW = ImGui::GetFrameHeight() * 3.0f + 4.0f;
    ImGui::SameLine(std::max(lineRight - buttonsW, prevEnd));
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
      moveFrom = index;
      moveDir = -1;
    }
    ImGui::SameLine(0, 2);
    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
      moveFrom = index;
      moveDir = 1;
    }
    ImGui::SameLine(0, 2);
    if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) removeAt = index;
    ImGui::SetItemTooltip("Remove step");
    ImGui::PopID();
    return changed;
  }

  void DrawBindingTab() {
    const auto& c = theme::Colors();
    const float fs = ImGui::GetFontSize();
    const float labelW = fs * 6.0f;

    SectionHeader("Symbol");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(fs * 16.0f);
    tools::InputString("##symname", &name_, 0, "Category.Name");
    const std::string name = util::Trim(name_);
    const std::optional<game::SymbolInfo> existing = ValidSymbolName(name) ? game::Bindings::Get().Info(name) : std::nullopt;
    if (existing) {
      ImGui::SameLine();
      ImGui::TextColored(c.warning, "replaces the existing %s symbol", game::SymbolKindName(existing->kind));
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Kind");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(fs * 8.0f);
    ImGui::Combo("##kind", &kind_, "address\0pointer\0function\0");
    ImGui::SameLine();
    ImGui::TextUnformatted("Type");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 6.0f);
    const char* typeLabel = typeHint_ == 0 ? "(none)" : mem::ValueTypeName(mem::kAllValueTypes[typeHint_ - 1]);
    if (ImGui::BeginCombo("##typehint", typeLabel)) {
      if (ImGui::Selectable("(none)", typeHint_ == 0)) typeHint_ = 0;
      for (int i = 0; i < static_cast<int>(std::size(mem::kAllValueTypes)); ++i) {
        if (ImGui::Selectable(mem::ValueTypeName(mem::kAllValueTypes[i]), typeHint_ == i + 1)) typeHint_ = i + 1;
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Verified", &verified_);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("RTTI check");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(fs * 16.0f);
    tools::InputString("##expect", &expectClass_, 0, "expected class (optional)");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Notes");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(-FLT_MIN);
    tools::InputString("##notes", &notes_, 0, "how it was found, game build");

    SectionHeader("Steps");
    int moveFrom = -1, moveDir = 0, removeAt = -1;
    for (int i = 0; i < static_cast<int>(steps_.size()); ++i) DrawStepRow(steps_[static_cast<size_t>(i)], i, moveFrom, moveDir, removeAt);
    if (removeAt >= 0) steps_.erase(steps_.begin() + removeAt);
    if (moveFrom >= 0) {
      const int to = moveFrom + moveDir;
      if (to >= 0 && to < static_cast<int>(steps_.size())) std::swap(steps_[static_cast<size_t>(moveFrom)], steps_[static_cast<size_t>(to)]);
    }
    if (ImGui::Button("Add step")) ImGui::OpenPopup("##addstep");
    if (ImGui::BeginPopup("##addstep")) {
      for (const StepSpec& spec : kStepSpecs) {
        if (ImGui::MenuItem(spec.label)) {
          Step s{spec.kind};
          if (spec.kind == StepKind::Rip) {
            s.a = 3;
            s.b = 7;
          }
          steps_.push_back(s);
        }
        ImGui::SetItemTooltip("%s", spec.help);
      }
      ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(steps_.empty());
    if (ImGui::Button("Clear steps")) steps_.clear();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(c.textFaint, "Fill from Generate / Test pattern / Strings, or build by hand.");

    std::vector<std::string> errors, warnings;
    const std::optional<json> def = BuildDefinition(errors, warnings);
    if (!ValidSymbolName(name)) errors.insert(errors.begin(), "Name: use letters, digits, '_', '.' or ':'");

    SectionHeader("Preview");
    preview_ = def ? def->dump(2) : std::string();
    {
      tools::MonoFontScope mono;
      const float h = std::clamp(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.5f, fs * 4.0f, fs * 18.0f);
      tools::InputStringMultiline("##preview", &preview_, ImVec2(-FLT_MIN, h), ImGuiInputTextFlags_ReadOnly);
    }
    for (const std::string& e : errors) ImGui::TextColored(c.danger, "%s", e.c_str());
    for (const std::string& w : warnings) ImGui::TextColored(c.warning, "%s", w.c_str());

    ImGui::BeginDisabled(!errors.empty() || defineJob_.Running());
    if (ImGui::Button("Define binding")) {
      const json definition = *def;
      defineError_.clear();
      defineDone_ = false;
      defineJob_.Start([name, definition](DefineJob::State& st) {
        st.result.name = name;
        std::string err;
        st.result.ok = game::Bindings::Get().Define(name, definition, &err);
        if (!st.result.ok) st.error = err.empty() ? "Define failed" : err;
      });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!def);
    if (ImGui::Button("Copy JSON") && def) {
      json wrapped = json::object();
      wrapped[name.empty() ? "Unnamed" : name] = *def;
      tools::CopyToClipboard(wrapped.dump(2), "Binding JSON copied");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (defineJob_.Running()) {
      ImGui::TextColored(c.textDim, "Resolving...");
    } else if (!defineError_.empty()) {
      ImGui::TextColored(c.danger, "%s", defineError_.c_str());
    } else if (defineDone_ && !definedName_.empty()) {
      DrawDefinedStatus(definedName_);
    }
  }

  void DrawDefinedStatus(const std::string& symbol) {
    const auto& c = theme::Colors();
    const std::optional<game::SymbolInfo> info = game::Bindings::Get().Info(symbol);
    if (!info) {
      ImGui::TextColored(c.warning, "%s is not listed by the bindings yet", symbol.c_str());
      return;
    }
    const bool good = info->status == game::SymbolStatus::Resolved || info->status == game::SymbolStatus::Overridden;
    ImGui::TextColored(good ? c.success : c.danger, "%s: %s", symbol.c_str(), game::SymbolStatusName(info->status));
    if (!good) {
      if (!info->error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(c.danger, "- %s", info->error.c_str());
      }
      return;
    }
    if (const std::optional<uintptr_t> a = game::Bindings::Get().Addr(symbol)) {
      ImGui::SameLine();
      if (ImGui::TextLink(mem::Describe(*a).c_str())) GotoHexView(*a);
      tools::AddressContextMenu("##defctx", *a);
    }
  }

  int selectTab_ = -1;

  ModulesJob modulesJob_;
  std::vector<mem::Module> modules_;
  bool modulesRequested_ = false;
  std::string moduleFilter_;

  // Generate
  std::string genExpr_;
  int genMaxBytes_ = 96;
  GenJob genJob_;
  std::string genSig_;

  // Test
  std::string testPattern_, testModule_;
  bool testExecOnly_ = true;
  int ripMode_ = 0, ripOffset_ = 0, ripDisp_ = 3, ripLen_ = 7;
  TestJob testJob_;

  // Strings
  std::string strText_, strModule_;
  bool strWide_ = false;
  std::string strSearchedText_, strSearchedModule_;
  bool strSearchedWide_ = false;
  StringJob strJob_;

  // Row signatures
  SigJob sigJob_;
  uintptr_t sigFor_ = 0;

  // Binding builder
  std::string name_, expectClass_, notes_, preview_;
  int kind_ = 0, typeHint_ = 0;
  bool verified_ = false;
  std::vector<Step> steps_;
  DefineJob defineJob_;
  std::string defineError_, definedName_;
  bool defineDone_ = false;
};

}  // namespace

std::unique_ptr<Window> MakeSignatureWindow() { return std::make_unique<SignatureWindow>(); }

}  // namespace cg::ui
