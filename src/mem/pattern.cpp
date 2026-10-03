#include "mem/pattern.h"

#include <algorithm>
#include <cstring>
#include <format>

#include <windows.h>

#include "mem/basic_internal.h"
#include "mem/disasm.h"
#include "mem/safe.h"

namespace cg::mem {
namespace {

using detail::kPageSize;
using detail::kUserMax;
using detail::kUserMin;

constexpr size_t kChunk = 1u << 20;
constexpr size_t kMaxPatternBytes = 4096;
constexpr size_t kMaxRipInsnLen = 15;
constexpr size_t kMaxFunctionSweep = 64 * 1024;   // FindXrefs disambiguation via .pdata
constexpr size_t kVoteWindow = 64;

bool Cancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(std::memory_order_relaxed); }

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// How common a byte is in x64 code and data; the scan anchors memchr on the least common exact byte.
int Commonness(uint8_t b) {
  switch (b) {
    case 0x00: return 100;
    case 0xFF: case 0xCC: case 0x48: return 90;
    case 0x8B: case 0x89: case 0x24: case 0x0F: return 80;
    case 0x4C: case 0x44: case 0x8D: case 0x90: case 0x01: case 0x85: case 0xC0: case 0xE8: case 0x83: return 70;
    case 0x10: case 0x20: case 0x08: case 0x40: case 0x74: case 0x75: case 0xC3: case 0x41: case 0x49: case 0x4D:
    case 0x33: case 0xD2: case 0xC9: case 0x45: case 0x05: case 0x0D: case 0x15: case 0x18: case 0x28: case 0x30:
    case 0x38: case 0x50: case 0x80: case 0xE9: case 0xEB: case 0x84: case 0x04: case 0x02: case 0x03: case 0xF8:
    case 0x5C: case 0x54: case 0x4E: case 0x0C: case 0xC7: case 0x8E: return 50;
    default: return 10;
  }
}

int ChooseAnchor(const Pattern& p) {
  int best = -1;
  int bestScore = 1000;
  for (size_t i = 0; i < p.Size(); ++i) {
    if (p.mask[i] != 0xFF) continue;
    const int score = Commonness(p.bytes[i]);
    if (score < bestScore) {
      bestScore = score;
      best = static_cast<int>(i);
    }
  }
  return best;
}

bool PatternValid(const Pattern& p) { return !p.bytes.empty() && p.bytes.size() == p.mask.size(); }

// Calls fn(runBegin, runEnd) for each maximal run of contiguous committed readable memory inside
// [start, end). Returns false if fn or the cancel flag stopped the walk.
template <class F> bool ForEachReadableRun(uintptr_t start, uintptr_t end, const std::atomic<bool>* cancel, F&& fn) {
  uintptr_t runBegin = 0, runEnd = 0;
  bool inRun = false;
  for (uintptr_t cur = start; cur < end;) {
    if (Cancelled(cancel)) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) != sizeof(mbi)) break;
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (regionEnd <= cur) break;
    const uintptr_t segEnd = std::min(end, regionEnd);
    if (mbi.State == MEM_COMMIT && detail::ProtReadable(mbi.Protect)) {
      if (inRun && runEnd == cur) {
        runEnd = segEnd;
      } else {
        if (inRun && !fn(runBegin, runEnd)) return false;
        runBegin = cur;
        runEnd = segEnd;
        inRun = true;
      }
    } else if (inRun) {
      if (!fn(runBegin, runEnd)) return false;
      inRun = false;
    }
    cur = segEnd;
  }
  return !inRun || fn(runBegin, runEnd);
}

// Copies readable memory in [start, end) chunk by chunk through ReadRaw and calls
// fn(data, len, limit, base): `data` holds `len` bytes starting at `base`; positions < limit belong
// to this call (the remaining `overlap` bytes let a match starting there run into the next chunk).
template <class F>
bool ForEachChunk(uintptr_t start, uintptr_t end, size_t overlap, const std::atomic<bool>* cancel, F&& fn) {
  std::vector<uint8_t> buf(kChunk + overlap);
  return ForEachReadableRun(start, end, cancel, [&](uintptr_t runBegin, uintptr_t runEnd) {
    for (uintptr_t pos = runBegin; pos < runEnd; pos += std::min<uintptr_t>(kChunk, runEnd - pos)) {
      if (Cancelled(cancel)) return false;
      const size_t len = static_cast<size_t>(std::min<uintptr_t>(kChunk + overlap, runEnd - pos));
      if (ReadRaw(pos, buf.data(), len)) {
        if (!fn(buf.data(), len, kChunk, pos)) return false;
        continue;
      }
      // The region changed under us (freed / re-protected): salvage whatever pages are still readable.
      size_t pieceBegin = 0, off = 0;
      auto flush = [&]() {
        if (off > pieceBegin && pieceBegin < kChunk)
          return fn(buf.data() + pieceBegin, off - pieceBegin, kChunk - pieceBegin, pos + pieceBegin);
        return true;
      };
      while (off < len) {
        const size_t n = std::min(kPageSize - static_cast<size_t>((pos + off) & (kPageSize - 1)), len - off);
        if (ReadRaw(pos + off, buf.data() + off, n)) {
          off += n;
          continue;
        }
        if (!flush()) return false;
        off += n;
        pieceBegin = off;
      }
      if (!flush()) return false;
    }
    return true;
  });
}

// Finds matches starting at positions [0, limit) of data[0, len). onMatch(addr) returns false to stop.
template <class F>
bool ScanBuffer(const Pattern& p, int anchor, const uint8_t* data, size_t len, size_t limit, uintptr_t base, F&& onMatch) {
  const size_t n = p.Size();
  if (len < n) return true;
  const size_t endStart = std::min(limit, len - n + 1);   // exclusive
  if (anchor < 0) {
    for (size_t i = 0; i < endStart; ++i)
      if (p.Matches(data + i) && !onMatch(base + i)) return false;
    return true;
  }
  const size_t a = static_cast<size_t>(anchor);
  const uint8_t ab = p.bytes[a];
  for (size_t i = 0; i < endStart;) {
    const void* hit = std::memchr(data + i + a, ab, endStart - i);
    if (!hit) break;
    const size_t s = static_cast<size_t>(static_cast<const uint8_t*>(hit) - data) - a;
    if (p.Matches(data + s) && !onMatch(base + s)) return false;
    i = s + 1;
  }
  return true;
}

// Clamps [start, start+size) to user space. Returns false when nothing is left.
bool ClampRange(uintptr_t start, size_t size, uintptr_t& b, uintptr_t& e) {
  if (start >= kUserMax || size == 0) return false;
  b = std::max(start, kUserMin);
  e = size > kUserMax - start ? kUserMax : start + size;
  return b < e;
}

template <class F> bool ScanRange(const Pattern& p, uintptr_t b, uintptr_t e, const std::atomic<bool>* cancel, F&& onMatch) {
  if (!PatternValid(p) || e <= b || e - b < p.Size()) return true;
  const int anchor = ChooseAnchor(p);
  return ForEachChunk(b, e, p.Size() - 1, cancel, [&](const uint8_t* data, size_t len, size_t limit, uintptr_t base) {
    return ScanBuffer(p, anchor, data, len, limit, base, onMatch);
  });
}

std::vector<uint8_t> Utf8ToUtf16Bytes(std::string_view s) {
  std::vector<uint8_t> out;
  if (s.empty() || s.size() > static_cast<size_t>(INT32_MAX)) return out;
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return out;
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n);
  out.resize(w.size() * 2);
  for (size_t i = 0; i < w.size(); ++i) {
    out[2 * i] = static_cast<uint8_t>(w[i] & 0xFF);
    out[2 * i + 1] = static_cast<uint8_t>((w[i] >> 8) & 0xFF);
  }
  return out;
}

// ---- FindXrefs helpers -------------------------------------------------------------------------

struct RuntimeFunction {
  uint32_t begin, end, unwind;
};

// Start of the .pdata entry (primary or chained chunk) covering `addr`; always an instruction start.
std::optional<uintptr_t> PdataChunkBegin(uintptr_t addr) {
  DWORD64 imageBase = 0;
  PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(static_cast<DWORD64>(addr), &imageBase, nullptr);
  if (!rf || !imageBase) return std::nullopt;
  RuntimeFunction entry{};
  if (!ReadRaw(reinterpret_cast<uintptr_t>(rf), &entry, sizeof(entry))) return std::nullopt;
  return static_cast<uintptr_t>(imageBase) + entry.begin;
}

bool IsXrefInsn(const Insn& insn, uintptr_t target, size_t dispPos) {
  if (insn.length == 0 || insn.target != target) return false;
  if (insn.ripRelative && insn.dispSize == 4 && insn.dispOffset == dispPos) return true;
  return insn.relBranch && insn.immSize == 4 && insn.immOffset == dispPos;
}

// Linear sweep over data[from..) (data starts at `base`); returns the first candidate it lands on.
std::optional<uintptr_t> SweepTo(const uint8_t* data, size_t len, uintptr_t base, uintptr_t from,
                                 const std::vector<uintptr_t>& cands) {
  const uintptr_t last = cands.back();
  uintptr_t cur = from;
  while (cur <= last) {
    if (std::find(cands.begin(), cands.end(), cur) != cands.end()) return cur;
    Insn insn;
    if (cur < base || cur - base >= len) return std::nullopt;
    if (!detail::DecodeBytes(data + (cur - base), len - (cur - base), cur, insn, false)) return std::nullopt;
    cur += insn.length;
  }
  return std::nullopt;
}

// Several instruction starts can place a valid displacement at the same position (e.g. "48 8B 05"
// and "8B 05"). Prefer the one a linear sweep from the enclosing .pdata entry reaches; otherwise the
// one most short sweeps from the preceding bytes agree on.
uintptr_t ChooseXrefStart(const std::vector<uintptr_t>& cands, const uint8_t* win, size_t winLen, uintptr_t winBase,
                          const Section& sec) {
  if (cands.size() == 1) return cands.front();
  const uintptr_t first = cands.front();

  if (auto chunk = PdataChunkBegin(first); chunk && *chunk >= sec.start && *chunk <= first &&
                                           first - *chunk <= kMaxFunctionSweep) {
    const uintptr_t hi = std::min<uintptr_t>(sec.start + sec.size, cands.back() + kMaxRipInsnLen);
    std::vector<uint8_t> body(static_cast<size_t>(hi - *chunk));
    if (ReadRaw(*chunk, body.data(), body.size()))
      if (auto hit = SweepTo(body.data(), body.size(), *chunk, *chunk, cands)) return *hit;
  }

  std::vector<int> votes(cands.size(), 0);
  for (uintptr_t q = winBase; q < first; ++q) {
    if (auto hit = SweepTo(win, winLen, winBase, q, cands))
      ++votes[static_cast<size_t>(std::find(cands.begin(), cands.end(), *hit) - cands.begin())];
  }
  size_t best = 0;   // candidates are ascending: ties favour the longest (earliest) start
  for (size_t i = 1; i < cands.size(); ++i)
    if (votes[i] > votes[best]) best = i;
  return cands[best];
}

// Validates a candidate displacement at `p` and returns the instruction start that owns it.
std::optional<uintptr_t> ValidateXref(uintptr_t p, uintptr_t target, const Section& sec) {
  const uintptr_t secEnd = sec.start + sec.size;
  uintptr_t lo = p - std::min<uintptr_t>(p - sec.start, kVoteWindow);
  const uintptr_t hi = std::min<uintptr_t>(secEnd, p + 4 + 8);
  uint8_t win[kVoteWindow + 16];
  if (!ReadRaw(lo, win, static_cast<size_t>(hi - lo))) {
    lo = std::max(lo, p & ~static_cast<uintptr_t>(kPageSize - 1));
    if (!ReadRaw(lo, win, static_cast<size_t>(hi - lo))) return std::nullopt;
  }
  const size_t winLen = static_cast<size_t>(hi - lo);

  std::vector<uintptr_t> cands;
  for (size_t k = 11; k >= 1; --k) {   // ascending addresses
    if (p - lo < k) continue;
    const uintptr_t s = p - k;
    Insn insn;
    if (detail::DecodeBytes(win + (s - lo), static_cast<size_t>(hi - s), s, insn, false) && IsXrefInsn(insn, target, k))
      cands.push_back(s);
  }
  if (cands.empty()) return std::nullopt;
  return ChooseXrefStart(cands, win, winLen, lo, sec);
}

}  // namespace

std::optional<Pattern> Pattern::Parse(std::string_view ida) {
  Pattern p;
  size_t i = 0;
  while (i < ida.size()) {
    const char c = ida[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < ida.size() && ida[j] != ' ' && ida[j] != '\t' && ida[j] != '\r' && ida[j] != '\n') ++j;
    const std::string_view tok = ida.substr(i, j - i);
    i = j;
    if (p.bytes.size() >= kMaxPatternBytes) return std::nullopt;

    if (tok == "?" || tok == "??") {
      p.bytes.push_back(0);
      p.mask.push_back(0);
      continue;
    }
    if (tok.size() != 2) return std::nullopt;
    const int hi = tok[0] == '?' ? -2 : HexDigit(tok[0]);
    const int lo = tok[1] == '?' ? -2 : HexDigit(tok[1]);
    if (hi == -1 || lo == -1) return std::nullopt;
    const uint8_t mask = static_cast<uint8_t>((hi >= 0 ? 0xF0 : 0) | (lo >= 0 ? 0x0F : 0));
    const uint8_t value = static_cast<uint8_t>(((hi >= 0 ? hi : 0) << 4) | (lo >= 0 ? lo : 0));
    p.bytes.push_back(value);
    p.mask.push_back(mask);
  }
  if (p.bytes.empty()) return std::nullopt;
  return p;
}

std::string Pattern::ToString() const {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string s;
  s.reserve(bytes.size() * 3);
  for (size_t i = 0; i < bytes.size() && i < mask.size(); ++i) {
    if (i) s += ' ';
    const uint8_t m = mask[i];
    const uint8_t b = bytes[i];
    s += (m & 0xF0) == 0xF0 ? kHex[b >> 4] : '?';
    s += (m & 0x0F) == 0x0F ? kHex[b & 0xF] : '?';
  }
  return s;
}

bool Pattern::Matches(const uint8_t* p) const {
  const size_t n = std::min(bytes.size(), mask.size());
  for (size_t i = 0; i < n; ++i)
    if ((p[i] & mask[i]) != (bytes[i] & mask[i])) return false;
  return true;
}

std::optional<uintptr_t> FindFirst(const Pattern& p, uintptr_t start, size_t size) {
  auto all = FindAll(p, start, size, 1);
  if (all.empty()) return std::nullopt;
  return all.front();
}

std::vector<uintptr_t> FindAll(const Pattern& p, uintptr_t start, size_t size, size_t maxResults,
                               const std::atomic<bool>* cancel) {
  std::vector<uintptr_t> out;
  uintptr_t b = 0, e = 0;
  if (maxResults == 0 || !PatternValid(p) || !ClampRange(start, size, b, e)) return out;
  try {
    ScanRange(p, b, e, cancel, [&](uintptr_t a) {
      out.push_back(a);
      return out.size() < maxResults;
    });
  } catch (...) {
    // Allocation failure: return what was found so far.
  }
  return out;
}

std::vector<uintptr_t> FindAllInModule(const Pattern& p, const Module& m, bool execOnly, size_t maxResults) {
  std::vector<uintptr_t> out;
  if (!m.Valid() || maxResults == 0) return out;
  auto scan = [&](uintptr_t start, size_t size) {
    if (out.size() >= maxResults) return;
    auto found = FindAll(p, start, size, maxResults - out.size());
    out.insert(out.end(), found.begin(), found.end());
  };
  if (m.sections.empty()) {
    if (!execOnly) scan(m.base, m.size);
    return out;
  }
  for (const Section& s : m.sections)
    if (!execOnly || s.Executable()) scan(s.start, s.size);
  return out;
}

std::optional<uintptr_t> FindInModule(const Pattern& p, const Module& m, bool execOnly) {
  auto all = FindAllInModule(p, m, execOnly, 1);
  if (all.empty()) return std::nullopt;
  return all.front();
}

std::optional<uintptr_t> ResolveRip(uintptr_t instr, int dispOffset, int instrLen) {
  if (dispOffset < 0 || instrLen < dispOffset + 4 || instrLen > static_cast<int>(kMaxRipInsnLen)) return std::nullopt;
  const auto disp = Read<int32_t>(instr + static_cast<uintptr_t>(dispOffset));
  if (!disp) return std::nullopt;
  return instr + static_cast<uintptr_t>(instrLen) + static_cast<uintptr_t>(static_cast<int64_t>(*disp));
}

std::optional<uintptr_t> ResolveCall(uintptr_t instr) {
  uint8_t code[5];
  if (!ReadRaw(instr, code, sizeof(code)) || (code[0] != 0xE8 && code[0] != 0xE9)) return std::nullopt;
  int32_t rel = 0;
  std::memcpy(&rel, code + 1, sizeof(rel));
  return instr + 5 + static_cast<uintptr_t>(static_cast<int64_t>(rel));
}

std::optional<uintptr_t> FindString(const Module& m, std::string_view text, bool wide) {
  if (!m.Valid() || text.empty() || text.find('\0') != std::string_view::npos) return std::nullopt;
  const size_t unit = wide ? 2 : 1;
  std::vector<uint8_t> needle = wide ? Utf8ToUtf16Bytes(text) : std::vector<uint8_t>(text.begin(), text.end());
  if (needle.empty() || needle.size() + unit > kMaxPatternBytes) return std::nullopt;
  needle.insert(needle.end(), unit, 0);
  Pattern p;
  p.bytes = std::move(needle);
  p.mask.assign(p.bytes.size(), 0xFF);

  std::optional<uintptr_t> found;
  try {
    for (const Section& s : m.sections) {
      if (s.Executable()) continue;
      uintptr_t b = 0, e = 0;
      if (!ClampRange(s.start, s.size, b, e)) continue;
      ScanRange(p, b, e, nullptr, [&](uintptr_t a) {
        if (wide && ((a - s.start) & 1)) return true;
        if (a != s.start) {
          uint8_t before[2] = {1, 1};
          if (!ReadRaw(a - unit, before, unit) || before[0] != 0 || (wide && before[1] != 0)) return true;
        }
        found = a;
        return false;
      });
      if (found) break;
    }
  } catch (...) {
    return std::nullopt;
  }
  return found;
}

std::vector<uintptr_t> FindXrefs(const Module& m, uintptr_t target, size_t maxResults, const std::atomic<bool>* cancel) {
  std::vector<uintptr_t> out;
  if (!m.Valid() || maxResults == 0 || !IsPlausiblePtr(target)) return out;
  constexpr uintptr_t kReach = (1ull << 31) + 16;   // rel32 range plus the longest trailing immediate
  try {
    for (const Section& sec : m.sections) {
      if (!sec.Executable() || Cancelled(cancel) || out.size() >= maxResults) continue;
      const uintptr_t secEnd = sec.start + sec.size;
      if ((target < sec.start && sec.start - target > kReach) || (target > secEnd && target - secEnd > kReach)) continue;
      uintptr_t b = 0, e = 0;
      if (!ClampRange(sec.start, sec.size, b, e)) continue;
      ForEachChunk(b, e, 3, cancel, [&](const uint8_t* data, size_t len, size_t limit, uintptr_t base) {
        if (len < 4) return true;
        const size_t endPos = std::min(limit, len - 3);
        for (size_t i = 0; i < endPos; ++i) {
          if ((i & 0xFFFF) == 0 && Cancelled(cancel)) return false;
          int32_t rel;
          std::memcpy(&rel, data + i, sizeof(rel));
          const uintptr_t p = base + i;
          // next-instruction address = p + 4 + size of any immediate after the displacement (0/1/2/4)
          const uintptr_t delta = target - (p + 4 + static_cast<uintptr_t>(static_cast<int64_t>(rel)));
          if (delta > 4 || delta == 3 || p == sec.start) continue;
          if (auto s = ValidateXref(p, target, sec)) {
            out.push_back(*s);
            if (out.size() >= maxResults) return false;
          }
        }
        return true;
      });
    }
  } catch (...) {
    // Allocation failure: keep whatever was collected.
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::optional<uintptr_t> FunctionStart(uintptr_t addr) {
  if (!IsPlausiblePtr(addr)) return std::nullopt;
  DWORD64 imageBase = 0;
  PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(static_cast<DWORD64>(addr), &imageBase, nullptr);
  if (!rf || !imageBase) return std::nullopt;
  RuntimeFunction cur{};
  if (!ReadRaw(reinterpret_cast<uintptr_t>(rf), &cur, sizeof(cur))) return std::nullopt;
  const uintptr_t base = static_cast<uintptr_t>(imageBase);

  constexpr uint32_t kRuntimeFunctionIndirect = 0x1;
  constexpr uint8_t kUnwFlagChainInfo = 0x4;
  for (int hop = 0; hop < 32; ++hop) {
    if (cur.unwind & kRuntimeFunctionIndirect) {
      // The entry shares another RUNTIME_FUNCTION's unwind data; follow it.
      RuntimeFunction next{};
      if (!ReadRaw(base + (cur.unwind & ~kRuntimeFunctionIndirect), &next, sizeof(next))) break;
      cur = next;
      continue;
    }
    uint8_t header[4];   // UNWIND_INFO: Version:3 Flags:5, SizeOfProlog, CountOfCodes, FrameRegister/Offset
    if (!ReadRaw(base + cur.unwind, header, sizeof(header))) break;
    if (!((header[0] >> 3) & kUnwFlagChainInfo)) break;
    const uintptr_t chained = base + cur.unwind + 4 + static_cast<uintptr_t>((header[2] + 1u) & ~1u) * 2;
    RuntimeFunction next{};
    if (!ReadRaw(chained, &next, sizeof(next))) break;
    cur = next;
  }
  return base + cur.begin;
}

}  // namespace cg::mem
