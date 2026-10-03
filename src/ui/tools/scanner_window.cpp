// Memory scanner: Cheat-Engine-style first/next value scans over the process' committed memory
// (mem::Scanner runs on worker threads; this window only polls it and pages through results).
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "core/config.h"
#include "core/util.h"
#include "mem/module.h"
#include "mem/scanner.h"
#include "mem/value.h"
#include "render/fonts.h"
#include "render/theme.h"
#include "ui/notify.h"
#include "ui/tools/tools_common.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace cg::ui {
namespace {

namespace theme = render::theme;
using mem::ScanOp;

constexpr ScanOp kAllOps[] = {ScanOp::Exact,     ScanOp::NotEqual,  ScanOp::Bigger,    ScanOp::Smaller,
                              ScanOp::Between,   ScanOp::Unknown,   ScanOp::Changed,   ScanOp::Unchanged,
                              ScanOp::Increased, ScanOp::Decreased, ScanOp::IncreasedBy, ScanOp::DecreasedBy};

// ImGui positions are floats: past a few hundred thousand rows the scroll math loses precision,
// and nobody inspects more than that by hand anyway.
constexpr size_t kMaxDisplayRows = 500000;
constexpr uint64_t kPageRefreshMs = 100;
constexpr size_t kPageMargin = 16;
constexpr size_t kDescribeCacheMax = 4096;
constexpr size_t kMaxBulkAdd = 1000;
constexpr const char* kDefaultRangeStart = "10000";
constexpr const char* kDefaultRangeEnd = "7FFFFFFF0000";

bool IsIntegral(mem::ValueType t) { return mem::IsNumeric(t) && !mem::IsFloat(t); }

bool OpAvailable(ScanOp op, mem::ValueType t, bool next) {
  if (!mem::IsNumeric(t) && op != ScanOp::Exact && op != ScanOp::Changed && op != ScanOp::Unchanged) return false;
  return next ? op != ScanOp::Unknown : mem::ScanOpValidForFirst(op);
}

class ScannerWindow final : public Window {
 public:
  ScannerWindow() { LoadSettings(); }
  ~ScannerWindow() override { scanner_.Cancel(); }

  const char* Id() const override { return tools::kScannerId; }
  const char* Title() const override { return "Memory Scanner"; }
  const char* Icon() const override { return CG_ICON_SEARCH; }
  const char* Group() const override { return tools::kReGroup; }
  float DefaultWidth() const override { return 860; }
  float DefaultHeight() const override { return 560; }

  void Draw() override {
    const bool busy = scanner_.Busy();
    const bool hasResults = !busy && scanner_.HasResults();
    if (hasResults) type_ = scanner_.Type();
    // While a scan runs the first/next mode is unknown, so the selection is left alone.
    if (!busy && !OpAvailable(op_, type_, hasResults)) op_ = FirstAvailableOp(hasResults);

    const float panelW = ImGui::GetFontSize() * 19.0f;
    const float resultsW = std::max(ImGui::GetContentRegionAvail().x - panelW - ImGui::GetStyle().ItemSpacing.x, ImGui::GetFontSize() * 16.0f);
    if (ImGui::BeginChild("##results", ImVec2(resultsW, 0), ImGuiChildFlags_None)) DrawResultsPane(busy, hasResults);
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##controls", ImVec2(0, 0), ImGuiChildFlags_None)) DrawControls(busy, hasResults);
    ImGui::EndChild();
  }

 private:
  // ------------------------------------------------------------------------------------------
  // Settings
  // ------------------------------------------------------------------------------------------
  void LoadSettings() {
    const Config& c = Config::Get();
    type_ = mem::ParseValueType(c.ReadString("tools.scanner.type", "i32")).value_or(mem::ValueType::I32);
    writableOnly_ = c.ReadBool("tools.scanner.writableOnly", true);
    includeImages_ = c.ReadBool("tools.scanner.includeImages", false);
    fastScan_ = c.ReadBool("tools.scanner.fastScan", true);
    floatRounded_ = c.ReadBool("tools.scanner.floatRounded", true);
    hex_ = c.ReadBool("tools.scanner.hex", false);
    snapshotGiB_ = static_cast<int>(std::clamp<int64_t>(c.ReadInt("tools.scanner.snapshotGiB", 2), 1, 16));
  }
  void SaveSettings() const {
    Config& c = Config::Get();
    c.WriteString("tools.scanner.type", mem::ValueTypeName(type_));
    c.WriteBool("tools.scanner.writableOnly", writableOnly_);
    c.WriteBool("tools.scanner.includeImages", includeImages_);
    c.WriteBool("tools.scanner.fastScan", fastScan_);
    c.WriteBool("tools.scanner.floatRounded", floatRounded_);
    c.WriteBool("tools.scanner.hex", hex_);
    c.WriteInt("tools.scanner.snapshotGiB", snapshotGiB_);
  }

  ScanOp FirstAvailableOp(bool next) const {
    for (ScanOp op : kAllOps) {
      if (OpAvailable(op, type_, next)) return op;
    }
    return ScanOp::Exact;
  }

  // ------------------------------------------------------------------------------------------
  // Scanning
  // ------------------------------------------------------------------------------------------
  std::string PrepareValue(const std::string& raw) const {
    if (type_ == mem::ValueType::String || type_ == mem::ValueType::WString) return raw;
    std::string v = util::Trim(raw);
    if (hex_ && IsIntegral(type_) && !v.empty() && v[0] != '-' && !util::StartsWith(util::ToLower(v), "0x")) v = "0x" + v;
    return v;
  }

  bool BuildParams(mem::ScanParams& p) {
    p.type = type_;
    p.op = op_;
    p.value = PrepareValue(value_);
    p.value2 = PrepareValue(value2_);
    p.writableOnly = writableOnly_;
    p.includeImages = includeImages_;
    p.fastScan = fastScan_;
    p.floatRounded = floatRounded_;
    if (mem::ScanOpNeedsValue(op_) && p.value.empty()) {
      localError_ = "Enter a value to scan for";
      return false;
    }
    if (op_ == ScanOp::Between && p.value2.empty()) {
      localError_ = "Enter the upper bound of the range";
      return false;
    }
    const mem::ExprResult rs = tools::Eval(rangeStartExpr_);
    if (!rs.ok) {
      localError_ = "Range start: " + rs.error;
      return false;
    }
    const mem::ExprResult re = tools::Eval(rangeEndExpr_);
    if (!re.ok) {
      localError_ = "Range end: " + re.error;
      return false;
    }
    if (re.value <= rs.value) {
      localError_ = "The range end must be above the range start";
      return false;
    }
    p.rangeStart = rs.value;
    p.rangeEnd = re.value;
    return true;
  }

  void StartScan(bool next) {
    localError_.clear();
    mem::ScanParams p;
    if (!BuildParams(p)) return;
    scanner_.SetSnapshotLimit(static_cast<uint64_t>(snapshotGiB_) << 30);
    const bool ok = next ? scanner_.NextScan(p) : scanner_.FirstScan(p);
    if (!ok) {
      localError_ = scanner_.LastError();
      if (localError_.empty()) localError_ = "The scanner is busy";
      return;
    }
    ClearSelection();
    InvalidatePage();
  }

  void NewScan() {
    scanner_.Reset();
    localError_.clear();
    ClearSelection();
    InvalidatePage();
    describeCache_.clear();
  }

  // ------------------------------------------------------------------------------------------
  // Result paging
  // ------------------------------------------------------------------------------------------
  void InvalidatePage() {
    page_.clear();
    pageGen_ = UINT64_MAX;
  }

  // Makes the cache cover [start, end). Fetches only the visible slice (plus a small margin) and
  // reuses it for kPageRefreshMs so live values update without re-reading every frame.
  void EnsurePage(size_t start, size_t end) {
    const uint64_t gen = scanner_.Generation();
    const uint64_t now = util::NowMs();
    if (gen != describeGen_) {
      describeCache_.clear();
      describeGen_ = gen;
    }
    const bool covers = start >= pageOffset_ && end <= pageOffset_ + page_.size();
    if (covers && gen == pageGen_ && hex_ == pageHex_ && now - pageTimeMs_ < kPageRefreshMs) return;
    const size_t from = start > kPageMargin ? start - kPageMargin : 0;
    const size_t count = (end - from) + kPageMargin;
    page_ = scanner_.Page(from, count, hex_);
    pageOffset_ = from;
    pageGen_ = gen;
    pageHex_ = hex_;
    pageTimeMs_ = now;
  }

  const mem::Scanner::Row* RowAt(size_t i) const {
    if (i < pageOffset_ || i - pageOffset_ >= page_.size()) return nullptr;
    return &page_[i - pageOffset_];
  }

  const std::string& DescribeCached(uintptr_t a) {
    auto it = describeCache_.find(a);
    if (it != describeCache_.end()) return it->second;
    if (describeCache_.size() >= kDescribeCacheMax) describeCache_.clear();
    return describeCache_.emplace(a, mem::Describe(a)).first->second;
  }

  // ------------------------------------------------------------------------------------------
  // Selection (index range in the result list)
  // ------------------------------------------------------------------------------------------
  void ClearSelection() {
    selAnchor_ = SIZE_MAX;
    selCursor_ = SIZE_MAX;
  }
  bool HasSelection() const { return selAnchor_ != SIZE_MAX; }
  size_t SelLo() const { return std::min(selAnchor_, selCursor_); }
  size_t SelHi() const { return std::max(selAnchor_, selCursor_); }
  bool IsSelected(size_t i) const { return HasSelection() && i >= SelLo() && i <= SelHi(); }
  size_t SelectionCount() const { return HasSelection() ? SelHi() - SelLo() + 1 : 0; }

  void OnRowClicked(size_t i) {
    if (ImGui::GetIO().KeyShift && HasSelection()) {
      selCursor_ = i;
    } else {
      selAnchor_ = i;
      selCursor_ = i;
    }
  }

  std::vector<mem::Scanner::Row> SelectedRows() const {
    if (!HasSelection()) return {};
    return scanner_.Page(SelLo(), std::min(SelectionCount(), kMaxBulkAdd), false);
  }

  void AddSelectedToTable() {
    const size_t n = SelectionCount();
    const std::vector<mem::Scanner::Row> rows = SelectedRows();
    const char* type = mem::ValueTypeName(scanner_.Type());
    for (const auto& r : rows) AddToAddressTable(r.address, type, "");
    if (n > kMaxBulkAdd) {
      notify::Push(notify::Kind::Warning, "Address table", "Added the first " + tools::FormatCount(kMaxBulkAdd) + " of " + tools::FormatCount(n) + " selected results");
    }
  }

  void CopySelectedAddresses() {
    std::string text;
    for (const auto& r : SelectedRows()) {
      text += tools::FormatAddress(r.address);
      text += '\n';
    }
    if (!text.empty()) tools::CopyToClipboard(text, "Addresses copied");
  }

  // ------------------------------------------------------------------------------------------
  // Drawing
  // ------------------------------------------------------------------------------------------
  void DrawResultsPane(bool busy, bool hasResults) {
    const auto& c = theme::Colors();
    const size_t total = hasResults ? scanner_.ResultCount() : 0;
    ImGui::AlignTextToFramePadding();
    if (busy) {
      ImGui::TextColored(c.textDim, "Scanning...");
    } else if (hasResults) {
      ImGui::TextColored(c.accent, "Found: %s", tools::FormatCount(total).c_str());
      if (HasSelection()) {
        ImGui::SameLine();
        ImGui::TextColored(c.textDim, "(%s selected)", tools::FormatCount(SelectionCount()).c_str());
      }
    } else {
      ImGui::TextColored(c.textFaint, "No scan yet");
    }

    const std::string scannerError = scanner_.LastError();
    if (!localError_.empty()) ImGui::TextColored(c.danger, "%s", localError_.c_str());
    else if (!scannerError.empty()) ImGui::TextColored(c.danger, "%s", scannerError.c_str());

    if (busy) {
      const float p = std::clamp(scanner_.Progress(), 0.0f, 1.0f);
      char overlay[32];
      std::snprintf(overlay, sizeof overlay, "%.0f%%", p * 100.0f);
      ImGui::ProgressBar(p, ImVec2(-FLT_MIN, 0), overlay);
      return;
    }
    if (!hasResults) {
      ImGui::Spacing();
      ImGui::TextColored(c.textFaint, "Pick a value type and scan type, enter a value and press First Scan.");
      ImGui::TextColored(c.textFaint, "Unknown initial value snapshots memory; refine with Changed/Unchanged.");
      return;
    }

    const size_t shown = std::min(total, kMaxDisplayRows);
    if (total > shown) {
      ImGui::TextColored(c.warning, "Showing the first %s of %s results. Narrow them down with Next Scan.",
                         tools::FormatCount(shown).c_str(), tools::FormatCount(total).c_str());
    }

    const float footer = ImGui::GetFrameHeightWithSpacing();
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##scanresults", 3, flags, ImVec2(0, -footer))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.7f);
      ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("Previous", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableHeadersRow();

      const char* typeName = mem::ValueTypeName(scanner_.Type());
      const int bpSize = static_cast<int>(std::clamp<size_t>(mem::ValueSize(scanner_.Type()), 1, 8));
      tools::MonoFontScope mono;
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(shown));
      while (clipper.Step()) {
        EnsurePage(static_cast<size_t>(clipper.DisplayStart), static_cast<size_t>(clipper.DisplayEnd));
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
          const size_t idx = static_cast<size_t>(i);
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          const mem::Scanner::Row* row = RowAt(idx);
          if (!row) {
            ImGui::TextColored(c.textFaint, "...");
            continue;
          }
          const uintptr_t addr = row->address;
          ImGui::PushID(i);
          if (ImGui::Selectable(DescribeCached(addr).c_str(), IsSelected(idx),
                                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
            OnRowClicked(idx);
          }
          if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) AddToAddressTable(addr, typeName, "");
          if (ImGui::BeginPopupContextItem("##ctx")) {
            if (!IsSelected(idx)) OnRowClicked(idx);
            tools::AddressMenuOptions o;
            o.valueType = typeName;
            o.size = bpSize;
            tools::AddressMenuItems(addr, o);
            ImGui::Separator();
            char label[64];
            std::snprintf(label, sizeof label, "Add %s selected to address table", tools::FormatCount(SelectionCount()).c_str());
            if (ImGui::MenuItem(label, nullptr, false, SelectionCount() > 1)) AddSelectedToTable();
            if (ImGui::MenuItem("Copy selected addresses", "Ctrl+C")) CopySelectedAddresses();
            ImGui::EndPopup();
          }
          ImGui::TableSetColumnIndex(1);
          if (row->current != row->previous) ImGui::TextColored(c.warning, "%s", row->current.c_str());
          else ImGui::TextUnformatted(row->current.c_str());
          ImGui::TableSetColumnIndex(2);
          ImGui::TextColored(c.textDim, "%s", row->previous.c_str());
          ImGui::PopID();
        }
      }
      ImGui::EndTable();
    }

    if (HasSelection() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) {
      CopySelectedAddresses();
    }

    ImGui::BeginDisabled(!HasSelection());
    if (ImGui::Button("Add selected to address table")) AddSelectedToTable();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(c.textFaint, "Double-click adds one; Shift+click selects a range");
  }

  void DrawControls(bool busy, bool hasResults) {
    const auto& c = theme::Colors();
    const float fs = ImGui::GetFontSize();
    const float btnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    SectionHeader("Scan");
    ImGui::BeginDisabled(busy);
    if (!hasResults) {
      if (ImGui::Button("First Scan", ImVec2(btnW, 0))) StartScan(false);
      ImGui::SameLine();
      ImGui::BeginDisabled(true);
      ImGui::Button("Next Scan", ImVec2(btnW, 0));
      ImGui::EndDisabled();
    } else {
      if (ImGui::Button("New Scan", ImVec2(btnW, 0))) NewScan();
      ImGui::SameLine();
      if (ImGui::Button("Next Scan", ImVec2(btnW, 0))) StartScan(true);
    }
    ImGui::EndDisabled();
    if (busy) {
      ImGui::PushStyleColor(ImGuiCol_Button, c.danger);
      if (ImGui::Button("Cancel scan", ImVec2(-FLT_MIN, 0))) scanner_.Cancel();
      ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(busy);
    const float labelW = fs * 5.0f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Value type");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::BeginDisabled(hasResults);
    if (tools::ValueTypeCombo("##vtype", &type_)) {
      if (!OpAvailable(op_, type_, hasResults)) op_ = FirstAvailableOp(hasResults);
      SaveSettings();
    }
    ImGui::EndDisabled();
    if (hasResults) ImGui::SetItemTooltip("Press New Scan to change the value type");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Scan type");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##op", mem::ScanOpName(op_))) {
      for (ScanOp op : kAllOps) {
        if (!OpAvailable(op, type_, hasResults)) continue;
        const bool sel = op == op_;
        if (ImGui::Selectable(mem::ScanOpName(op), sel)) op_ = op;
        if (sel) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    bool submit = false;
    if (mem::ScanOpNeedsValue(op_)) {
      const char* hint = hex_ && IsIntegral(type_) ? "hex value" : (type_ == mem::ValueType::Bytes ? "90 90 C3" : "value");
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(op_ == ScanOp::Between ? "From" : "Value");
      ImGui::SameLine(labelW);
      ImGui::SetNextItemWidth(-FLT_MIN);
      submit |= tools::InputString("##value", &value_, ImGuiInputTextFlags_EnterReturnsTrue, hint);
      if (op_ == ScanOp::Between) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("To");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(-FLT_MIN);
        submit |= tools::InputString("##value2", &value2_, ImGuiInputTextFlags_EnterReturnsTrue, hint);
      }
    }
    if (submit && !busy) StartScan(hasResults);

    SectionHeader("Options");
    bool changed = false;
    changed |= ImGui::Checkbox("Hex", &hex_);
    ImGui::SetItemTooltip("Integer values are typed and shown in hexadecimal");
    ImGui::BeginDisabled(hasResults);
    changed |= ImGui::Checkbox("Writable memory only", &writableOnly_);
    ImGui::SetItemTooltip("Skip read-only pages (code, constants). Most game state is writable.");
    changed |= ImGui::Checkbox("Include module images", &includeImages_);
    ImGui::SetItemTooltip("Also scan the .data/.bss of loaded modules (static variables)");
    changed |= ImGui::Checkbox("Fast scan (aligned)", &fastScan_);
    ImGui::SetItemTooltip("Only test addresses aligned to min(value size, 4). Much faster; misses packed fields.");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!mem::IsFloat(type_));
    changed |= ImGui::Checkbox("Rounded floats", &floatRounded_);
    ImGui::SetItemTooltip("Exact float scans match within the precision you typed (100.5 matches 100.45..100.55)");
    ImGui::EndDisabled();

    SectionHeader("Memory range");
    ImGui::BeginDisabled(hasResults);
    uintptr_t ignored = 0;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Start");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(-FLT_MIN);
    AddressInput("##rstart", &rangeStartExpr_, &ignored);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("End");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(-FLT_MIN);
    AddressInput("##rend", &rangeEndExpr_, &ignored);
    if (ImGui::SmallButton("Whole process")) {
      rangeStartExpr_ = kDefaultRangeStart;
      rangeEndExpr_ = kDefaultRangeEnd;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Game module")) {
      const mem::Module& m = mem::Module::Main();
      if (m.Valid()) {
        rangeStartExpr_ = tools::FormatAddress(m.base);
        rangeEndExpr_ = tools::FormatAddress(m.base + m.size);
        if (!includeImages_) {
          includeImages_ = true;
          changed = true;
        }
      }
    }
    ImGui::SetItemTooltip("Limit the scan to the game executable's image (statics)");
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Snapshot cap");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##snap", &snapshotGiB_, 1, 16, "%d GiB")) changed = true;
    ImGui::SetItemTooltip("Upper bound for unknown-initial-value snapshots (protects the game's memory)");
    ImGui::EndDisabled();

    if (changed) SaveSettings();
  }

  mem::Scanner scanner_;
  mem::ValueType type_ = mem::ValueType::I32;
  ScanOp op_ = ScanOp::Exact;
  std::string value_, value2_;
  bool writableOnly_ = true, includeImages_ = false, fastScan_ = true, floatRounded_ = true, hex_ = false;
  int snapshotGiB_ = 2;
  std::string rangeStartExpr_ = kDefaultRangeStart, rangeEndExpr_ = kDefaultRangeEnd;
  std::string localError_;

  std::vector<mem::Scanner::Row> page_;
  size_t pageOffset_ = 0;
  uint64_t pageGen_ = UINT64_MAX;
  bool pageHex_ = false;
  uint64_t pageTimeMs_ = 0;

  std::unordered_map<uintptr_t, std::string> describeCache_;
  uint64_t describeGen_ = UINT64_MAX;

  size_t selAnchor_ = SIZE_MAX, selCursor_ = SIZE_MAX;
};

}  // namespace

std::unique_ptr<Window> MakeScannerWindow() { return std::make_unique<ScannerWindow>(); }

}  // namespace cg::ui
