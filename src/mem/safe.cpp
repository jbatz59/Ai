#include "mem/safe.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <mutex>
#include <vector>

#include <windows.h>

#include "core/log.h"
#include "core/mp_guard.h"
#include "mem/basic_internal.h"

namespace cg::mem {
namespace {

using detail::kPageSize;
using detail::kUserMax;
using detail::kUserMin;

// Per-thread cache of recently queried *accessible* regions. Only positive results are cached and
// they expire quickly, so a stale entry can at worst let a read reach ReadProcessMemory, which still
// fails cleanly on memory that has since been freed or protected.
constexpr size_t kCacheSlots = 8;
constexpr uint64_t kCacheTtlMs = 250;
constexpr size_t kMaxStringChars = 1u << 20;   // hard cap for ReadCString/ReadWString
constexpr uint64_t kBlockedWarnIntervalMs = 5000;

struct CachedRegion {
  uintptr_t begin;
  uintptr_t end;
  DWORD protect;
  uint32_t gen;
  uint64_t stampMs;
};

std::atomic<uint32_t> g_cacheGen{1};
thread_local CachedRegion t_cache[kCacheSlots];
thread_local uint32_t t_cacheNext;

struct Region {
  uintptr_t begin = 0;
  uintptr_t end = 0;
  DWORD protect = 0;   // 0 when not committed
};

bool QueryUncached(uintptr_t addr, Region& r) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
  r.begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
  r.end = r.begin + mbi.RegionSize;
  r.protect = mbi.State == MEM_COMMIT ? mbi.Protect : 0;
  return r.end > addr;
}

bool Query(uintptr_t addr, Region& r) {
  const uint32_t gen = g_cacheGen.load(std::memory_order_relaxed);
  const uint64_t now = GetTickCount64();
  for (const CachedRegion& e : t_cache) {
    if (e.gen == gen && addr >= e.begin && addr < e.end && now - e.stampMs < kCacheTtlMs) {
      r = {e.begin, e.end, e.protect};
      return true;
    }
  }
  if (!QueryUncached(addr, r)) return false;
  if (detail::ProtReadable(r.protect) || detail::ProtWritable(r.protect) || detail::ProtExecutable(r.protect))
    t_cache[t_cacheNext++ % kCacheSlots] = {r.begin, r.end, r.protect, gen, now};
  return true;
}

bool SpanInUserRange(uintptr_t addr, size_t size) {
  return addr >= kUserMin && addr < kUserMax && size <= kUserMax - addr;
}

template <class Pred> bool SpanOk(uintptr_t addr, size_t size, Pred pred) {
  if (size == 0) return true;
  if (!SpanInUserRange(addr, size)) return false;
  const uintptr_t end = addr + size;
  for (uintptr_t cur = addr; cur < end;) {
    Region r;
    if (!Query(cur, r) || !pred(r.protect)) return false;
    cur = r.end;
  }
  return true;
}

void WarnBlocked(uintptr_t addr, size_t size) {
  static std::atomic<uint64_t> lastWarn{0};
  const uint64_t now = GetTickCount64();
  uint64_t prev = lastWarn.load(std::memory_order_relaxed);
  if (prev != 0 && now - prev < kBlockedWarnIntervalMs) return;
  if (!lastWarn.compare_exchange_strong(prev, now == 0 ? 1 : now)) return;
  try {
    log::Warn("mem", "write of {} byte(s) at 0x{:X} refused (multiplayer guard): {}", size, addr, mp_guard::Reason());
  } catch (...) {
  }
}

// Serialises writes that temporarily change page protection, so two writers to the same page can
// never restore each other's temporary protection and leave a page writable/executable.
std::mutex g_protectMutex;

struct ProtectSpan {
  uintptr_t begin;
  size_t size;
  DWORD protect;
};

}  // namespace

bool IsReadable(uintptr_t addr, size_t size) { return SpanOk(addr, size, detail::ProtReadable); }
bool IsWritable(uintptr_t addr, size_t size) { return SpanOk(addr, size, detail::ProtWritable); }
bool IsExecutable(uintptr_t addr) { return SpanOk(addr, 1, detail::ProtExecutable); }

bool ReadRaw(uintptr_t addr, void* out, size_t size) {
  if (size == 0) return true;
  if (!out || !SpanOk(addr, size, detail::ProtReadable)) return false;
  // ReadProcessMemory on our own process reports faults as a failure instead of raising them, so a
  // region freed between the check above and the copy cannot crash the game.
  SIZE_T got = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, size, &got) && got == size;
}

bool WriteRaw(uintptr_t addr, const void* in, size_t size) {
  if (size == 0) return true;
  if (!in) return false;
  if (mp_guard::Blocked()) {
    WarnBlocked(addr, size);
    return false;
  }
  if (!SpanInUserRange(addr, size)) return false;

  std::lock_guard lock(g_protectMutex);

  // Fresh (uncached) region info: the original protection must be exact to restore it.
  std::vector<ProtectSpan> spans;
  bool allWritable = true;
  bool anyExec = false;
  const uintptr_t end = addr + size;
  for (uintptr_t cur = addr; cur < end;) {
    Region r;
    if (!QueryUncached(cur, r)) return false;
    // Uncommitted, PAGE_NOACCESS and PAGE_GUARD memory are never written: they are deliberate
    // tripwires (stack guards, anti-tamper) or simply not there.
    if (r.protect == 0 || (r.protect & PAGE_GUARD) || (r.protect & 0xFF) == PAGE_NOACCESS) return false;
    const uintptr_t segEnd = std::min(end, r.end);
    spans.push_back({cur, static_cast<size_t>(segEnd - cur), r.protect});
    allWritable = allWritable && detail::ProtWritable(r.protect);
    anyExec = anyExec || detail::ProtExecutable(r.protect);
    cur = segEnd;
  }

  std::vector<ProtectSpan> changed;
  auto restore = [&changed] {
    for (const ProtectSpan& s : changed) {
      DWORD ignored = 0;
      VirtualProtect(reinterpret_cast<LPVOID>(s.begin), s.size, s.protect, &ignored);
    }
  };

  if (!allWritable) {
    changed.reserve(spans.size());
    for (const ProtectSpan& s : spans) {
      if (detail::ProtWritable(s.protect)) continue;
      const DWORD modifiers = s.protect & (PAGE_NOCACHE | PAGE_WRITECOMBINE);
      const DWORD wanted = (detail::ProtExecutable(s.protect) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE) | modifiers;
      DWORD old = 0;
      if (!VirtualProtect(reinterpret_cast<LPVOID>(s.begin), s.size, wanted, &old)) {
        restore();
        return false;
      }
      changed.push_back({s.begin, s.size, old});
    }
  }

  SIZE_T written = 0;
  const bool ok =
      WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(addr), in, size, &written) && written == size;
  restore();
  if (anyExec) FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), size);
  return ok;
}

void InvalidateRegionCache() { g_cacheGen.fetch_add(1, std::memory_order_relaxed); }

std::optional<uintptr_t> ReadPtr(uintptr_t addr) {
  uintptr_t v = 0;
  if (!ReadRaw(addr, &v, sizeof(v)) || !IsPlausiblePtr(v)) return std::nullopt;
  return v;
}

std::optional<uintptr_t> Deref(uintptr_t base, std::span<const int64_t> offsets) {
  if (offsets.empty()) return base;
  uintptr_t addr = base;
  for (size_t i = 0; i + 1 < offsets.size(); ++i) {
    const auto next = ReadPtr(addr + static_cast<uintptr_t>(offsets[i]));
    if (!next) return std::nullopt;
    addr = *next;
  }
  return addr + static_cast<uintptr_t>(offsets.back());
}

namespace {

// Reads NUL-terminated units of type C in page-bounded chunks so a string that ends right before
// an unreadable page is still returned in full.
template <class C> std::basic_string<C> ReadTerminated(uintptr_t addr, size_t maxLen) {
  std::basic_string<C> out;
  maxLen = std::min(maxLen, kMaxStringChars);
  C buf[64];
  uintptr_t cur = addr;
  while (out.size() < maxLen) {
    const size_t toPage = (kPageSize - (cur & (kPageSize - 1))) / sizeof(C);
    size_t n = std::min({std::size(buf), maxLen - out.size(), toPage});
    if (n == 0) n = 1;   // a unit straddling the page boundary
    if (!ReadRaw(cur, buf, n * sizeof(C))) break;
    const C* nul = std::find(buf, buf + n, C{});
    out.append(buf, static_cast<size_t>(nul - buf));
    if (nul != buf + n) break;
    cur += n * sizeof(C);
  }
  return out;
}

}  // namespace

std::string ReadCString(uintptr_t addr, size_t maxLen) { return ReadTerminated<char>(addr, maxLen); }
std::wstring ReadWString(uintptr_t addr, size_t maxLen) { return ReadTerminated<wchar_t>(addr, maxLen); }

}  // namespace cg::mem
