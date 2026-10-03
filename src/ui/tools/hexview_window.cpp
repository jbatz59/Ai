// Hex view: virtualised 16-bytes-per-row memory grid with in-place editing, pointer following,
// a typed data inspector, a disassembly tab and persisted bookmarks. Only the visible rows are
// read each frame (one bulk ReadRaw with a per-row fallback for partly unreadable ranges).
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/mp_guard.h"
#include "core/util.h"
#include "mem/disasm.h"
#include "mem/module.h"
#include "mem/pattern.h"
#include "mem/rtti.h"
#include "mem/safe.h"
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

constexpr uintptr_t kMaxAddr = 0x800000000000ull;   // exclusive upper bound of user-mode space
constexpr uintptr_t kRowBytes = 16;
constexpr uintptr_t kSpanChunk = 0x10000;           // loaded on each side of a target / per extension
constexpr uintptr_t kMaxSpan = 0x400000;            // virtual rows kept at once (float scroll precision)
constexpr float kEdgeRows = 8.0f;                   // extend the span this close to an edge
constexpr size_t kMaxHistory = 64;
constexpr uint64_t kChangeFlashMs = 1200;
constexpr size_t kDisasmBatch = 256;
constexpr size_t kDisasmMax = 4096;
constexpr uint64_t kDisasmRefreshMs = 1000;
constexpr size_t kMaxCopyBytes = 0x10000;
constexpr const char* kBookmarksKey = "tools.hexview.bookmarks";

uintptr_t AlignDown(uintptr_t a, uintptr_t al) { return a & ~(al - 1); }

bool Printable(uint8_t c) { return c >= 0x20 && c < 0x7F; }

// Text that looks like a real string (>= 3 printable chars before the terminator), else "".
std::string AsciiPreview(uintptr_t a, size_t maxLen) {
  const std::string s = mem::ReadCString(a, maxLen);
  if (s.size() < 3) return {};
  for (char ch : s) {
    if (!Printable(static_cast<uint8_t>(ch))) return {};
  }
  return s;
}
std::string WidePreview(uintptr_t a, size_t maxLen) {
  const std::wstring w = mem::ReadWString(a, maxLen);
  if (w.size() < 3) return {};
  std::string out;
  out.reserve(w.size());
  for (wchar_t ch : w) {
    if (ch < 0x20 || ch >= 0x7F) return {};
    out.push_back(static_cast<char>(ch));
  }
  return out;
}

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
  return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

struct RowCache {
  uint8_t bytes[kRowBytes]{};
  uint64_t changedAt[kRowBytes]{};
  bool readable = false;
  uint64_t lastFrame = 0;
};

struct Bookmark {
  std::string name;
  std::string expr;
};

struct DisasmLine {
  mem::Insn insn;
  std::string addrText;
  std::string bytesText;
  std::string targetText;
  std::string comment;
};

// Annotation for a RIP-relative operand: the string it points at, or the RTTI class of the
// object at / behind it.
std::string CommentForTarget(uintptr_t target) {
  if (std::string s = AsciiPreview(target, 64); !s.empty()) return "\"" + s + "\"";
  if (std::string s = WidePreview(target, 64); !s.empty()) return "L\"" + s + "\"";
  if (std::string cls = mem::rtti::ClassNameOf(target); !cls.empty()) return "object " + cls;
  if (auto p = mem::ReadPtr(target)) {
    if (std::string cls = mem::rtti::ClassNameOf(*p); !cls.empty()) return "-> " + cls + "*";
  }
  return {};
}

std::vector<DisasmLine> BuildDisassembly(uintptr_t addr, size_t count, const std::atomic<bool>& cancel) {
  std::vector<DisasmLine> out;
  const std::vector<mem::Insn> insns = mem::DecodeRange(addr, count);
  out.reserve(insns.size());
  for (const mem::Insn& in : insns) {
    if (cancel.load(std::memory_order_relaxed)) break;
    DisasmLine l;
    l.insn = in;
    l.addrText = mem::Describe(in.address);
    l.bytesText = util::HexBytes(in.bytes, std::min<size_t>(in.length, sizeof in.bytes));
    if ((in.relBranch || in.ripRelative) && in.target) {
      l.targetText = mem::Describe(in.target);
      if (in.ripRelative) l.comment = CommentForTarget(in.target);
    }
    out.push_back(std::move(l));
    if (in.length == 0) break;
  }
  return out;
}

enum Tab { kTabMemory, kTabDisasm, kTabBookmarks };

struct InspectorType {
  const char* label;
  mem::ValueType type;
};
constexpr InspectorType kInspectorTypes[] = {
    {"i8", mem::ValueType::I8},   {"u8", mem::ValueType::U8},   {"i16", mem::ValueType::I16}, {"u16", mem::ValueType::U16},
    {"i32", mem::ValueType::I32}, {"u32", mem::ValueType::U32}, {"i64", mem::ValueType::I64}, {"u64", mem::ValueType::U64},
    {"f32", mem::ValueType::F32}, {"f64", mem::ValueType::F64},
};

class HexViewWindow final : public Window, public HexViewTarget, public tools::DisassemblyTarget {
 public:
  HexViewWindow() { LoadBookmarks(); }

  const char* Id() const override { return tools::kHexViewId; }
  const char* Title() const override { return "Hex View"; }
  const char* Icon() const override { return CG_ICON_TOOLS; }
  const char* Group() const override { return tools::kReGroup; }
  float DefaultWidth() const override { return 960; }
  float DefaultHeight() const override { return 560; }

  void Goto(uintptr_t a) override {
    Navigate(a, true, false);
    selectTab_ = kTabMemory;
  }

  void Disassemble(uintptr_t a) override {
    Navigate(a, true, false);
    selectTab_ = kTabDisasm;
  }

  void Draw() override {
    ++frame_;
    if (!hasBase_) {
      const mem::Module& m = mem::Module::Main();
      Navigate(m.Valid() ? m.base : 0x10000, false, false);
    }
    DrawToolbar();
    if (!ImGui::BeginTabBar("##hvtabs")) return;
    const int forced = selectTab_;
    selectTab_ = -1;
    if (ImGui::BeginTabItem("Memory", nullptr, forced == kTabMemory ? ImGuiTabItemFlags_SetSelected : 0)) {
      DrawMemoryTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Disassembly", nullptr, forced == kTabDisasm ? ImGuiTabItemFlags_SetSelected : 0)) {
      DrawDisassemblyTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Bookmarks", nullptr, forced == kTabBookmarks ? ImGuiTabItemFlags_SetSelected : 0)) {
      DrawBookmarksTab();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

 private:
  // ------------------------------------------------------------------------------------------
  // Navigation
  // ------------------------------------------------------------------------------------------
  void Navigate(uintptr_t a, bool pushHistory, bool fromAddressBar) {
    a = std::min(a, kMaxAddr - 1);
    if (pushHistory && hasBase_ && a != base_) {
      back_.push_back(base_);
      if (back_.size() > kMaxHistory) back_.erase(back_.begin());
      fwd_.clear();
    }
    base_ = a;
    hasBase_ = true;
    if (!fromAddressBar) expr_ = tools::FormatAddress(a);
    anchor_ = cursor_ = a;
    hasSel_ = true;
    pendingNibble_ = -1;
    CenterSpanOn(a);
    disasmAddr_ = a;
    disasmExpr_ = tools::FormatAddress(a);
    disasmCount_ = kDisasmBatch;
    disasmScrollTop_ = true;
  }

  void CenterSpanOn(uintptr_t a) {
    const uintptr_t row = AlignDown(a, kRowBytes);
    spanStart_ = row > kSpanChunk ? row - kSpanChunk : 0;
    spanEnd_ = std::min(row + kSpanChunk, kMaxAddr);
    scrollRow_ = row;
    scrollMode_ = ScrollMode::Context;
  }

  void Back() {
    if (back_.empty()) return;
    fwd_.push_back(base_);
    const uintptr_t a = back_.back();
    back_.pop_back();
    Navigate(a, false, false);
  }
  void Forward() {
    if (fwd_.empty()) return;
    back_.push_back(base_);
    const uintptr_t a = fwd_.back();
    fwd_.pop_back();
    Navigate(a, false, false);
  }

  bool FollowPointerAt(uintptr_t at) {
    const uintptr_t q = AlignDown(at, 8);
    const std::optional<uint64_t> v = mem::Read<uint64_t>(q);
    if (!v || !mem::IsPlausiblePtr(*v) || !mem::IsReadable(*v, 1)) {
      notify::Push(notify::Kind::Warning, "Not a pointer",
                   tools::FormatAddress(q) + (v ? " holds " + util::Hex(*v) : std::string(" is unreadable")), 2.5f);
      return false;
    }
    Navigate(*v, true, false);
    return true;
  }

  // ------------------------------------------------------------------------------------------
  // Toolbar
  // ------------------------------------------------------------------------------------------
  void DrawToolbar() {
    const auto& c = theme::Colors();
    ImGui::BeginDisabled(back_.empty());
    if (ImGui::ArrowButton("##back", ImGuiDir_Left)) Back();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Back");
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(fwd_.empty());
    if (ImGui::ArrowButton("##fwd", ImGuiDir_Right)) Forward();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Forward");
    ImGui::SameLine();

    if (focusBar_) {
      ImGui::SetKeyboardFocusHere();
      focusBar_ = false;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
    uintptr_t target = 0;
    if (tools::AddressBar("##hvaddr", &expr_, &target)) Navigate(target, true, true);
    ImGui::SameLine();
    if (ImGui::Button("Go")) {
      const mem::ExprResult r = tools::Eval(expr_);
      if (r.ok) Navigate(r.value, true, true);
      else notify::Push(notify::Kind::Error, "Invalid address", r.error);
    }
    ImGui::SameLine();
    if (ImGui::Button("Bookmark")) AddBookmark(mem::Describe(base_), base_);
    ImGui::SetItemTooltip("Bookmark the current address (Bookmarks tab)");
    ImGui::SameLine();
    ImGui::Checkbox("Relative", &relative_);
    ImGui::SetItemTooltip("Show row offsets relative to the current address");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(c.textDim, "%s", mem::Describe(base_).c_str());
  }

  // ------------------------------------------------------------------------------------------
  // Memory tab
  // ------------------------------------------------------------------------------------------
  enum class ScrollMode { None, Context, Reveal };

  uintptr_t SelLo() const { return std::min(anchor_, cursor_); }
  uintptr_t SelHi() const { return std::max(anchor_, cursor_); }
  size_t SelLen() const { return hasSel_ ? static_cast<size_t>(SelHi() - SelLo() + 1) : 0; }
  bool InSel(uintptr_t a) const { return hasSel_ && a >= SelLo() && a <= SelHi(); }

  void MoveCursor(intptr_t delta, bool extend) {
    if (!hasSel_) return;
    const intptr_t next = static_cast<intptr_t>(cursor_) + delta;
    cursor_ = static_cast<uintptr_t>(std::clamp<intptr_t>(next, 0, static_cast<intptr_t>(kMaxAddr - 1)));
    if (!extend) anchor_ = cursor_;
    pendingNibble_ = -1;
    if (cursor_ < spanStart_ || cursor_ >= spanEnd_) CenterSpanOn(cursor_);
    scrollRow_ = AlignDown(cursor_, kRowBytes);
    scrollMode_ = ScrollMode::Reveal;
  }

  // Span growth and scroll requests are applied before the grid child begins, so the new scroll
  // position takes effect in the same frame the rows are laid out (no one-frame jump).
  void PrepareGridScroll() {
    if (rowH_ <= 0) return;
    const float totalRows = static_cast<float>((spanEnd_ - spanStart_) / kRowBytes);
    if (scrollMode_ != ScrollMode::None) {
      const float rowY = static_cast<float>((scrollRow_ - spanStart_) / kRowBytes) * rowH_;
      float y = lastScrollY_;
      if (scrollMode_ == ScrollMode::Context) {
        y = rowY - std::max(lastViewH_ * 0.25f, 0.0f);
      } else if (rowY < lastScrollY_) {
        y = rowY;
      } else if (rowY + rowH_ > lastScrollY_ + lastViewH_) {
        y = rowY + rowH_ - lastViewH_;
      }
      y = std::clamp(y, 0.0f, std::max(totalRows * rowH_ - lastViewH_, 0.0f));
      ImGui::SetNextWindowScroll(ImVec2(-1.0f, y));
      lastScrollY_ = y;
      scrollMode_ = ScrollMode::None;
      return;
    }
    const float topRows = lastScrollY_ / rowH_;
    const float bottomRows = totalRows - (lastScrollY_ + lastViewH_) / rowH_;
    if (topRows < kEdgeRows && spanStart_ > 0) {
      const uintptr_t add = std::min(kSpanChunk, spanStart_);
      spanStart_ -= add;
      float y = lastScrollY_ + static_cast<float>(add / kRowBytes) * rowH_;
      if (spanEnd_ - spanStart_ > kMaxSpan) spanEnd_ = spanStart_ + kMaxSpan;
      ImGui::SetNextWindowScroll(ImVec2(-1.0f, y));
      lastScrollY_ = y;
    } else if (bottomRows < kEdgeRows && spanEnd_ < kMaxAddr) {
      spanEnd_ += std::min(kSpanChunk, kMaxAddr - spanEnd_);
      if (spanEnd_ - spanStart_ > kMaxSpan) {
        const uintptr_t drop = (spanEnd_ - spanStart_) - kMaxSpan;
        spanStart_ += drop;
        const float y = std::max(lastScrollY_ - static_cast<float>(drop / kRowBytes) * rowH_, 0.0f);
        ImGui::SetNextWindowScroll(ImVec2(-1.0f, y));
        lastScrollY_ = y;
      }
    }
  }

  void ReadRows(uintptr_t first, int count, uint64_t now) {
    const size_t bytes = static_cast<size_t>(count) * kRowBytes;
    block_.resize(bytes);
    const bool all = bytes > 0 && mem::ReadRaw(first, block_.data(), bytes);
    for (int r = 0; r < count; ++r) {
      const uintptr_t ra = first + static_cast<uintptr_t>(r) * kRowBytes;
      uint8_t fresh[kRowBytes];
      bool ok = true;
      if (all) std::memcpy(fresh, block_.data() + static_cast<size_t>(r) * kRowBytes, kRowBytes);
      else ok = mem::ReadRaw(ra, fresh, kRowBytes);   // rows never straddle a page
      auto [it, inserted] = rows_.try_emplace(ra);
      RowCache& rc = it->second;
      if (!inserted && rc.readable && ok) {
        for (size_t i = 0; i < kRowBytes; ++i) {
          if (fresh[i] != rc.bytes[i]) rc.changedAt[i] = now;
        }
      }
      if (ok) std::memcpy(rc.bytes, fresh, kRowBytes);
      rc.readable = ok;
      rc.lastFrame = frame_;
    }
  }

  void PruneRows() {
    for (auto it = rows_.begin(); it != rows_.end();) {
      if (it->second.lastFrame != frame_) it = rows_.erase(it);
      else ++it;
    }
  }

  struct Layout {
    float charW = 0, addrX = 0, hexX = 0, asciiX = 0, totalW = 0;
    float HexCell(int i) const { return hexX + static_cast<float>(i * 3) * charW + (i >= 8 ? charW : 0.0f); }
  };

  Layout ComputeLayout() const {
    Layout l;
    l.charW = ImGui::CalcTextSize("0").x;
    const float addrChars = relative_ ? 9.0f : 12.0f;
    l.addrX = 0;
    l.hexX = (addrChars + 2.0f) * l.charW;
    l.asciiX = l.HexCell(15) + 4.0f * l.charW;
    l.totalW = l.asciiX + 17.0f * l.charW;
    return l;
  }

  // Byte index under relX, and whether it is in the ASCII column. -1 when between columns.
  static int HitTest(const Layout& l, float relX, bool& ascii) {
    for (int i = 0; i < static_cast<int>(kRowBytes); ++i) {
      const float x0 = l.HexCell(i) - l.charW * 0.5f;
      if (relX >= x0 && relX < x0 + 3.0f * l.charW) {
        ascii = false;
        return i;
      }
    }
    if (relX >= l.asciiX && relX < l.asciiX + 16.0f * l.charW) {
      ascii = true;
      return std::clamp(static_cast<int>((relX - l.asciiX) / l.charW), 0, 15);
    }
    return -1;
  }

  void DrawMemoryTab() {
    const float inspectorW = ImGui::GetFontSize() * 19.0f;
    const float gridW = std::max(ImGui::GetContentRegionAvail().x - inspectorW - ImGui::GetStyle().ItemSpacing.x, ImGui::GetFontSize() * 10.0f);
    {
      tools::MonoFontScope mono;
      rowH_ = ImGui::GetTextLineHeightWithSpacing();
    }
    PrepareGridScroll();
    if (ImGui::BeginChild("##grid", ImVec2(gridW, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_HorizontalScrollbar)) {
      DrawGrid();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##inspector", ImVec2(0, 0), ImGuiChildFlags_Borders)) DrawInspector();
    ImGui::EndChild();
  }

  void DrawGrid() {
    const auto& c = theme::Colors();
    tools::MonoFontScope mono;
    const Layout l = ComputeLayout();
    const float lineH = ImGui::GetTextLineHeight();
    const uint64_t now = util::NowMs();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();

    const ImU32 colAddr = theme::U32(c.textDim);
    const ImU32 colAddrBase = theme::U32(c.accent);
    const ImU32 colText = theme::U32(c.text);
    const ImU32 colZero = theme::U32(c.textFaint);
    const ImU32 colUnreadable = theme::U32(c.textFaint, 0.6f);
    const ImU32 colSel = theme::U32(c.accentDim, 0.55f);
    const ImU32 colCursor = theme::U32(c.accent);
    const ImU32 colCursorSoft = theme::U32(c.accent, 0.45f);
    const uintptr_t baseRow = AlignDown(base_, kRowBytes);

    int hoveredIdx = -1;
    bool hoveredAscii = false;
    uintptr_t hoveredRow = 0;

    const int rowCount = static_cast<int>((spanEnd_ - spanStart_) / kRowBytes);
    ImGuiListClipper clipper;
    clipper.Begin(rowCount, rowH_);
    while (clipper.Step()) {
      const uintptr_t first = spanStart_ + static_cast<uintptr_t>(clipper.DisplayStart) * kRowBytes;
      ReadRows(first, clipper.DisplayEnd - clipper.DisplayStart, now);
      for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
        const uintptr_t ra = spanStart_ + static_cast<uintptr_t>(r) * kRowBytes;
        const RowCache& rc = rows_[ra];
        const ImVec2 p = ImGui::GetCursorScreenPos();

        ImGui::PushID(reinterpret_cast<const void*>(ra));
        ImGui::InvisibleButton("##row", ImVec2(l.totalW, lineH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool rowHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        ImGui::PopID();
        if (rowHovered) {
          bool ascii = false;
          const int idx = HitTest(l, io.MousePos.x - p.x, ascii);
          if (idx >= 0) {
            hoveredIdx = idx;
            hoveredAscii = ascii;
            hoveredRow = ra;
          }
        }

        // Offset column
        char buf[32];
        if (relative_) {
          const intptr_t off = static_cast<intptr_t>(ra) - static_cast<intptr_t>(baseRow);
          std::snprintf(buf, sizeof buf, "%c%07llX", off < 0 ? '-' : '+', static_cast<unsigned long long>(off < 0 ? -off : off));
        } else {
          std::snprintf(buf, sizeof buf, "%012llX", static_cast<unsigned long long>(ra));
        }
        dl->AddText(ImVec2(p.x + l.addrX, p.y), ra == baseRow ? colAddrBase : colAddr, buf);

        for (int i = 0; i < static_cast<int>(kRowBytes); ++i) {
          const uintptr_t a = ra + static_cast<uintptr_t>(i);
          const float hx = p.x + l.HexCell(i);
          const float ax = p.x + l.asciiX + static_cast<float>(i) * l.charW;
          if (InSel(a)) {
            const bool nextSel = i < 15 && InSel(a + 1) && i != 7;
            dl->AddRectFilled(ImVec2(hx - l.charW * 0.5f, p.y), ImVec2(hx + (nextSel ? 2.5f : 2.25f) * l.charW, p.y + lineH), colSel);
            dl->AddRectFilled(ImVec2(ax, p.y), ImVec2(ax + l.charW, p.y + lineH), colSel);
          }
          if (hasSel_ && a == cursor_) {
            dl->AddRect(ImVec2(hx - l.charW * 0.25f, p.y), ImVec2(hx + 2.25f * l.charW, p.y + lineH), asciiFocus_ ? colCursorSoft : colCursor, 2.0f);
            dl->AddRect(ImVec2(ax, p.y), ImVec2(ax + l.charW, p.y + lineH), asciiFocus_ ? colCursor : colCursorSoft, 2.0f);
          }
          if (!rc.readable) {
            dl->AddText(ImVec2(hx, p.y), colUnreadable, "??");
            dl->AddText(ImVec2(ax, p.y), colUnreadable, "?");
            continue;
          }
          const uint8_t b = rc.bytes[i];
          ImU32 col = b == 0 ? colZero : colText;
          if (rc.changedAt[i] && now - rc.changedAt[i] < kChangeFlashMs) {
            const float t = 1.0f - static_cast<float>(now - rc.changedAt[i]) / static_cast<float>(kChangeFlashMs);
            col = theme::U32(Mix(b == 0 ? c.textFaint : c.text, c.warning, t));
          }
          char hex[3];
          if (hasSel_ && a == cursor_ && pendingNibble_ >= 0) {
            std::snprintf(hex, sizeof hex, "%X_", pendingNibble_);
            dl->AddText(ImVec2(hx, p.y), colCursor, hex);
          } else {
            std::snprintf(hex, sizeof hex, "%02X", b);
            dl->AddText(ImVec2(hx, p.y), col, hex);
          }
          const char ch[2] = {Printable(b) ? static_cast<char>(b) : '.', 0};
          dl->AddText(ImVec2(ax, p.y), Printable(b) ? col : colZero, ch);
        }
      }
    }
    lastScrollY_ = ImGui::GetScrollY();
    lastViewH_ = ImGui::GetWindowHeight();
    PruneRows();

    HandleGridMouse(hoveredRow, hoveredIdx, hoveredAscii);
    HandleGridKeyboard();
    DrawByteContextMenu();
  }

  void HandleGridMouse(uintptr_t hoveredRow, int hoveredIdx, bool hoveredAscii) {
    const ImGuiIO& io = ImGui::GetIO();
    if (dragging_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging_ = false;
    if (hoveredIdx < 0) return;
    const uintptr_t a = hoveredRow + static_cast<uintptr_t>(hoveredIdx);

    if (io.KeyCtrl) {
      const uintptr_t q = AlignDown(a, 8);
      const std::optional<uint64_t> v = mem::Read<uint64_t>(q);
      if (ImGui::BeginTooltip()) {
        if (v && mem::IsPlausiblePtr(*v)) {
          const std::string cls = mem::rtti::ClassNameOf(*v);
          ImGui::Text("Ctrl+click: follow %s", mem::Describe(*v).c_str());
          if (!cls.empty()) ImGui::TextColored(theme::Colors().accent, "%s", cls.c_str());
        } else {
          ImGui::TextColored(theme::Colors().textDim, "%s is not a pointer", tools::FormatAddress(q).c_str());
        }
        ImGui::EndTooltip();
      }
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered()) {
      if (io.KeyCtrl) {
        FollowPointerAt(a);
        return;
      }
      cursor_ = a;
      if (!io.KeyShift || !hasSel_) anchor_ = a;
      hasSel_ = true;
      asciiFocus_ = hoveredAscii;
      pendingNibble_ = -1;
      dragging_ = true;
    } else if (dragging_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
      cursor_ = a;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsWindowHovered()) {
      if (!InSel(a)) {
        anchor_ = cursor_ = a;
        hasSel_ = true;
      }
      ctxAddr_ = a;
      pendingNibble_ = -1;
      ImGui::OpenPopup("##bytectx");
    }
  }

  void WriteByteAtCursor(uint8_t value) {
    if (!mem::Write<uint8_t>(cursor_, value)) {
      notify::Push(notify::Kind::Error, "Write failed",
                   tools::FormatAddress(cursor_) + (mp_guard::Blocked() ? ": blocked by the multiplayer guard" : " is not writable"));
      pendingNibble_ = -1;
      return;
    }
    MoveCursor(1, false);
  }

  void HandleGridKeyboard() {
    if (!hasSel_ || !ImGui::IsWindowFocused() || ImGui::IsAnyItemActive()) return;
    const ImGuiIO& io = ImGui::GetIO();
    const bool shift = io.KeyShift;
    const intptr_t pageRows = std::max<intptr_t>(static_cast<intptr_t>(lastViewH_ / std::max(rowH_, 1.0f)) - 1, 1);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) MoveCursor(-1, shift);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) MoveCursor(1, shift);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) MoveCursor(-static_cast<intptr_t>(kRowBytes), shift);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) MoveCursor(static_cast<intptr_t>(kRowBytes), shift);
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) MoveCursor(-pageRows * static_cast<intptr_t>(kRowBytes), shift);
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) MoveCursor(pageRows * static_cast<intptr_t>(kRowBytes), shift);
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) MoveCursor(-static_cast<intptr_t>(cursor_ % kRowBytes), shift);
    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) MoveCursor(static_cast<intptr_t>(kRowBytes - 1 - cursor_ % kRowBytes), shift);
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) asciiFocus_ = !asciiFocus_;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      pendingNibble_ = -1;
      anchor_ = cursor_;
    }
    if (io.KeyCtrl) {
      if (ImGui::IsKeyPressed(ImGuiKey_C, false)) CopySelectionHex();
      if (ImGui::IsKeyPressed(ImGuiKey_G, false)) focusBar_ = true;
      return;   // Ctrl+<key> also produces control characters; never treat them as edits
    }
    for (ImWchar ch : io.InputQueueCharacters) {
      if (asciiFocus_) {
        if (ch >= 0x20 && ch < 0x7F) WriteByteAtCursor(static_cast<uint8_t>(ch));
        continue;
      }
      int v = -1;
      if (ch >= '0' && ch <= '9') v = ch - '0';
      else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
      else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
      if (v < 0) continue;
      if (pendingNibble_ < 0) {
        pendingNibble_ = v;
      } else {
        const uint8_t value = static_cast<uint8_t>((pendingNibble_ << 4) | v);
        pendingNibble_ = -1;
        anchor_ = cursor_;
        WriteByteAtCursor(value);
      }
    }
  }

  std::string SelectionHex() const {
    const size_t n = std::min(SelLen(), kMaxCopyBytes);
    std::string out;
    out.reserve(n * 3);
    uint8_t b = 0;
    for (size_t i = 0; i < n; ++i) {
      if (i) out.push_back(' ');
      if (mem::ReadRaw(SelLo() + i, &b, 1)) {
        char hex[3];
        std::snprintf(hex, sizeof hex, "%02X", b);
        out += hex;
      } else {
        out += "??";
      }
    }
    return out;
  }

  void CopySelectionHex() {
    if (!hasSel_) return;
    tools::CopyToClipboard(SelectionHex(), SelLen() > kMaxCopyBytes ? "Bytes copied (first 64 KiB)" : "Bytes copied");
  }

  void DrawByteContextMenu() {
    if (!ImGui::BeginPopup("##bytectx")) return;
    const size_t len = SelLen();
    tools::AddressMenuOptions o;
    o.valueType = len >= 8 ? "i64" : len >= 4 ? "i32" : len >= 2 ? "i16" : "u8";
    o.size = static_cast<int>(std::clamp<size_t>(len, 1, 8));
    const uintptr_t a = len > 1 ? SelLo() : ctxAddr_;
    tools::AddressMenuItems(a, o);
    ImGui::Separator();
    if (ImGui::MenuItem("Copy selection as hex", "Ctrl+C")) CopySelectionHex();
    const std::optional<uint64_t> q = mem::Read<uint64_t>(AlignDown(ctxAddr_, 8));
    if (ImGui::MenuItem("Follow pointer", "Ctrl+click", false, q && mem::IsPlausiblePtr(*q))) FollowPointerAt(ctxAddr_);
    if (ImGui::MenuItem("Bookmark here")) AddBookmark(mem::Describe(a), a);
    if (ImGui::MenuItem("Make this the base address")) Navigate(a, true, false);
    ImGui::EndPopup();
  }

  // ------------------------------------------------------------------------------------------
  // Data inspector
  // ------------------------------------------------------------------------------------------
  void DrawInspector() {
    const auto& c = theme::Colors();
    SectionHeader("Inspector");
    if (!hasSel_) {
      ImGui::TextColored(c.textFaint, "Click a byte to inspect it.");
      return;
    }
    const uintptr_t a = SelLo();
    ImGui::TextUnformatted(mem::Describe(a).c_str());
    ImGui::TextColored(c.textDim, "%s selected", tools::FormatCount(SelLen()).c_str());

    uint8_t raw[8]{};
    size_t avail = 0;
    for (size_t n = sizeof raw; n > 0; --n) {
      if (mem::ReadRaw(a, raw, n)) {
        avail = n;
        break;
      }
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##insp", 2, flags)) {
      ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.0f);
      ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
      for (int i = 0; i < static_cast<int>(std::size(kInspectorTypes)); ++i) {
        const InspectorType& t = kInspectorTypes[i];
        const size_t size = mem::ValueSize(t.type);
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(c.textDim, "%s", t.label);
        ImGui::TableSetColumnIndex(1);
        if (inspEditRow_ == i) {
          DrawInspectorEditor(a, t.type);
        } else {
          tools::MonoFontScope mono;
          const std::string v = avail >= size ? mem::FormatValue(t.type, raw, size, false) : std::string("--");
          ImGui::AlignTextToFramePadding();
          ImGui::Selectable(v.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
          if (avail >= size && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            inspEditRow_ = i;
            inspEditText_ = v;
            inspEditFocus_ = true;
          }
          ImGui::SetItemTooltip("Double-click to edit");
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }

    SectionHeader("Pointer");
    if (avail >= 8) {
      uint64_t p = 0;
      std::memcpy(&p, raw, sizeof p);
      if (mem::IsPlausiblePtr(p) && mem::IsReadable(p, 1)) {
        if (ImGui::TextLink(mem::Describe(p).c_str())) Navigate(p, true, false);
        ImGui::SetItemTooltip("Follow pointer");
        const std::string cls = mem::rtti::ClassNameOf(p);
        if (!cls.empty()) ImGui::TextColored(c.accent, "%s", cls.c_str());
        if (std::string s = AsciiPreview(p, 64); !s.empty()) ImGui::TextWrapped("\"%s\"", s.c_str());
        else if (std::string w = WidePreview(p, 64); !w.empty()) ImGui::TextWrapped("L\"%s\"", w.c_str());
      } else {
        ImGui::TextColored(c.textFaint, "%s: not a pointer", util::Hex(p).c_str());
      }
    } else {
      ImGui::TextColored(c.textFaint, "--");
    }

    SectionHeader("String here");
    if (std::string s = AsciiPreview(a, 96); !s.empty()) ImGui::TextWrapped("\"%s\"", s.c_str());
    else if (std::string w = WidePreview(a, 96); !w.empty()) ImGui::TextWrapped("L\"%s\"", w.c_str());
    else ImGui::TextColored(c.textFaint, "--");

    const std::string cls = mem::rtti::ClassNameOf(a);
    if (!cls.empty()) {
      SectionHeader("Object");
      ImGui::TextColored(c.accent, "%s", cls.c_str());
      if (ImGui::SmallButton("Inspect struct")) InspectStruct(a);
    }
  }

  void DrawInspectorEditor(uintptr_t a, mem::ValueType type) {
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (inspEditFocus_) {
      ImGui::SetKeyboardFocusHere();
      inspEditFocus_ = false;
    }
    const bool enter = tools::InputString("##inspedit", &inspEditText_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    bool done = enter;
    if (!enter && ImGui::IsItemDeactivated()) done = true;
    const bool cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (!done && !cancel) {
      if (ImGui::IsItemActive()) inspInactiveFrames_ = 0;
      else if (++inspInactiveFrames_ > 3) inspEditRow_ = -1;
      return;
    }
    inspEditRow_ = -1;
    inspInactiveFrames_ = 0;
    if (cancel) return;
    std::vector<uint8_t> bytes;
    if (!mem::ParseValue(type, util::Trim(inspEditText_), bytes) || bytes.size() != mem::ValueSize(type)) {
      notify::Push(notify::Kind::Error, "Invalid value", std::string("Not a valid ") + mem::ValueTypeName(type));
      return;
    }
    if (!mem::WriteRaw(a, bytes.data(), bytes.size())) {
      notify::Push(notify::Kind::Error, "Write failed",
                   tools::FormatAddress(a) + (mp_guard::Blocked() ? ": blocked by the multiplayer guard" : " is not writable"));
    }
  }

  // ------------------------------------------------------------------------------------------
  // Disassembly tab
  // ------------------------------------------------------------------------------------------
  void StartDisasmJob() {
    jobAddr_ = disasmAddr_;
    jobCount_ = disasmCount_;
    const uintptr_t addr = disasmAddr_;
    const size_t count = disasmCount_;
    disasmJob_.Start([addr, count](AsyncDisasm::State& st) { st.result = BuildDisassembly(addr, count, st.cancel); });
  }

  void UpdateDisasm() {
    const uint64_t now = util::NowMs();
    if (disasmJob_.Ready()) {
      AsyncDisasm::State& st = disasmJob_.Get();
      disasmError_ = st.error;
      if (st.error.empty()) disasm_ = std::move(st.result);
      disasmBuiltFor_ = jobAddr_;
      disasmBuiltCount_ = jobCount_;
      disasmTimeMs_ = now;
      haveDisasm_ = true;
      disasmJob_.Cancel();
    }
    const bool wantNew = !haveDisasm_ || disasmBuiltFor_ != disasmAddr_ || disasmBuiltCount_ != disasmCount_;
    if (disasmJob_.Running()) {
      if (wantNew && (jobAddr_ != disasmAddr_ || jobCount_ != disasmCount_)) StartDisasmJob();
      return;
    }
    if (wantNew || now - disasmTimeMs_ > kDisasmRefreshMs) StartDisasmJob();
  }

  void DisasmNavigate(uintptr_t a, bool push) {
    if (push && a != disasmAddr_) {
      disasmBack_.push_back(disasmAddr_);
      if (disasmBack_.size() > kMaxHistory) disasmBack_.erase(disasmBack_.begin());
    }
    disasmAddr_ = a;
    disasmExpr_ = tools::FormatAddress(a);
    disasmCount_ = kDisasmBatch;
    disasmScrollTop_ = true;
  }

  void FollowTarget(const mem::Insn& in) {
    if (in.relBranch || mem::IsExecutable(in.target)) {
      DisasmNavigate(in.target, true);
    } else {
      Navigate(in.target, true, false);
      selectTab_ = kTabMemory;
    }
  }

  void DrawDisassemblyTab() {
    const auto& c = theme::Colors();
    UpdateDisasm();

    ImGui::BeginDisabled(disasmBack_.empty());
    if (ImGui::ArrowButton("##dback", ImGuiDir_Left)) {
      const uintptr_t prev = disasmBack_.back();
      disasmBack_.pop_back();
      DisasmNavigate(prev, false);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Back (branch history)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    uintptr_t target = 0;
    if (tools::AddressBar("##disaddr", &disasmExpr_, &target)) DisasmNavigate(target, true);
    ImGui::SameLine();
    if (ImGui::Button("From cursor") && hasSel_) DisasmNavigate(cursor_, true);
    ImGui::SameLine();
    if (ImGui::Button("Function start")) {
      if (auto fs = mem::FunctionStart(disasmAddr_)) DisasmNavigate(*fs, true);
      else notify::Push(notify::Kind::Warning, "Function start", "No unwind entry covers " + mem::Describe(disasmAddr_));
    }
    ImGui::SetItemTooltip("Jump to the start of the function containing this address (.pdata)");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (!mem::IsExecutable(disasmAddr_)) ImGui::TextColored(c.warning, "not executable memory");
    else ImGui::TextColored(c.textDim, "%s", mem::Describe(disasmAddr_).c_str());

    if (!disasmError_.empty()) ImGui::TextColored(c.danger, "%s", disasmError_.c_str());
    if (!haveDisasm_ || disasmBuiltFor_ != disasmAddr_) {
      ImGui::TextColored(c.textDim, "Decoding...");
      return;
    }
    if (disasm_.empty() || disasm_.front().insn.length == 0) {
      ImGui::TextColored(c.textFaint, "Nothing decodable at %s", tools::FormatAddress(disasmAddr_).c_str());
      return;
    }

    if (disasmScrollTop_) {
      ImGui::SetNextWindowScroll(ImVec2(-1.0f, 0.0f));
      disasmScrollTop_ = false;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##disasm", 4, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthStretch, 1.3f);
    ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthStretch, 2.2f);
    ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableHeadersRow();

    tools::MonoFontScope mono;
    const size_t rows = disasm_.size();
    const bool canMore = rows >= disasmCount_ && disasmCount_ < kDisasmMax && disasm_.back().insn.length != 0;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows + (canMore ? 1 : 0)));
    while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (static_cast<size_t>(i) == rows) {
          if (ImGui::SmallButton("Decode more")) disasmCount_ = std::min(disasmCount_ + kDisasmBatch, kDisasmMax);
          continue;
        }
        const DisasmLine& line = disasm_[static_cast<size_t>(i)];
        const mem::Insn& in = line.insn;
        ImGui::PushID(i);
        const bool isCurrent = in.address == disasmAddr_;
        if (isCurrent) ImGui::PushStyleColor(ImGuiCol_Text, c.accent);
        ImGui::Selectable(line.addrText.c_str(), hasSel_ && in.address <= cursor_ && cursor_ < in.address + std::max<uint8_t>(in.length, 1),
                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick);
        if (isCurrent) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && in.target) FollowTarget(in);
        if (ImGui::BeginPopupContextItem("##dctx")) {
          tools::AddressMenuOptions o;
          o.valueType = "bytes";
          o.size = 1;
          tools::AddressMenuItems(in.address, o);
          ImGui::Separator();
          if (ImGui::MenuItem("Copy instruction")) tools::CopyToClipboard(in.text, "Instruction copied");
          if (ImGui::MenuItem("Copy bytes")) tools::CopyToClipboard(line.bytesText, "Bytes copied");
          if (ImGui::MenuItem("Follow target", nullptr, false, in.target != 0)) FollowTarget(in);
          if (ImGui::MenuItem("Disassemble from here")) DisasmNavigate(in.address, true);
          ImGui::EndPopup();
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::TextColored(c.textDim, "%s", in.length ? line.bytesText.c_str() : "??");
        ImGui::TableSetColumnIndex(2);
        if (in.length == 0) ImGui::TextColored(c.danger, "(undecodable)");
        else ImGui::TextUnformatted(in.text.c_str());
        ImGui::TableSetColumnIndex(3);
        if (!line.targetText.empty()) {
          if (ImGui::TextLink(line.targetText.c_str())) FollowTarget(in);
          ImGui::SetItemTooltip(in.relBranch ? "Follow branch" : "Open target");
          if (!line.comment.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(c.success, "%s", line.comment.c_str());
          }
        }
        ImGui::PopID();
      }
    }
    ImGui::EndTable();
  }

  // ------------------------------------------------------------------------------------------
  // Bookmarks
  // ------------------------------------------------------------------------------------------
  void LoadBookmarks() {
    bookmarks_.clear();
    const json j = Config::Get().ReadJson(kBookmarksKey);
    if (!j.is_array()) return;
    for (const json& b : j) {
      try {
        if (!b.is_object()) continue;
        Bookmark bm;
        bm.name = b.value("name", std::string());
        bm.expr = b.value("expr", std::string());
        if (!bm.expr.empty()) bookmarks_.push_back(std::move(bm));
      } catch (const json::exception&) {
        continue;
      }
    }
  }

  void SaveBookmarks() const {
    json arr = json::array();
    for (const Bookmark& b : bookmarks_) {
      json o = json::object();
      o["name"] = b.name;
      o["expr"] = b.expr;
      arr.push_back(std::move(o));
    }
    Config::Get().WriteJson(kBookmarksKey, arr);
  }

  void AddBookmark(std::string name, uintptr_t a) {
    Bookmark b;
    b.name = name.empty() ? mem::Describe(a) : std::move(name);
    b.expr = tools::AddressToExpr(a);
    bookmarks_.push_back(std::move(b));
    SaveBookmarks();
    notify::Push(notify::Kind::Success, "Bookmark added", bookmarks_.back().name, 2.0f);
  }

  void DrawBookmarksTab() {
    const auto& c = theme::Colors();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    const bool enter = tools::InputString("##bmname", &newBookmarkName_, ImGuiInputTextFlags_EnterReturnsTrue, "Bookmark name");
    ImGui::SameLine();
    if (ImGui::Button("Add current address") || enter) {
      AddBookmark(util::Trim(newBookmarkName_), base_);
      newBookmarkName_.clear();
    }
    ImGui::SameLine();
    ImGui::TextColored(c.textFaint, "Stored as module-relative expressions, so they survive restarts.");

    int removeAt = -1;
    bool changed = false;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##bookmarks", 3, flags)) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.4f);
      ImGui::TableSetupColumn("Expression", ImGuiTableColumnFlags_WidthStretch, 1.6f);
      ImGui::TableSetupColumn("Resolved", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableHeadersRow();
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(bookmarks_.size()));
      while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
          Bookmark& b = bookmarks_[static_cast<size_t>(i)];
          const mem::ExprResult r = tools::Eval(b.expr);
          ImGui::PushID(i);
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::Selectable(b.name.empty() ? "(unnamed)" : b.name.c_str(), false,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
          if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && r.ok) {
            Navigate(r.value, true, false);
            selectTab_ = kTabMemory;
          }
          if (ImGui::BeginPopupContextItem("##bmctx")) {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
            changed |= tools::InputString("Name", &b.name);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
            changed |= tools::InputString("Expression", &b.expr);
            ImGui::Separator();
            if (r.ok) {
              if (ImGui::MenuItem("Go to")) {
                Navigate(r.value, true, false);
                selectTab_ = kTabMemory;
              }
              if (ImGui::BeginMenu("Address")) {
                tools::AddressMenuItems(r.value);
                ImGui::EndMenu();
              }
            }
            if (ImGui::MenuItem("Delete")) removeAt = i;
            ImGui::EndPopup();
          }
          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(b.expr.c_str());
          ImGui::TableSetColumnIndex(2);
          if (r.ok) {
            tools::MonoFontScope mono;
            ImGui::TextUnformatted(tools::FormatAddress(r.value).c_str());
          } else {
            ImGui::TextColored(c.danger, "??");
            ImGui::SetItemTooltip("%s", r.error.c_str());
          }
          ImGui::PopID();
        }
      }
      ImGui::EndTable();
    }
    if (bookmarks_.empty()) ImGui::TextColored(c.textFaint, "No bookmarks. Right-click a bookmark to rename, edit or delete it.");
    if (removeAt >= 0 && static_cast<size_t>(removeAt) < bookmarks_.size()) {
      bookmarks_.erase(bookmarks_.begin() + removeAt);
      changed = true;
    }
    if (changed) SaveBookmarks();
  }

  using AsyncDisasm = tools::AsyncJob<std::vector<DisasmLine>>;

  // Navigation
  bool hasBase_ = false;
  uintptr_t base_ = 0;
  std::string expr_;
  std::vector<uintptr_t> back_, fwd_;
  bool focusBar_ = false;
  int selectTab_ = -1;
  uint64_t frame_ = 0;

  // Grid
  uintptr_t spanStart_ = 0, spanEnd_ = 0;
  uintptr_t scrollRow_ = 0;
  ScrollMode scrollMode_ = ScrollMode::None;
  float rowH_ = 0, lastScrollY_ = 0, lastViewH_ = 0;
  bool relative_ = false;
  std::unordered_map<uintptr_t, RowCache> rows_;
  std::vector<uint8_t> block_;

  // Selection / editing
  bool hasSel_ = false;
  uintptr_t anchor_ = 0, cursor_ = 0, ctxAddr_ = 0;
  bool dragging_ = false, asciiFocus_ = false;
  int pendingNibble_ = -1;
  int inspEditRow_ = -1, inspInactiveFrames_ = 0;
  bool inspEditFocus_ = false;
  std::string inspEditText_;

  // Disassembly
  uintptr_t disasmAddr_ = 0;
  std::string disasmExpr_;
  size_t disasmCount_ = kDisasmBatch;
  std::vector<DisasmLine> disasm_;
  std::string disasmError_;
  bool haveDisasm_ = false, disasmScrollTop_ = false;
  uintptr_t disasmBuiltFor_ = 0, jobAddr_ = 0;
  size_t disasmBuiltCount_ = 0, jobCount_ = 0;
  uint64_t disasmTimeMs_ = 0;
  std::vector<uintptr_t> disasmBack_;
  AsyncDisasm disasmJob_;

  // Bookmarks
  std::vector<Bookmark> bookmarks_;
  std::string newBookmarkName_;
};

}  // namespace

std::unique_ptr<Window> MakeHexViewWindow() { return std::make_unique<HexViewWindow>(); }

}  // namespace cg::ui
