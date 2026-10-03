// Cheat-Engine-style address table: descriptions, address expressions that are re-evaluated every
// frame (so pointer paths follow re-created objects), typed live values, freezing and per-entry
// hotkeys. The table is a process-wide singleton so AddressTableTick() keeps frozen values
// applied while the window is closed.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/hotkeys.h"
#include "core/mp_guard.h"
#include "core/paths.h"
#include "core/util.h"
#include "mem/safe.h"
#include "mem/value.h"
#include "render/fonts.h"
#include "render/overlay.h"
#include "render/theme.h"
#include "ui/notify.h"
#include "ui/tools/tools_common.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace cg::ui {
namespace {

using nlohmann::json;
namespace theme = render::theme;

constexpr const char* kCurrentKey = "tools.addresstable.current";
constexpr size_t kMaxLength = 4096;          // String/WString characters, Bytes bytes
constexpr size_t kDefaultLength = 32;
constexpr uint64_t kAutosaveIntervalMs = 1000;
constexpr uintmax_t kMaxTableFileBytes = 16u << 20;

bool HasLength(mem::ValueType t) {
  return t == mem::ValueType::String || t == mem::ValueType::WString || t == mem::ValueType::Bytes;
}
bool IsIntegral(mem::ValueType t) { return mem::IsNumeric(t) && !mem::IsFloat(t); }

size_t ByteSize(mem::ValueType t, size_t length) {
  switch (t) {
    case mem::ValueType::String: return length;
    case mem::ValueType::WString: return length * 2;
    case mem::ValueType::Bytes: return length;
    default: return mem::ValueSize(t);
  }
}

struct Entry {
  uint64_t uid = 0;
  std::string description;
  std::string expr;
  mem::ValueType type = mem::ValueType::I32;
  size_t length = kDefaultLength;
  bool hex = false;
  Hotkey hotkey;
  bool active = false;           // frozen; deliberately never persisted (see LoadEntries)
  std::vector<uint8_t> frozen;   // value written every frame while active
  // Evaluation cache, valid for one TableState::serial (one frame).
  uint64_t evalSerial = 0;
  bool resolved = false;
  uintptr_t address = 0;
  std::string error;
  bool writeFailed = false;
};

size_t ByteSize(const Entry& e) { return ByteSize(e.type, e.length); }

std::string Label(const Entry& e) { return e.description.empty() ? e.expr : e.description; }

struct TableState {
  std::vector<Entry> entries;
  uint64_t nextUid = 1;
  uint64_t serial = 1;   // bumped once per tick; Evaluate() caches per serial
  uint64_t lastTickFrame = 0;
  bool tickedOnce = false;
  bool loaded = false;
  bool dirty = false;
  uint64_t lastSaveMs = 0;
  std::string name;      // last saved/loaded table file name ("" = never saved)
  std::vector<uint8_t> scratch;
};

std::atomic<bool> g_reloadFromConfig{false};

TableState& RawState() {
  static TableState s;
  return s;
}

// ----------------------------------------------------------------------------------------------
// Serialization
// ----------------------------------------------------------------------------------------------

json EntryToJson(const Entry& e) {
  json j = json::object();
  j["description"] = e.description;
  j["expr"] = e.expr;
  j["type"] = mem::ValueTypeName(e.type);
  if (HasLength(e.type)) j["length"] = e.length;
  if (e.hex) j["hex"] = true;
  if (e.hotkey.Valid()) j["hotkey"] = e.hotkey.ToString();
  if (!e.frozen.empty()) j["frozen"] = util::HexBytes(e.frozen.data(), e.frozen.size(), ' ');
  return j;
}

bool EntryFromJson(const json& j, Entry& e) {
  try {
    if (!j.is_object()) return false;
    e.expr = util::Trim(j.value("expr", std::string()));
    if (e.expr.empty()) return false;
    e.description = j.value("description", std::string());
    e.type = mem::ParseValueType(j.value("type", std::string("i32"))).value_or(mem::ValueType::I32);
    const int64_t len = j.value("length", static_cast<int64_t>(kDefaultLength));
    e.length = static_cast<size_t>(std::clamp<int64_t>(len, 1, static_cast<int64_t>(kMaxLength)));
    e.hex = j.value("hex", false);
    e.hotkey = Hotkey::Parse(j.value("hotkey", std::string()));
    e.frozen.clear();
    const std::string frozen = j.value("frozen", std::string());
    if (!frozen.empty() && !mem::ParseValue(mem::ValueType::Bytes, frozen, e.frozen)) e.frozen.clear();
    // Freezing is never restored from disk: after a game restart the expression may resolve to
    // unrelated memory and writing a stale value there could corrupt the game.
    e.active = false;
    return true;
  } catch (const json::exception&) {
    return false;
  }
}

json TableToJson(const TableState& s) {
  json arr = json::array();
  for (const Entry& e : s.entries) arr.push_back(EntryToJson(e));
  json root = json::object();
  root["schema"] = 1;
  root["name"] = s.name;
  root["entries"] = std::move(arr);
  return root;
}

// Accepts {"entries": [...]} or a bare array. Returns the number of entries appended.
size_t LoadEntries(const json& root, TableState& s, std::vector<Entry>& out) {
  const json* arr = nullptr;
  if (root.is_array()) arr = &root;
  else if (root.is_object() && root.contains("entries") && root["entries"].is_array()) arr = &root["entries"];
  if (!arr) return 0;
  size_t n = 0;
  for (const json& j : *arr) {
    Entry e;
    if (!EntryFromJson(j, e)) continue;
    e.uid = s.nextUid++;
    out.push_back(std::move(e));
    ++n;
  }
  return n;
}

void MarkDirty() { RawState().dirty = true; }

void FlushAutosave(bool force) {
  TableState& s = RawState();
  if (!s.loaded || !s.dirty) return;
  const uint64_t now = util::NowMs();
  if (!force && now - s.lastSaveMs < kAutosaveIntervalMs) return;
  Config::Get().WriteJson(kCurrentKey, TableToJson(s));
  s.dirty = false;
  s.lastSaveMs = now;
}

void LoadFromConfig(TableState& s) {
  const json root = Config::Get().ReadJson(kCurrentKey);
  s.entries.clear();
  LoadEntries(root, s, s.entries);
  s.name.clear();
  try {
    if (root.is_object()) s.name = root.value("name", std::string());
  } catch (const json::exception&) {
    s.name.clear();
  }
  s.dirty = false;
}

void OnConfigReloaded() { g_reloadFromConfig.store(true, std::memory_order_relaxed); }

// The table, loaded from Config on first use and again after a profile switch.
TableState& Table() {
  TableState& s = RawState();
  if (!s.loaded) {
    s.loaded = true;
    Config::Get().AddReloadListener(&OnConfigReloaded);
    LoadFromConfig(s);
  }
  if (g_reloadFromConfig.exchange(false, std::memory_order_relaxed)) LoadFromConfig(s);
  return s;
}

// ----------------------------------------------------------------------------------------------
// Table files: paths::Data(L"tables")/<name>.json
// ----------------------------------------------------------------------------------------------

std::filesystem::path TablesDir() { return paths::Data(L"tables"); }

bool ValidTableName(const std::string& n) {
  if (n.empty() || n.size() > 64 || n.front() == '.' || n.front() == ' ' || n.back() == ' ' || n.back() == '.') return false;
  for (char c : n) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' ||
                    c == '-' || c == '.' || c == '(' || c == ')';
    if (!ok) return false;
  }
  return true;
}

std::filesystem::path TableFile(const std::string& name) { return TablesDir() / (util::Widen(name) + L".json"); }

bool WriteFileAtomic(const std::filesystem::path& p, const std::string& data, std::string& err) {
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::filesystem::path tmp = p;
  tmp += L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      err = "cannot open " + util::Narrow(tmp.wstring()) + " for writing";
      return false;
    }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.flush();
    if (!f) {
      err = "write failed";
      f.close();
      std::filesystem::remove(tmp, ec);
      return false;
    }
  }
  if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    err = "rename failed (Win32 error " + std::to_string(GetLastError()) + ")";
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
}

std::optional<std::string> ReadSmallFile(const std::filesystem::path& p, std::string& err) {
  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(p, ec);
  if (ec) {
    err = "file not found";
    return std::nullopt;
  }
  if (size > kMaxTableFileBytes) {
    err = "file is larger than 16 MB";
    return std::nullopt;
  }
  std::ifstream f(p, std::ios::binary);
  if (!f) {
    err = "cannot open file";
    return std::nullopt;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::vector<std::string> ListTableFiles() {
  std::vector<std::string> out;
  std::error_code ec;
  std::filesystem::directory_iterator it(TablesDir(), ec);
  const std::filesystem::directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code fec;
    if (!it->is_regular_file(fec)) continue;
    const std::filesystem::path& p = it->path();
    if (!util::IEquals(util::Narrow(p.extension().wstring()), ".json")) continue;
    out.push_back(util::Narrow(p.stem().wstring()));
  }
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return util::ToLower(a) < util::ToLower(b); });
  return out;
}

// ----------------------------------------------------------------------------------------------
// Values
// ----------------------------------------------------------------------------------------------

void Evaluate(Entry& e, uint64_t serial) {
  if (e.evalSerial == serial) return;
  e.evalSerial = serial;
  mem::ExprResult r = tools::Eval(e.expr);
  e.resolved = r.ok;
  e.address = r.ok ? r.value : 0;
  if (r.ok) e.error.clear();
  else e.error = std::move(r.error);
}

// Reads the entry's bytes into buf. Strings that run into an unreadable page are cut at the page
// boundary. Returns the number of valid bytes (0 = unreadable).
size_t ReadEntry(const Entry& e, std::vector<uint8_t>& buf) {
  const size_t n = ByteSize(e);
  buf.resize(n);
  if (!e.resolved || n == 0) return 0;
  if (mem::ReadRaw(e.address, buf.data(), n)) return n;
  if (e.type == mem::ValueType::String || e.type == mem::ValueType::WString) {
    const size_t toPageEnd = 0x1000 - (e.address & 0xFFF);
    if (toPageEnd < n && mem::ReadRaw(e.address, buf.data(), toPageEnd)) return toPageEnd;
  }
  return 0;
}

std::optional<std::string> FormatEntryValue(const Entry& e, std::vector<uint8_t>& buf) {
  size_t n = ReadEntry(e, buf);
  if (n == 0) return std::nullopt;
  if (e.type == mem::ValueType::String) {
    n = static_cast<size_t>(std::find(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(n), uint8_t{0}) - buf.begin());
  } else if (e.type == mem::ValueType::WString) {
    n &= ~size_t{1};
    for (size_t i = 0; i + 1 < n; i += 2) {
      if (buf[i] == 0 && buf[i + 1] == 0) {
        n = i;
        break;
      }
    }
  }
  return mem::FormatValue(e.type, buf.data(), n, e.hex && IsIntegral(e.type));
}

std::string WriteFailureReason() {
  if (mp_guard::Blocked()) return "blocked by the multiplayer guard: " + mp_guard::Reason();
  return "the address is not writable";
}

bool CaptureFrozen(Entry& e, uint64_t serial, std::string& err) {
  Evaluate(e, serial);
  if (!e.resolved) {
    err = "address does not resolve (" + e.error + ")";
    return false;
  }
  std::vector<uint8_t> buf(ByteSize(e));
  if (buf.empty() || !mem::ReadRaw(e.address, buf.data(), buf.size())) {
    err = "address is not readable";
    return false;
  }
  e.frozen = std::move(buf);
  return true;
}

bool SetFrozen(Entry& e, bool on, uint64_t serial, std::string& err) {
  if (!on) {
    e.active = false;
    e.writeFailed = false;
    return true;
  }
  if (!CaptureFrozen(e, serial, err)) return false;
  e.active = true;
  e.writeFailed = false;
  MarkDirty();
  return true;
}

// Parses `text` for the entry's type and writes it. Strings get a terminator and must fit the
// entry length so a long string never runs over neighbouring data.
bool WriteValueText(Entry& e, const std::string& rawText, uint64_t serial, std::string& err) {
  Evaluate(e, serial);
  if (!e.resolved) {
    err = "address does not resolve (" + e.error + ")";
    return false;
  }
  std::string text = util::Trim(rawText);
  if (e.type == mem::ValueType::String || e.type == mem::ValueType::WString) text = rawText;
  if (e.hex && IsIntegral(e.type) && !text.empty() && text[0] != '-' && !util::StartsWith(util::ToLower(text), "0x")) text = "0x" + text;
  std::vector<uint8_t> bytes;
  if (!mem::ParseValue(e.type, text, bytes)) {
    err = std::string("not a valid ") + mem::ValueTypeName(e.type) + " value";
    return false;
  }
  const size_t capacity = ByteSize(e);
  if (e.type == mem::ValueType::String) {
    bytes.push_back(0);
  } else if (e.type == mem::ValueType::WString) {
    bytes.push_back(0);
    bytes.push_back(0);
  }
  if (HasLength(e.type) && bytes.size() > capacity) {
    err = "value needs " + std::to_string(bytes.size()) + " bytes but the entry length allows " + std::to_string(capacity);
    return false;
  }
  if (bytes.empty()) {
    err = "nothing to write";
    return false;
  }
  if (!mem::WriteRaw(e.address, bytes.data(), bytes.size())) {
    err = WriteFailureReason();
    return false;
  }
  if (e.active) {
    std::string ignored;
    if (!CaptureFrozen(e, serial, ignored)) e.active = false;
    MarkDirty();
  }
  return true;
}

void ApplyFreeze(TableState& s, Entry& e) {
  if (e.frozen.empty()) return;
  Evaluate(e, s.serial);
  if (!e.resolved) return;   // pointer path temporarily broken (loading screen); stay armed
  const size_t n = e.frozen.size();
  s.scratch.resize(n);
  if (mem::ReadRaw(e.address, s.scratch.data(), n) && std::memcmp(s.scratch.data(), e.frozen.data(), n) == 0) {
    e.writeFailed = false;
    return;
  }
  const bool ok = mem::WriteRaw(e.address, e.frozen.data(), n);
  if (!ok && !e.writeFailed) notify::Push(notify::Kind::Warning, "Freeze write failed", Label(e) + ": " + WriteFailureReason());
  e.writeFailed = !ok;
}

void AddEntryImpl(uintptr_t a, const std::string& typeName, const std::string& desc) {
  TableState& s = Table();
  Entry e;
  e.uid = s.nextUid++;
  e.description = desc;
  e.expr = tools::AddressToExpr(a);
  e.type = mem::ParseValueType(typeName).value_or(mem::ValueType::I32);
  s.entries.push_back(std::move(e));
  MarkDirty();
}

// ----------------------------------------------------------------------------------------------
// Window
// ----------------------------------------------------------------------------------------------

enum Col { kColFreeze, kColDesc, kColAddr, kColResolved, kColType, kColValue, kColHotkey, kColCount };

class AddressTableWindow final : public Window, public AddressTableTarget {
 public:
  ~AddressTableWindow() override { FlushAutosave(true); }

  const char* Id() const override { return tools::kAddressTableId; }
  const char* Title() const override { return "Address Table"; }
  const char* Icon() const override { return CG_ICON_TABLE; }
  const char* Group() const override { return tools::kReGroup; }
  float DefaultWidth() const override { return 900; }
  float DefaultHeight() const override { return 440; }

  void AddEntry(uintptr_t a, const std::string& type, const std::string& desc) override {
    AddEntryImpl(a, type, desc);
    notify::Push(notify::Kind::Success, "Added to address table", desc.empty() ? mem::Describe(a) : desc, 2.0f);
  }

  void Draw() override {
    AddressTableTick();
    TableState& s = Table();
    DrawToolbar(s);
    DrawTable(s);
    DrawAddPopup(s);
    DrawSavePopup(s);
    DrawLoadPopup(s);
    DrawClearPopup(s);
    ApplyDeferred(s);
    FlushAutosave(false);
  }

 private:
  struct EditState {
    uint64_t uid = 0;
    int column = -1;
    std::string text;
    bool focus = false;
    int framesInactive = 0;
  };

  bool IsEditing(const Entry& e, int col) const { return edit_.uid == e.uid && edit_.column == col; }

  void BeginEdit(Entry& e, int col) {
    if (col != kColDesc && col != kColAddr && col != kColValue) return;
    edit_ = {};
    edit_.uid = e.uid;
    edit_.column = col;
    edit_.focus = true;
    if (col == kColDesc) edit_.text = e.description;
    else if (col == kColAddr) edit_.text = e.expr;
    else edit_.text = FormatEntryValue(e, buf_).value_or(std::string());
  }

  void CommitEdit(TableState& s, Entry& e) {
    const int col = edit_.column;
    std::string text = edit_.text;
    edit_ = {};
    if (col == kColDesc) {
      e.description = std::move(text);
      MarkDirty();
    } else if (col == kColAddr) {
      std::string expr = util::Trim(text);
      if (expr.empty()) {
        notify::Push(notify::Kind::Warning, "Address unchanged", "The address expression cannot be empty");
        return;
      }
      if (expr != e.expr) {
        e.expr = std::move(expr);
        e.evalSerial = 0;
        // The frozen bytes were captured at the old location; re-arming is an explicit user action.
        e.active = false;
        e.writeFailed = false;
        MarkDirty();
      }
    } else if (col == kColValue) {
      std::string err;
      if (!WriteValueText(e, text, s.serial, err)) notify::Push(notify::Kind::Error, "Write failed", Label(e) + ": " + err);
    }
  }

  void DrawEditor(TableState& s, Entry& e) {
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (edit_.focus) {
      ImGui::SetKeyboardFocusHere();
      edit_.focus = false;
    }
    const bool enter = tools::InputString("##edit", &edit_.text, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (enter) {
      CommitEdit(s, e);
      return;
    }
    if (ImGui::IsItemDeactivated()) {
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) edit_ = {};
      else CommitEdit(s, e);
      return;
    }
    // Focus request never landed (row scrolled away, window lost focus): drop the editor.
    if (ImGui::IsItemActive()) edit_.framesInactive = 0;
    else if (++edit_.framesInactive > 3) edit_ = {};
  }

  void HandleRowClick(const Entry& e, int visibleIndex, const std::vector<size_t>& visible, const TableState& s) {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl) {
      if (!selected_.erase(e.uid)) selected_.insert(e.uid);
    } else if (io.KeyShift && lastClickedIndex_ >= 0) {
      selected_.clear();
      const int lo = std::min(lastClickedIndex_, visibleIndex), hi = std::max(lastClickedIndex_, visibleIndex);
      for (int i = lo; i <= hi && i < static_cast<int>(visible.size()); ++i) selected_.insert(s.entries[visible[static_cast<size_t>(i)]].uid);
      return;
    } else {
      selected_.clear();
      selected_.insert(e.uid);
    }
    lastClickedIndex_ = visibleIndex;
  }

  void ToggleFreeze(TableState& s, Entry& e, bool on) {
    std::string err;
    if (!SetFrozen(e, on, s.serial, err)) notify::Push(notify::Kind::Warning, "Cannot freeze", Label(e) + ": " + err);
  }

  void DrawToolbar(TableState& s) {
    const auto& c = theme::Colors();
    if (ImGui::Button("Add address")) {
      addExpr_.clear();
      addDesc_.clear();
      addType_ = mem::ValueType::I32;
      addLength_ = static_cast<int>(kDefaultLength);
      addError_.clear();
      ImGui::OpenPopup("Add address##at");
    }
    ImGui::SameLine();
    if (ImGui::Button("Save as...")) {
      saveName_ = s.name.empty() ? "table" : s.name;
      fileError_.clear();
      ImGui::OpenPopup("Save table##at");
    }
    ImGui::SameLine();
    if (ImGui::Button("Load...")) {
      files_ = ListTableFiles();
      fileError_.clear();
      confirmDelete_.clear();
      ImGui::OpenPopup("Load table##at");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(s.entries.empty());
    if (ImGui::Button("Clear")) ImGui::OpenPopup("Clear table##at");
    ImGui::EndDisabled();
    ImGui::SameLine();
    size_t frozen = 0;
    for (const Entry& e : s.entries) frozen += e.active ? 1 : 0;
    ImGui::BeginDisabled(frozen == 0);
    if (ImGui::Button("Unfreeze all")) {
      for (Entry& e : s.entries) {
        e.active = false;
        e.writeFailed = false;
      }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    SearchBox("##atfilter", filter_, sizeof filter_, "Filter...");

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(c.textDim, "%s  |  %s entries, %s frozen", s.name.empty() ? "(unsaved table)" : s.name.c_str(),
                       tools::FormatCount(s.entries.size()).c_str(), tools::FormatCount(frozen).c_str());
    if (mp_guard::Blocked()) {
      ImGui::SameLine();
      Badge("WRITES BLOCKED", theme::U32(c.danger), theme::U32(c.text));
      ImGui::SetItemTooltip("%s", mp_guard::Reason().c_str());
    }
    ImGui::SameLine();
    HelpMarker(
        "Double-click a description, address or value to edit it. Addresses are expressions "
        "(module+offset, [pointer]+offset, bindings symbols) re-evaluated every frame. "
        "Freezing writes the captured value every frame, even while this window is closed. "
        "Freezes are not restored when a table is loaded.");
  }

  void DrawTable(TableState& s) {
    std::vector<size_t> visible;
    visible.reserve(s.entries.size());
    const std::string_view filter = filter_;
    for (size_t i = 0; i < s.entries.size(); ++i) {
      const Entry& e = s.entries[i];
      if (filter.empty() || util::IContains(e.description, filter) || util::IContains(e.expr, filter)) visible.push_back(i);
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_Hideable;
    if (!ImGui::BeginTable("##entries", kColCount, flags)) return;
    const float fs = ImGui::GetFontSize();
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Freeze", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide, fs * 3.4f);
    ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 2.2f);
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Resolved", ImGuiTableColumnFlags_WidthFixed, fs * 8.5f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, fs * 6.5f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Hotkey", ImGuiTableColumnFlags_WidthFixed, fs * 7.5f);
    ImGui::TableHeadersRow();

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()));
    while (clipper.Step()) {
      for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) DrawRow(s, s.entries[visible[static_cast<size_t>(r)]], r, visible);
    }
    ImGui::EndTable();

    if (s.entries.empty()) {
      ImGui::TextColored(theme::Colors().textFaint, "Empty. Add addresses here, or double-click scanner results.");
    }

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
      for (uint64_t uid : selected_) toDelete_.push_back(uid);
    }
  }

  void DrawRow(TableState& s, Entry& e, int visibleIndex, const std::vector<size_t>& visible) {
    const auto& c = theme::Colors();
    const float rowH = ImGui::GetFrameHeight();
    ImGui::PushID(static_cast<int>(e.uid & 0x7FFFFFFF));
    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
    Evaluate(e, s.serial);

    // Freeze
    ImGui::TableSetColumnIndex(kColFreeze);
    bool active = e.active;
    if (ImGui::Checkbox("##freeze", &active)) ToggleFreeze(s, e, active);
    if (e.active && (e.writeFailed || !e.resolved)) {
      ImGui::SameLine();
      StatusDot(theme::U32(e.writeFailed ? c.danger : c.warning),
                e.writeFailed ? "Writing the frozen value fails" : "Address does not resolve right now; the freeze resumes when it does");
    }

    // Description (+ the row selectable spanning every column)
    ImGui::TableSetColumnIndex(kColDesc);
    if (IsEditing(e, kColDesc)) {
      DrawEditor(s, e);
    } else {
      // Unlabelled selectable + separately drawn text, so a description containing "##" is
      // shown verbatim instead of being parsed as an ImGui id.
      const bool selected = selected_.count(e.uid) != 0;
      const ImVec2 cellPos = ImGui::GetCursorPos();
      const bool clicked = ImGui::Selectable("##row", selected,
                                             ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap |
                                                 ImGuiSelectableFlags_AllowDoubleClick,
                                             ImVec2(0, rowH));
      if (clicked) HandleRowClick(e, visibleIndex, visible, s);
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) BeginEdit(e, ImGui::TableGetHoveredColumn());
      if (ImGui::BeginPopupContextItem("##rowctx")) {
        if (!selected_.count(e.uid)) {
          selected_.clear();
          selected_.insert(e.uid);
        }
        DrawRowMenu(s, e);
        ImGui::EndPopup();
      }
      ImGui::SetCursorPos(cellPos);
      ImGui::AlignTextToFramePadding();
      if (e.description.empty()) ImGui::TextColored(c.textFaint, "(no description)");
      else ImGui::TextUnformatted(e.description.c_str());
    }

    // Address expression
    ImGui::TableSetColumnIndex(kColAddr);
    if (IsEditing(e, kColAddr)) {
      DrawEditor(s, e);
    } else {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(e.expr.c_str());
    }

    // Resolved
    ImGui::TableSetColumnIndex(kColResolved);
    ImGui::AlignTextToFramePadding();
    if (e.resolved) {
      tools::MonoFontScope mono;
      ImGui::TextUnformatted(tools::FormatAddress(e.address).c_str());
      if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted(mem::Describe(e.address).c_str());
        ImGui::EndTooltip();
      }
    } else {
      ImGui::TextColored(c.danger, "??");
      ImGui::SetItemTooltip("%s", e.error.c_str());
    }

    // Type (+ length for strings/bytes)
    ImGui::TableSetColumnIndex(kColType);
    DrawTypeCell(e);

    // Value
    ImGui::TableSetColumnIndex(kColValue);
    if (IsEditing(e, kColValue)) {
      DrawEditor(s, e);
    } else {
      ImGui::AlignTextToFramePadding();
      const std::optional<std::string> v = FormatEntryValue(e, buf_);
      if (!v) ImGui::TextColored(c.textFaint, "??");
      else if (e.active) ImGui::TextColored(c.accent, "%s", v->c_str());
      else ImGui::TextUnformatted(v->c_str());
    }

    // Hotkey
    ImGui::TableSetColumnIndex(kColHotkey);
    if (HotkeyButton("##hk", &e.hotkey)) MarkDirty();

    ImGui::PopID();
  }

  void DrawTypeCell(Entry& e) {
    char label[48];
    if (HasLength(e.type)) std::snprintf(label, sizeof label, "%s[%zu]", mem::ValueTypeName(e.type), e.length);
    else std::snprintf(label, sizeof label, "%s", mem::ValueTypeName(e.type));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::BeginCombo("##type", label, ImGuiComboFlags_HeightLarge)) return;
    for (mem::ValueType t : mem::kAllValueTypes) {
      const bool sel = t == e.type;
      if (ImGui::Selectable(mem::ValueTypeName(t), sel) && !sel) {
        e.type = t;
        e.active = false;   // the frozen bytes no longer match the type
        e.writeFailed = false;
        MarkDirty();
      }
      if (sel) ImGui::SetItemDefaultFocus();
    }
    if (HasLength(e.type)) {
      ImGui::Separator();
      int len = static_cast<int>(e.length);
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
      if (ImGui::InputInt(e.type == mem::ValueType::Bytes ? "bytes" : "chars", &len)) {
        const size_t n = static_cast<size_t>(std::clamp(len, 1, static_cast<int>(kMaxLength)));
        if (n != e.length) {
          e.length = n;
          e.active = false;
          e.writeFailed = false;
          MarkDirty();
        }
      }
    }
    ImGui::EndCombo();
  }

  void DrawRowMenu(TableState& s, Entry& e) {
    if (e.resolved) {
      tools::AddressMenuOptions o;
      o.valueType = mem::ValueTypeName(e.type);
      o.size = static_cast<int>(std::min<size_t>(ByteSize(e), 8));
      o.description = e.description.c_str();
      tools::AddressMenuItems(e.address, o);
    } else {
      ImGui::TextColored(theme::Colors().danger, "Unresolved: %s", e.error.c_str());
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Freeze", nullptr, e.active)) ToggleFreeze(s, e, !e.active);
    if (ImGui::MenuItem("Show as hex", nullptr, e.hex, IsIntegral(e.type))) {
      e.hex = !e.hex;
      MarkDirty();
    }
    if (ImGui::MenuItem("Edit description")) BeginEdit(e, kColDesc);
    if (ImGui::MenuItem("Edit address")) BeginEdit(e, kColAddr);
    if (ImGui::MenuItem("Edit value", nullptr, false, e.resolved)) BeginEdit(e, kColValue);
    if (ImGui::MenuItem("Copy expression")) tools::CopyToClipboard(e.expr, "Expression copied");
    ImGui::Separator();
    if (ImGui::MenuItem("Duplicate")) toDuplicate_.push_back(e.uid);
    char del[48];
    if (selected_.size() > 1) std::snprintf(del, sizeof del, "Delete %zu entries", selected_.size());
    else std::snprintf(del, sizeof del, "Delete");
    if (ImGui::MenuItem(del, "Del")) {
      if (selected_.empty()) toDelete_.push_back(e.uid);
      for (uint64_t uid : selected_) toDelete_.push_back(uid);
    }
  }

  void DrawAddPopup(TableState& s) {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Add address##at", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::TextUnformatted("Address expression");
    uintptr_t preview = 0;
    ImGui::SetNextItemWidth(-FLT_MIN);
    AddressInput("##addexpr", &addExpr_, &preview);
    ImGui::TextUnformatted("Description");
    ImGui::SetNextItemWidth(-FLT_MIN);
    tools::InputString("##adddesc", &addDesc_);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    tools::ValueTypeCombo("Type", &addType_);
    if (HasLength(addType_)) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
      ImGui::InputInt(addType_ == mem::ValueType::Bytes ? "bytes" : "chars", &addLength_);
      addLength_ = std::clamp(addLength_, 1, static_cast<int>(kMaxLength));
    }
    if (!addError_.empty()) ImGui::TextColored(theme::Colors().danger, "%s", addError_.c_str());
    ImGui::Separator();
    if (ImGui::Button("Add", ImVec2(ImGui::GetFontSize() * 6.0f, 0))) {
      const std::string expr = util::Trim(addExpr_);
      if (expr.empty()) {
        addError_ = "Enter an address expression";
      } else {
        Entry e;
        e.uid = s.nextUid++;
        e.expr = expr;
        e.description = addDesc_;
        e.type = addType_;
        e.length = static_cast<size_t>(addLength_);
        const mem::ExprResult r = tools::Eval(expr);
        if (!r.ok) notify::Push(notify::Kind::Warning, "Added unresolved entry", expr + ": " + r.error);
        s.entries.push_back(std::move(e));
        MarkDirty();
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(ImGui::GetFontSize() * 6.0f, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  void DrawSavePopup(TableState& s) {
    if (!ImGui::BeginPopupModal("Save table##at", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::TextUnformatted("Table name");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
    const bool enter = tools::InputString("##savename", &saveName_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextColored(theme::Colors().textFaint, "Saved to %s", util::Narrow(TablesDir().wstring()).c_str());
    if (!fileError_.empty()) ImGui::TextColored(theme::Colors().danger, "%s", fileError_.c_str());
    ImGui::Separator();
    if (ImGui::Button("Save", ImVec2(ImGui::GetFontSize() * 6.0f, 0)) || enter) {
      const std::string name = util::Trim(saveName_);
      std::string err;
      if (!ValidTableName(name)) {
        fileError_ = "Use 1-64 letters, digits, spaces, _ - . ( )";
      } else {
        const std::string previous = s.name;
        s.name = name;
        if (WriteFileAtomic(TableFile(name), TableToJson(s).dump(2), err)) {
          MarkDirty();
          notify::Push(notify::Kind::Success, "Table saved", name);
          ImGui::CloseCurrentPopup();
        } else {
          s.name = previous;
          fileError_ = err;
        }
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(ImGui::GetFontSize() * 6.0f, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  bool LoadTableFile(TableState& s, const std::string& name, bool merge) {
    std::string err;
    const std::optional<std::string> text = ReadSmallFile(TableFile(name), err);
    if (!text) {
      fileError_ = name + ": " + err;
      return false;
    }
    const json root = json::parse(*text, nullptr, false);
    if (root.is_discarded()) {
      fileError_ = name + ": not valid JSON";
      return false;
    }
    std::vector<Entry> loaded;
    const size_t n = LoadEntries(root, s, loaded);
    if (n == 0 && !(root.is_object() && root.contains("entries"))) {
      fileError_ = name + ": no address table entries found";
      return false;
    }
    if (!merge) {
      s.entries.clear();
      selected_.clear();
      edit_ = {};
      s.name = name;
    }
    for (Entry& e : loaded) s.entries.push_back(std::move(e));
    MarkDirty();
    notify::Push(notify::Kind::Success, merge ? "Table merged" : "Table loaded", name + " (" + std::to_string(n) + " entries)");
    return true;
  }

  void DrawLoadPopup(TableState& s) {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 28.0f, ImGui::GetFontSize() * 22.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Load table##at", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    const auto& c = theme::Colors();
    ImGui::TextColored(c.textDim, "%s", util::Narrow(TablesDir().wstring()).c_str());
    if (!fileError_.empty()) ImGui::TextColored(c.danger, "%s", fileError_.c_str());
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginChild("##files", ImVec2(0, -footer), ImGuiChildFlags_Borders)) {
      if (files_.empty()) ImGui::TextColored(c.textFaint, "No saved tables yet.");
      std::string removed;
      for (const std::string& f : files_) {
        ImGui::PushID(f.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(f.c_str());
        const float btnW = ImGui::GetFontSize() * 4.2f;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - btnW * 3 - ImGui::GetStyle().ItemSpacing.x * 2);
        if (ImGui::Button("Load", ImVec2(btnW, 0)) && LoadTableFile(s, f, false)) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ImGui::Button("Merge", ImVec2(btnW, 0)) && LoadTableFile(s, f, true)) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        const bool confirming = confirmDelete_ == f;
        if (confirming) ImGui::PushStyleColor(ImGuiCol_Button, c.danger);
        if (ImGui::Button(confirming ? "Sure?" : "Delete", ImVec2(btnW, 0))) {
          if (confirming) {
            std::error_code ec;
            if (std::filesystem::remove(TableFile(f), ec)) removed = f;
            else fileError_ = f + ": " + (ec ? ec.message() : std::string("not found"));
            confirmDelete_.clear();
          } else {
            confirmDelete_ = f;
          }
        }
        if (confirming) ImGui::PopStyleColor();
        ImGui::PopID();
      }
      if (!removed.empty()) files_.erase(std::remove(files_.begin(), files_.end(), removed), files_.end());
    }
    ImGui::EndChild();
    if (ImGui::Button("Refresh")) files_ = ListTableFiles();
    ImGui::SameLine();
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  void DrawClearPopup(TableState& s) {
    if (!ImGui::BeginPopupModal("Clear table##at", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::Text("Remove all %s entries? Saved table files are not affected.", tools::FormatCount(s.entries.size()).c_str());
    if (ImGui::Button("Clear", ImVec2(ImGui::GetFontSize() * 6.0f, 0))) {
      s.entries.clear();
      s.name.clear();
      selected_.clear();
      edit_ = {};
      MarkDirty();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(ImGui::GetFontSize() * 6.0f, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  void ApplyDeferred(TableState& s) {
    if (!toDuplicate_.empty()) {
      for (uint64_t uid : toDuplicate_) {
        auto it = std::find_if(s.entries.begin(), s.entries.end(), [uid](const Entry& e) { return e.uid == uid; });
        if (it == s.entries.end()) continue;
        Entry copy = *it;
        copy.uid = s.nextUid++;
        copy.active = false;
        copy.writeFailed = false;
        copy.hotkey = {};
        copy.evalSerial = 0;
        s.entries.insert(it + 1, std::move(copy));
      }
      toDuplicate_.clear();
      MarkDirty();
    }
    if (!toDelete_.empty()) {
      std::unordered_set<uint64_t> doomed(toDelete_.begin(), toDelete_.end());
      s.entries.erase(std::remove_if(s.entries.begin(), s.entries.end(), [&](const Entry& e) { return doomed.count(e.uid) != 0; }),
                      s.entries.end());
      for (uint64_t uid : doomed) selected_.erase(uid);
      if (doomed.count(edit_.uid)) edit_ = {};
      toDelete_.clear();
      lastClickedIndex_ = -1;
      MarkDirty();
    }
  }

  EditState edit_;
  std::unordered_set<uint64_t> selected_;
  int lastClickedIndex_ = -1;
  std::vector<uint8_t> buf_;
  char filter_[128] = {};

  std::string addExpr_, addDesc_, addError_;
  mem::ValueType addType_ = mem::ValueType::I32;
  int addLength_ = static_cast<int>(kDefaultLength);

  std::string saveName_, fileError_, confirmDelete_;
  std::vector<std::string> files_;

  std::vector<uint64_t> toDelete_, toDuplicate_;
};

}  // namespace

void AddressTableTick() {
  TableState& s = Table();
  const uint64_t frame = render::FrameCount();
  if (s.tickedOnce && frame == s.lastTickFrame) return;
  s.tickedOnce = true;
  s.lastTickFrame = frame;
  ++s.serial;
  for (Entry& e : s.entries) {
    if (e.hotkey.Valid() && hotkeys::Pressed(e.hotkey)) {
      std::string err;
      const bool on = !e.active;
      if (SetFrozen(e, on, s.serial, err)) notify::Push(notify::Kind::Info, on ? "Frozen" : "Unfrozen", Label(e), 2.0f);
      else notify::Push(notify::Kind::Warning, "Cannot freeze", Label(e) + ": " + err);
    }
    if (e.active) ApplyFreeze(s, e);
  }
  FlushAutosave(false);
}

std::unique_ptr<Window> MakeAddressTableWindow() { return std::make_unique<AddressTableWindow>(); }

}  // namespace cg::ui
