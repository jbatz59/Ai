// Loaded-module browser: module list, PE sections and the export directory. Exports are parsed
// here from the in-memory image with bounds-checked ReadRaw calls, so a module with a damaged or
// hostile header shows an error instead of crashing the game.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

#include <imgui.h>

#include "core/util.h"
#include "mem/module.h"
#include "mem/safe.h"
#include "render/fonts.h"
#include "render/theme.h"
#include "ui/notify.h"
#include "ui/tools/tools_common.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace cg::ui {
namespace {

namespace theme = render::theme;

constexpr uint32_t kMaxExports = 1u << 20;   // far above any real DLL; guards corrupt headers
constexpr size_t kMaxNameLen = 512;

struct ExportEntry {
  uint32_t ordinal = 0;
  uint32_t rva = 0;
  std::string name;        // "" when exported by ordinal only
  std::string forwarder;   // "OTHER.Function" for forwarded exports
};

struct ExportResult {
  uintptr_t base = 0;
  std::vector<ExportEntry> exports;
};

bool ReadImage(uintptr_t base, size_t imageSize, uint64_t rva, void* out, size_t bytes) {
  if (bytes == 0) return true;
  if (rva > imageSize || bytes > imageSize - rva) return false;
  return mem::ReadRaw(base + static_cast<uintptr_t>(rva), out, bytes);
}

template <class T>
bool ReadImage(uintptr_t base, size_t imageSize, uint64_t rva, T& out) {
  return ReadImage(base, imageSize, rva, &out, sizeof(T));
}

std::string ReadImageString(uintptr_t base, size_t imageSize, uint64_t rva) {
  if (rva >= imageSize) return {};
  return mem::ReadCString(base + static_cast<uintptr_t>(rva), std::min<size_t>(kMaxNameLen, imageSize - static_cast<size_t>(rva)));
}

// Parses the export directory of the image mapped at `base`. Returns false with `err` on a
// malformed header; a module without exports yields true and an empty list.
bool ParseExports(uintptr_t base, size_t imageSize, std::vector<ExportEntry>& out, std::string& err, const std::atomic<bool>& cancel) {
  IMAGE_DOS_HEADER dos{};
  if (!ReadImage(base, imageSize, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
    err = "no MZ header";
    return false;
  }
  if (dos.e_lfanew <= 0) {
    err = "invalid e_lfanew";
    return false;
  }
  const uint64_t nt = static_cast<uint64_t>(dos.e_lfanew);
  DWORD signature = 0;
  if (!ReadImage(base, imageSize, nt, signature) || signature != IMAGE_NT_SIGNATURE) {
    err = "no PE signature";
    return false;
  }
  const uint64_t optRva = nt + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
  WORD magic = 0;
  if (!ReadImage(base, imageSize, optRva, magic)) {
    err = "unreadable optional header";
    return false;
  }
  IMAGE_DATA_DIRECTORY dir{};
  if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    IMAGE_OPTIONAL_HEADER64 oh{};
    if (!ReadImage(base, imageSize, optRva, oh)) {
      err = "unreadable optional header";
      return false;
    }
    if (oh.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return true;
    dir = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
    IMAGE_OPTIONAL_HEADER32 oh{};
    if (!ReadImage(base, imageSize, optRva, oh)) {
      err = "unreadable optional header";
      return false;
    }
    if (oh.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return true;
    dir = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  } else {
    err = "unknown optional header magic " + util::Hex(magic);
    return false;
  }
  if (dir.VirtualAddress == 0 || dir.Size == 0) return true;

  IMAGE_EXPORT_DIRECTORY ed{};
  if (!ReadImage(base, imageSize, dir.VirtualAddress, ed)) {
    err = "unreadable export directory";
    return false;
  }
  const uint32_t nFuncs = ed.NumberOfFunctions;
  const uint32_t nNames = ed.NumberOfNames;
  if (nFuncs > kMaxExports || nNames > kMaxExports) {
    err = "implausible export count";
    return false;
  }
  std::vector<uint32_t> funcs(nFuncs), names(nNames);
  std::vector<uint16_t> ordinals(nNames);
  if (!ReadImage(base, imageSize, ed.AddressOfFunctions, funcs.data(), funcs.size() * sizeof(uint32_t)) ||
      !ReadImage(base, imageSize, ed.AddressOfNames, names.data(), names.size() * sizeof(uint32_t)) ||
      !ReadImage(base, imageSize, ed.AddressOfNameOrdinals, ordinals.data(), ordinals.size() * sizeof(uint16_t))) {
    err = "export tables are outside the image or unreadable";
    return false;
  }

  const uint64_t fwdBegin = dir.VirtualAddress;
  const uint64_t fwdEnd = fwdBegin + dir.Size;
  auto makeEntry = [&](uint32_t index) {
    ExportEntry e;
    e.ordinal = ed.Base + index;
    e.rva = funcs[index];
    if (e.rva >= fwdBegin && e.rva < fwdEnd) e.forwarder = ReadImageString(base, imageSize, e.rva);
    return e;
  };

  std::vector<uint8_t> named(nFuncs, 0);
  out.reserve(nFuncs);
  for (uint32_t j = 0; j < nNames; ++j) {
    if ((j & 255) == 0 && cancel.load(std::memory_order_relaxed)) return true;
    const uint32_t index = ordinals[j];
    if (index >= nFuncs) continue;
    ExportEntry e = makeEntry(index);
    e.name = ReadImageString(base, imageSize, names[j]);
    named[index] = 1;
    out.push_back(std::move(e));
  }
  for (uint32_t i = 0; i < nFuncs; ++i) {
    if (!named[i] && funcs[i] != 0) out.push_back(makeEntry(i));
  }
  return true;
}

std::string SectionFlags(uint32_t ch) {
  std::string s;
  s += (ch & IMAGE_SCN_MEM_READ) ? 'R' : '-';
  s += (ch & IMAGE_SCN_MEM_WRITE) ? 'W' : '-';
  s += (ch & IMAGE_SCN_MEM_EXECUTE) ? 'X' : '-';
  if (ch & IMAGE_SCN_CNT_CODE) s += " code";
  if (ch & IMAGE_SCN_CNT_INITIALIZED_DATA) s += " data";
  if (ch & IMAGE_SCN_CNT_UNINITIALIZED_DATA) s += " bss";
  return s;
}

class ModulesWindow final : public Window {
 public:
  const char* Id() const override { return tools::kModulesId; }
  const char* Title() const override { return "Modules"; }
  const char* Icon() const override { return CG_ICON_INFO; }
  const char* Group() const override { return tools::kReGroup; }
  float DefaultWidth() const override { return 860; }
  float DefaultHeight() const override { return 600; }

  void Draw() override {
    PollJobs();
    DrawToolbar();
    const float listH = std::max(ImGui::GetContentRegionAvail().y * 0.42f, ImGui::GetFrameHeightWithSpacing() * 4.0f);
    if (ImGui::BeginChild("##modlist", ImVec2(0, listH), ImGuiChildFlags_ResizeY)) DrawModuleTable();
    ImGui::EndChild();
    DrawDetails();
  }

 private:
  using ModulesJob = tools::AsyncJob<std::vector<mem::Module>>;
  using ExportsJob = tools::AsyncJob<ExportResult>;

  enum ModCol { kModName, kModBase, kModSize, kModPath };
  enum ExpCol { kExpOrdinal, kExpName, kExpAddress, kExpForwarder };

  void Refresh() {
    modulesJob_.Start([](ModulesJob::State& st) { st.result = mem::Module::All(); });
  }

  void PollJobs() {
    if (!requested_) {
      requested_ = true;
      Refresh();
    }
    if (modulesJob_.Ready()) {
      ModulesJob::State& st = modulesJob_.Get();
      if (st.error.empty()) {
        modules_ = std::move(st.result);
        modulesSorted_ = false;
        if (selectedBase_ && !Selected()) {
          selectedBase_ = 0;
          exports_.clear();
          exportsFor_ = 0;
        }
      } else {
        notify::Push(notify::Kind::Error, "Module list", st.error);
      }
      modulesJob_.Cancel();
    }
    if (exportsJob_.Ready()) {
      ExportsJob::State& st = exportsJob_.Get();
      exportsError_ = st.error;
      exports_ = std::move(st.result.exports);
      exportsFor_ = st.result.base;
      exportsSorted_ = false;
      exportViewDirty_ = true;
      exportsJob_.Cancel();
    }
  }

  const mem::Module* Selected() const {
    for (const mem::Module& m : modules_) {
      if (m.base == selectedBase_) return &m;
    }
    return nullptr;
  }

  void Select(const mem::Module& m) {
    if (selectedBase_ == m.base) return;
    selectedBase_ = m.base;
    exports_.clear();
    exportView_.clear();
    exportsError_.clear();
    exportsFor_ = 0;
    const uintptr_t base = m.base;
    const size_t size = m.size;
    exportsJob_.Start([base, size](ExportsJob::State& st) {
      st.result.base = base;
      std::string err;
      if (!ParseExports(base, size, st.result.exports, err, st.cancel)) st.error = err;
    });
  }

  void DrawToolbar() {
    const auto& c = theme::Colors();
    ImGui::BeginDisabled(modulesJob_.Running());
    if (ImGui::Button("Refresh")) Refresh();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    SearchBox("##modfilter", filter_, sizeof filter_, "Filter modules...");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (modulesJob_.Running() && modules_.empty()) ImGui::TextColored(c.textDim, "Enumerating...");
    else ImGui::TextColored(c.textDim, "%s modules", tools::FormatCount(modules_.size()).c_str());
  }

  void SortModules(const ImGuiTableColumnSortSpecs& spec) {
    const bool asc = spec.SortDirection != ImGuiSortDirection_Descending;
    const int col = spec.ColumnIndex;
    std::stable_sort(modules_.begin(), modules_.end(), [asc, col](const mem::Module& a, const mem::Module& b) {
      bool less = false, greater = false;
      switch (col) {
        case kModBase: less = a.base < b.base; greater = b.base < a.base; break;
        case kModSize: less = a.size < b.size; greater = b.size < a.size; break;
        case kModPath: less = a.path < b.path; greater = b.path < a.path; break;
        default: less = a.name < b.name; greater = b.name < a.name; break;
      }
      return asc ? less : greater;
    });
  }

  void DrawModuleTable() {
    const auto& c = theme::Colors();
    const float fs = ImGui::GetFontSize();
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##modules", 4, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 1.3f);
    ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthFixed, fs * 8.5f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, fs * 6.0f);
    ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 2.5f);
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
      if ((specs->SpecsDirty || !modulesSorted_) && specs->SpecsCount > 0) {
        SortModules(specs->Specs[0]);
        specs->SpecsDirty = false;
        modulesSorted_ = true;
      }
    }

    view_.clear();
    const std::string_view filter = filter_;
    for (size_t i = 0; i < modules_.size(); ++i) {
      const mem::Module& m = modules_[i];
      if (filter.empty() || util::IContains(m.name, filter) || util::IContains(util::Narrow(m.path), filter)) view_.push_back(i);
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(view_.size()));
    while (clipper.Step()) {
      for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
        const mem::Module& m = modules_[view_[static_cast<size_t>(r)]];
        ImGui::PushID(r);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(kModName);
        const bool isMain = m.base == mem::Module::Main().base;
        if (isMain) ImGui::PushStyleColor(ImGuiCol_Text, c.accent);
        if (ImGui::Selectable(m.name.c_str(), m.base == selectedBase_, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) Select(m);
        if (isMain) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) GotoHexView(m.base);
        if (ImGui::BeginPopupContextItem("##modctx")) {
          tools::AddressMenuItems(m.base);
          ImGui::Separator();
          if (ImGui::MenuItem("Copy name")) tools::CopyToClipboard(m.name, "Name copied");
          if (ImGui::MenuItem("Copy path")) tools::CopyToClipboard(util::Narrow(m.path), "Path copied");
          ImGui::EndPopup();
        }
        ImGui::TableSetColumnIndex(kModBase);
        {
          tools::MonoFontScope mono;
          ImGui::TextUnformatted(tools::FormatAddress(m.base).c_str());
        }
        ImGui::TableSetColumnIndex(kModSize);
        ImGui::TextUnformatted(util::FormatBytesSize(m.size).c_str());
        ImGui::TableSetColumnIndex(kModPath);
        const std::string path = util::Narrow(m.path);
        ImGui::TextColored(c.textDim, "%s", path.c_str());
        ImGui::PopID();
      }
    }
    ImGui::EndTable();
  }

  void DrawDetails() {
    const auto& c = theme::Colors();
    const mem::Module* m = Selected();
    if (!m) {
      ImGui::TextColored(c.textFaint, "Select a module to see its sections and exports.");
      return;
    }
    ImGui::TextUnformatted(m->name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(c.textDim, "%s - %s (%s)", tools::FormatAddress(m->base).c_str(), tools::FormatAddress(m->base + m->size).c_str(),
                       util::FormatBytesSize(m->size).c_str());
    if (!ImGui::BeginTabBar("##modtabs")) return;
    if (ImGui::BeginTabItem("Sections")) {
      DrawSections(*m);
      ImGui::EndTabItem();
    }
    char exportsLabel[48];
    if (exportsFor_ == m->base) std::snprintf(exportsLabel, sizeof exportsLabel, "Exports (%zu)###exports", exports_.size());
    else std::snprintf(exportsLabel, sizeof exportsLabel, "Exports###exports");
    if (ImGui::BeginTabItem(exportsLabel)) {
      DrawExports(*m);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  void DrawSections(const mem::Module& m) {
    const auto& c = theme::Colors();
    const float fs = ImGui::GetFontSize();
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##sections", 5, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, fs * 6.0f);
    ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("End", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, fs * 6.0f);
    ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < m.sections.size(); ++i) {
      const mem::Section& s = m.sections[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      if (ImGui::Selectable(s.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
          ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (s.Executable()) tools::GotoDisassembly(s.start);
        else GotoHexView(s.start);
      }
      if (ImGui::BeginPopupContextItem("##secctx")) {
        tools::AddressMenuItems(s.start);
        ImGui::Separator();
        if (ImGui::MenuItem("Copy range")) tools::CopyToClipboard(tools::FormatAddress(s.start) + "-" + tools::FormatAddress(s.start + s.size), "Range copied");
        ImGui::EndPopup();
      }
      tools::MonoFontScope mono;
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(tools::FormatAddress(s.start).c_str());
      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(tools::FormatAddress(s.start + s.size).c_str());
      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(util::FormatBytesSize(s.size).c_str());
      ImGui::TableSetColumnIndex(4);
      ImGui::TextColored(s.Executable() ? c.warning : (s.Writable() ? c.info : c.textDim), "%s", SectionFlags(s.characteristics).c_str());
      ImGui::PopID();
    }
    ImGui::EndTable();
    if (m.sections.empty()) ImGui::TextColored(c.textFaint, "No section headers.");
  }

  void SortExports(const ImGuiTableColumnSortSpecs& spec) {
    const bool asc = spec.SortDirection != ImGuiSortDirection_Descending;
    const int col = spec.ColumnIndex;
    std::stable_sort(exports_.begin(), exports_.end(), [asc, col](const ExportEntry& a, const ExportEntry& b) {
      bool less = false, greater = false;
      switch (col) {
        case kExpName: {
          // Named exports first, case-insensitive.
          if (a.name.empty() != b.name.empty()) return a.name.empty() < b.name.empty();
          const int cmp = _stricmp(a.name.c_str(), b.name.c_str());
          less = cmp < 0;
          greater = cmp > 0;
          break;
        }
        case kExpAddress: less = a.rva < b.rva; greater = b.rva < a.rva; break;
        case kExpForwarder: less = a.forwarder < b.forwarder; greater = b.forwarder < a.forwarder; break;
        default: less = a.ordinal < b.ordinal; greater = b.ordinal < a.ordinal; break;
      }
      return asc ? less : greater;
    });
    exportViewDirty_ = true;
  }

  void RebuildExportView() {
    exportView_.clear();
    const std::string_view filter = exportFilter_;
    for (size_t i = 0; i < exports_.size(); ++i) {
      const ExportEntry& e = exports_[i];
      if (filter.empty() || util::IContains(e.name, filter) || util::IContains(e.forwarder, filter)) exportView_.push_back(i);
    }
    exportViewFilter_ = exportFilter_;
    exportViewDirty_ = false;
  }

  void DrawExports(const mem::Module& m) {
    const auto& c = theme::Colors();
    const float fs = ImGui::GetFontSize();
    if (exportsJob_.Running() || exportsFor_ != m.base) {
      ImGui::TextColored(c.textDim, "Parsing export directory...");
      return;
    }
    if (!exportsError_.empty()) {
      ImGui::TextColored(c.danger, "Export directory: %s", exportsError_.c_str());
      return;
    }
    if (exports_.empty()) {
      ImGui::TextColored(c.textFaint, "%s exports nothing.", m.name.c_str());
      return;
    }
    ImGui::SetNextItemWidth(fs * 14.0f);
    SearchBox("##expfilter", exportFilter_, sizeof exportFilter_, "Filter exports...");
    if (exportViewDirty_ || exportViewFilter_ != exportFilter_) RebuildExportView();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(c.textDim, "%s shown", tools::FormatCount(exportView_.size()).c_str());

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##exports", 4, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Ordinal", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, fs * 4.5f);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("Forwarded to", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
      if ((specs->SpecsDirty || !exportsSorted_) && specs->SpecsCount > 0) {
        SortExports(specs->Specs[0]);
        specs->SpecsDirty = false;
        exportsSorted_ = true;
      }
    }
    if (exportViewDirty_) RebuildExportView();

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(exportView_.size()));
    while (clipper.Step()) {
      for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
        const ExportEntry& e = exports_[exportView_[static_cast<size_t>(r)]];
        const uintptr_t addr = m.base + e.rva;
        ImGui::PushID(r);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(kExpOrdinal);
        char ord[16];
        std::snprintf(ord, sizeof ord, "%u", e.ordinal);
        if (ImGui::Selectable(ord, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          if (e.forwarder.empty() && mem::IsExecutable(addr)) tools::GotoDisassembly(addr);
          else GotoHexView(addr);
        }
        if (ImGui::BeginPopupContextItem("##expctx")) {
          tools::AddressMenuItems(addr);
          ImGui::Separator();
          if (ImGui::MenuItem("Copy name", nullptr, false, !e.name.empty())) tools::CopyToClipboard(e.name, "Name copied");
          if (ImGui::MenuItem("Copy module!name", nullptr, false, !e.name.empty())) tools::CopyToClipboard(m.name + "!" + e.name, "Export copied");
          ImGui::EndPopup();
        }
        ImGui::TableSetColumnIndex(kExpName);
        if (e.name.empty()) ImGui::TextColored(c.textFaint, "(by ordinal)");
        else ImGui::TextUnformatted(e.name.c_str());
        ImGui::TableSetColumnIndex(kExpAddress);
        {
          tools::MonoFontScope mono;
          ImGui::TextUnformatted(tools::FormatAddress(addr).c_str());
        }
        ImGui::SetItemTooltip("RVA %s", util::Hex(e.rva).c_str());
        ImGui::TableSetColumnIndex(kExpForwarder);
        if (!e.forwarder.empty()) ImGui::TextColored(c.info, "%s", e.forwarder.c_str());
        ImGui::PopID();
      }
    }
    ImGui::EndTable();
  }

  ModulesJob modulesJob_;
  std::vector<mem::Module> modules_;
  std::vector<size_t> view_;
  bool requested_ = false, modulesSorted_ = false;
  char filter_[128] = {};
  uintptr_t selectedBase_ = 0;

  ExportsJob exportsJob_;
  std::vector<ExportEntry> exports_;
  std::vector<size_t> exportView_;
  std::string exportsError_, exportViewFilter_;
  uintptr_t exportsFor_ = 0;
  bool exportsSorted_ = false, exportViewDirty_ = true;
  char exportFilter_[128] = {};
};

}  // namespace

std::unique_ptr<Window> MakeModulesWindow() { return std::make_unique<ModulesWindow>(); }

}  // namespace cg::ui
