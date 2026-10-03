#include "mem/scanner.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <type_traits>

#include <windows.h>

#include "core/log.h"
#include "mem/safe.h"

namespace cg::mem {

const char* ScanOpName(ScanOp op) {
  switch (op) {
    case ScanOp::Exact: return "Exact value";
    case ScanOp::NotEqual: return "Not equal";
    case ScanOp::Bigger: return "Bigger than";
    case ScanOp::Smaller: return "Smaller than";
    case ScanOp::Between: return "Between";
    case ScanOp::Unknown: return "Unknown initial value";
    case ScanOp::Changed: return "Changed";
    case ScanOp::Unchanged: return "Unchanged";
    case ScanOp::Increased: return "Increased";
    case ScanOp::Decreased: return "Decreased";
    case ScanOp::IncreasedBy: return "Increased by";
    case ScanOp::DecreasedBy: return "Decreased by";
  }
  return "?";
}

bool ScanOpNeedsValue(ScanOp op) {
  switch (op) {
    case ScanOp::Exact: case ScanOp::NotEqual: case ScanOp::Bigger: case ScanOp::Smaller: case ScanOp::Between:
    case ScanOp::IncreasedBy: case ScanOp::DecreasedBy: return true;
    default: return false;
  }
}

bool ScanOpValidForFirst(ScanOp op) {
  switch (op) {
    case ScanOp::Exact: case ScanOp::NotEqual: case ScanOp::Bigger: case ScanOp::Smaller: case ScanOp::Between:
    case ScanOp::Unknown: return true;
    default: return false;
  }
}

namespace {

constexpr size_t kMaxResults = 20'000'000;
constexpr size_t kItemBytes = size_t{4} << 20;
constexpr size_t kItemEntries = size_t{1} << 18;
constexpr size_t kGroupSpan = size_t{64} << 10;
constexpr size_t kPage = 0x1000;
constexpr uintptr_t kUserMin = 0x10000;
constexpr uintptr_t kUserMax = 0x7FFFFFFF0000ull;

// Allocator backed directly by VirtualAlloc: the scanner's own buffers are created after the region
// walk, so they never show up as (self-)matches in the scanned memory.
template <class T> struct PageAlloc {
  using value_type = T;
  PageAlloc() noexcept = default;
  template <class U> PageAlloc(const PageAlloc<U>&) noexcept {}
  T* allocate(size_t n) {
    if (n > std::numeric_limits<size_t>::max() / sizeof(T)) throw std::bad_alloc();
    void* p = VirtualAlloc(nullptr, std::max<size_t>(n * sizeof(T), 1), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) throw std::bad_alloc();
    return static_cast<T*>(p);
  }
  void deallocate(T* p, size_t) noexcept {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
  }
  template <class U> bool operator==(const PageAlloc<U>&) const noexcept { return true; }
};
template <class T> using PVec = std::vector<T, PageAlloc<T>>;

struct NumRec {
  uintptr_t addr;
  uint64_t prev;
};
struct SnapRegion {
  uintptr_t start = 0;
  PVec<uint8_t> data;
};
struct Region {
  uintptr_t start;
  size_t size;
};
struct Slice {
  uintptr_t start, end, regionEnd;   // candidates in [start, end); readable data up to regionEnd
  size_t region = 0;                 // snapshot region index (snapshot scans)
};

bool ProtReadableRW(DWORD p, bool writableOnly) {
  if (p & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) return false;
  switch (p & 0xFF) {
    case PAGE_READWRITE: case PAGE_WRITECOPY: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return true;
    case PAGE_READONLY: case PAGE_EXECUTE_READ: return !writableOnly;
    default: return false;
  }
}

std::vector<Region> WalkRegions(const ScanParams& p) {
  std::vector<Region> out;
  const uintptr_t lo = std::max(p.rangeStart, kUserMin);
  const uintptr_t hi = std::min(p.rangeEnd, kUserMax);
  uintptr_t addr = lo;
  MEMORY_BASIC_INFORMATION mbi;
  while (addr < hi && VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t end = start + mbi.RegionSize;
    if (end <= addr) break;
    const bool typeOk = mbi.Type == MEM_PRIVATE || (p.includeImages && mbi.Type == MEM_IMAGE);
    if (mbi.State == MEM_COMMIT && typeOk && ProtReadableRW(mbi.Protect, p.writableOnly)) {
      const uintptr_t s = std::max(start, lo), e = std::min(end, hi);
      if (e > s) {
        if (!out.empty() && out.back().start + out.back().size == s) out.back().size += e - s;
        else out.push_back({s, e - s});
      }
    }
    addr = end;
  }
  return out;
}

unsigned WorkerCount() {
  const unsigned hc = std::thread::hardware_concurrency();
  return std::max(1u, hc > 1 ? hc - 1 : 1u);
}

template <class T> uint64_t Bits(T v) {
  uint64_t b = 0;
  std::memcpy(&b, &v, sizeof(T));
  return b;
}
template <class T> T Load(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

template <class T> struct Cmp {
  T a{}, b{};
  double lo = 0, hi = 0, tol = 0;
  bool rounded = false;
};

template <class T, ScanOp OP> inline bool Match(T cur, T prev, const Cmp<T>& c) {
  constexpr bool kFloat = std::is_floating_point_v<T>;
  if constexpr (OP == ScanOp::Exact || OP == ScanOp::NotEqual) {
    bool eq;
    if constexpr (kFloat) eq = c.rounded ? (static_cast<double>(cur) >= c.lo && static_cast<double>(cur) < c.hi) : cur == c.a;
    else eq = cur == c.a;
    return OP == ScanOp::Exact ? eq : !eq;
  } else if constexpr (OP == ScanOp::Bigger) {
    return cur > c.a;
  } else if constexpr (OP == ScanOp::Smaller) {
    return cur < c.a;
  } else if constexpr (OP == ScanOp::Between) {
    return cur >= c.a && cur <= c.b;
  } else if constexpr (OP == ScanOp::Changed) {
    return Bits(cur) != Bits(prev);
  } else if constexpr (OP == ScanOp::Unchanged) {
    return Bits(cur) == Bits(prev);
  } else if constexpr (OP == ScanOp::Increased) {
    return cur > prev;
  } else if constexpr (OP == ScanOp::Decreased) {
    return cur < prev;
  } else if constexpr (OP == ScanOp::IncreasedBy || OP == ScanOp::DecreasedBy) {
    if constexpr (kFloat) {
      const double expect = OP == ScanOp::IncreasedBy ? static_cast<double>(prev) + static_cast<double>(c.a)
                                                      : static_cast<double>(prev) - static_cast<double>(c.a);
      if (c.rounded) return std::fabs(static_cast<double>(cur) - expect) < c.tol;
      return cur == static_cast<T>(expect);
    } else {
      using U = std::make_unsigned_t<T>;
      const U e = OP == ScanOp::IncreasedBy ? static_cast<U>(static_cast<U>(prev) + static_cast<U>(c.a))
                                            : static_cast<U>(static_cast<U>(prev) - static_cast<U>(c.a));
      return static_cast<U>(cur) == e;
    }
  } else {
    return true;   // Unknown
  }
}

template <class F> bool WithType(ValueType t, F&& f) {
  switch (t) {
    case ValueType::I8: f(std::type_identity<int8_t>{}); return true;
    case ValueType::I16: f(std::type_identity<int16_t>{}); return true;
    case ValueType::I32: f(std::type_identity<int32_t>{}); return true;
    case ValueType::I64: f(std::type_identity<int64_t>{}); return true;
    case ValueType::U8: f(std::type_identity<uint8_t>{}); return true;
    case ValueType::U16: f(std::type_identity<uint16_t>{}); return true;
    case ValueType::U32: f(std::type_identity<uint32_t>{}); return true;
    case ValueType::U64: f(std::type_identity<uint64_t>{}); return true;
    case ValueType::F32: f(std::type_identity<float>{}); return true;
    case ValueType::F64: f(std::type_identity<double>{}); return true;
    default: return false;
  }
}

template <class F> void WithOp(ScanOp op, F&& f) {
  switch (op) {
    case ScanOp::Exact: f(std::integral_constant<ScanOp, ScanOp::Exact>{}); break;
    case ScanOp::NotEqual: f(std::integral_constant<ScanOp, ScanOp::NotEqual>{}); break;
    case ScanOp::Bigger: f(std::integral_constant<ScanOp, ScanOp::Bigger>{}); break;
    case ScanOp::Smaller: f(std::integral_constant<ScanOp, ScanOp::Smaller>{}); break;
    case ScanOp::Between: f(std::integral_constant<ScanOp, ScanOp::Between>{}); break;
    case ScanOp::Unknown: f(std::integral_constant<ScanOp, ScanOp::Unknown>{}); break;
    case ScanOp::Changed: f(std::integral_constant<ScanOp, ScanOp::Changed>{}); break;
    case ScanOp::Unchanged: f(std::integral_constant<ScanOp, ScanOp::Unchanged>{}); break;
    case ScanOp::Increased: f(std::integral_constant<ScanOp, ScanOp::Increased>{}); break;
    case ScanOp::Decreased: f(std::integral_constant<ScanOp, ScanOp::Decreased>{}); break;
    case ScanOp::IncreasedBy: f(std::integral_constant<ScanOp, ScanOp::IncreasedBy>{}); break;
    case ScanOp::DecreasedBy: f(std::integral_constant<ScanOp, ScanOp::DecreasedBy>{}); break;
  }
}

int Decimals(const std::string& text) {
  if (text.find("0x") != std::string::npos || text.find("0X") != std::string::npos) return -1;
  const size_t dot = text.find('.');
  if (dot == std::string::npos) return 0;
  int n = 0;
  for (size_t i = dot + 1; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) ++n;
  return std::min(n, 15);
}

// Reads the bytes needed for candidates of slice `s` and calls fn(data, dataAddr, dataLen, candEnd)
// for every readable piece (page-wise fallback when the whole slice cannot be read).
template <class F> void ReadSlice(const Slice& s, size_t valSize, uint8_t* buf, size_t bufSize, F&& fn) {
  const uintptr_t readEnd = std::min<uintptr_t>(s.end + valSize - 1, s.regionEnd);
  const size_t len = readEnd - s.start;
  if (len <= bufSize && ReadRaw(s.start, buf, len)) {
    fn(buf, s.start, len, s.end);
    return;
  }
  for (uintptr_t pg = s.start; pg < s.end; pg = (pg & ~(kPage - 1)) + kPage) {
    const uintptr_t pe = std::min<uintptr_t>((pg & ~(kPage - 1)) + kPage, s.end);
    const uintptr_t re = std::min<uintptr_t>(pe + valSize - 1, s.regionEnd);
    if (re - pg <= bufSize && ReadRaw(pg, buf, re - pg)) fn(buf, pg, re - pg, pe);
    else if (pe - pg <= bufSize && ReadRaw(pg, buf, pe - pg)) fn(buf, pg, pe - pg, pe);
  }
}

inline uintptr_t AlignUp(uintptr_t a, size_t align) { return (a + align - 1) & ~(static_cast<uintptr_t>(align) - 1); }

}  // namespace

struct Scanner::Impl {
  mutable std::mutex mu;
  std::thread master;
  std::atomic<bool> busy{false}, cancel{false};
  std::atomic<float> progress{0.0f};
  std::atomic<uint64_t> generation{0};
  std::atomic<uint64_t> snapshotLimit{uint64_t{2} << 30};
  std::string lastError;

  enum class Mode { None, Numeric, Addresses, Snapshot };
  // Results: written by the master thread under `mu` (swap at the end), read by Page() under `mu`.
  Mode mode = Mode::None;
  ValueType type = ValueType::I32;
  PVec<NumRec> num;
  PVec<uintptr_t> addrs;
  std::vector<uint8_t> pattern;   // String/WString/Bytes: last searched bytes
  std::vector<SnapRegion> snap;
  std::vector<uint64_t> snapPrefix;   // candidates before region i
  size_t snapAlign = 1;
  size_t count = 0;

  // Per-scan scratch.
  std::atomic<size_t> found{0};
  std::atomic<bool> truncated{false};
  std::atomic<bool> failed{false};

  void SetError(std::string e) {
    std::lock_guard lock(mu);
    lastError = std::move(e);
  }

  void ClearResultsLocked() {
    mode = Mode::None;
    num = {};
    addrs = {};
    pattern.clear();
    snap.clear();
    snapPrefix.clear();
    count = 0;
  }

  void JoinMaster() {
    if (master.joinable() && master.get_id() != std::this_thread::get_id()) master.join();
  }

  // Runs fn(item, worker) over `items` on the worker pool; weights drive Progress().
  template <class F> void RunParallel(size_t items, const std::vector<uint64_t>& weight, F&& fn) {
    uint64_t total = 0;
    for (uint64_t w : weight) total += w;
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> done{0};
    auto body = [&](unsigned worker) {
      for (;;) {
        if (cancel.load(std::memory_order_relaxed)) return;
        const size_t i = next.fetch_add(1);
        if (i >= items) return;
        try {
          fn(i, worker);
        } catch (...) {
          failed = true;
          cancel = true;
        }
        const uint64_t d = done.fetch_add(weight[i]) + weight[i];
        if (total) progress.store(static_cast<float>(static_cast<double>(d) / static_cast<double>(total)));
      }
    };
    const unsigned n = static_cast<unsigned>(std::min<size_t>(WorkerCount(), std::max<size_t>(items, 1)));
    std::vector<std::thread> threads;
    for (unsigned k = 1; k < n; ++k) {
      try {
        threads.emplace_back(body, k);
      } catch (...) {
        break;
      }
    }
    body(0);
    for (auto& t : threads) t.join();
  }

  bool Claim() {
    if (found.fetch_add(1, std::memory_order_relaxed) >= kMaxResults) {
      truncated = true;
      return false;
    }
    return true;
  }

  template <class V> static V Concat(std::vector<V>& parts) {
    size_t total = 0;
    for (auto& p : parts) total += p.size();
    V out;
    out.reserve(total);
    for (auto& p : parts) {
      out.insert(out.end(), p.begin(), p.end());
      p = V{};
    }
    return out;
  }

  static std::vector<Slice> MakeSlices(const std::vector<Region>& regions, std::vector<uint64_t>& weight) {
    std::vector<Slice> slices;
    for (const Region& r : regions) {
      for (size_t off = 0; off < r.size; off += kItemBytes) {
        const size_t n = std::min(kItemBytes, r.size - off);
        slices.push_back({r.start + off, r.start + off + n, r.start + r.size, 0});
        weight.push_back(n);
      }
    }
    return slices;
  }

  // ------------------------------------------------------------------------------------------
  void RunFirst(const ScanParams& p, std::vector<uint8_t> v1, std::vector<uint8_t> v2) {
    const std::vector<Region> regions = WalkRegions(p);
    if (p.op == ScanOp::Unknown) return RunSnapshot(p, regions);
    std::vector<uint64_t> weight;
    const std::vector<Slice> slices = MakeSlices(regions, weight);
    const unsigned workers = WorkerCount();
    std::vector<PVec<uint8_t>> bufs(workers);
    for (auto& b : bufs) b.resize(kItemBytes + 64);

    if (!IsNumeric(p.type)) {
      const size_t align = p.type == ValueType::WString && p.fastScan ? 2 : 1;
      std::vector<PVec<uintptr_t>> parts(slices.size());
      const size_t plen = v1.size();
      RunParallel(slices.size(), weight, [&](size_t i, unsigned w) {
        auto& out = parts[i];
        ReadSlice(slices[i], plen, bufs[w].data(), bufs[w].size(), [&](const uint8_t* d, uintptr_t at, size_t len, uintptr_t candEnd) {
          if (len < plen) return;
          for (uintptr_t a = AlignUp(at, align); a < candEnd && a + plen <= at + len; a += align) {
            const uint8_t* q = d + (a - at);
            if (*q != v1[0] || std::memcmp(q, v1.data(), plen) != 0) continue;
            if (!Claim()) return;
            out.push_back(a);
          }
        });
      });
      if (cancel) return;
      PVec<uintptr_t> merged = Concat(parts);
      std::lock_guard lock(mu);
      ClearResultsLocked();
      mode = Mode::Addresses;
      type = p.type;
      pattern = std::move(v1);
      count = merged.size();
      addrs = std::move(merged);
      return;
    }

    std::vector<PVec<NumRec>> parts(slices.size());
    WithType(p.type, [&](auto tid) {
      using T = typename decltype(tid)::type;
      const size_t vs = sizeof(T);
      const size_t align = p.fastScan ? std::min<size_t>(vs, 4) : 1;
      const Cmp<T> c = MakeCmp<T>(p, v1, v2);
      WithOp(p.op, [&](auto opc) {
        constexpr ScanOp OP = decltype(opc)::value;
        RunParallel(slices.size(), weight, [&](size_t i, unsigned w) {
          auto& out = parts[i];
          ReadSlice(slices[i], vs, bufs[w].data(), bufs[w].size(), [&](const uint8_t* d, uintptr_t at, size_t len, uintptr_t candEnd) {
            for (uintptr_t a = AlignUp(at, align); a < candEnd && a + vs <= at + len; a += align) {
              const T cur = Load<T>(d + (a - at));
              if (!Match<T, OP>(cur, cur, c)) continue;
              if (!Claim()) return;
              out.push_back({a, Bits(cur)});
            }
          });
        });
      });
    });
    if (cancel) return;
    PVec<NumRec> merged = Concat(parts);
    std::lock_guard lock(mu);
    ClearResultsLocked();
    mode = Mode::Numeric;
    type = p.type;
    count = merged.size();
    num = std::move(merged);
  }

  template <class T> static Cmp<T> MakeCmp(const ScanParams& p, const std::vector<uint8_t>& v1, const std::vector<uint8_t>& v2) {
    Cmp<T> c;
    if (v1.size() >= sizeof(T)) std::memcpy(&c.a, v1.data(), sizeof(T));
    if (v2.size() >= sizeof(T)) std::memcpy(&c.b, v2.data(), sizeof(T));
    if (p.op == ScanOp::Between && c.b < c.a) std::swap(c.a, c.b);
    if constexpr (std::is_floating_point_v<T>) {
      const int dec = Decimals(p.value);
      c.rounded = p.floatRounded && dec >= 0;
      if (c.rounded) {
        c.tol = 0.5 * std::pow(10.0, -dec);
        c.lo = static_cast<double>(c.a) - c.tol;
        c.hi = static_cast<double>(c.a) + c.tol;
      }
    }
    return c;
  }

  void RunSnapshot(const ScanParams& p, const std::vector<Region>& regions) {
    const size_t vs = ValueSize(p.type);
    const size_t align = p.fastScan ? std::min<size_t>(vs, 4) : 1;
    const uint64_t limit = snapshotLimit.load();
    uint64_t total = 0;
    size_t skipped = 0;
    std::vector<SnapRegion> out;
    for (const Region& r : regions) {
      if (r.size < vs) continue;
      if (total + r.size > limit) {
        ++skipped;
        continue;
      }
      SnapRegion s;
      s.start = r.start;
      s.data.resize(r.size);
      total += r.size;
      out.push_back(std::move(s));
    }
    std::vector<Slice> slices;
    std::vector<uint64_t> weight;
    for (size_t ri = 0; ri < out.size(); ++ri) {
      const SnapRegion& s = out[ri];
      for (size_t off = 0; off < s.data.size(); off += kItemBytes) {
        const size_t n = std::min(kItemBytes, s.data.size() - off);
        slices.push_back({s.start + off, s.start + off + n, s.start + s.data.size(), ri});
        weight.push_back(n);
      }
    }
    RunParallel(slices.size(), weight, [&](size_t i, unsigned) {
      const Slice& sl = slices[i];
      uint8_t* dst = out[sl.region].data.data() + (sl.start - out[sl.region].start);
      const size_t n = sl.end - sl.start;
      if (ReadRaw(sl.start, dst, n)) return;
      for (size_t o = 0; o < n; o += kPage) ReadRaw(sl.start + o, dst + o, std::min(kPage, n - o));
    });
    if (cancel) return;
    std::vector<uint64_t> prefix;
    uint64_t cnt = 0;
    for (const SnapRegion& s : out) {
      prefix.push_back(cnt);
      cnt += s.data.size() >= vs ? (s.data.size() - vs) / align + 1 : 0;
    }
    std::lock_guard lock(mu);
    ClearResultsLocked();
    mode = Mode::Snapshot;
    type = p.type;
    snap = std::move(out);
    snapPrefix = std::move(prefix);
    snapAlign = align;
    count = static_cast<size_t>(cnt);
    if (skipped)
      lastError = std::format("snapshot limited to {} MiB ({} region(s) skipped)", limit >> 20, skipped);
  }

  // ------------------------------------------------------------------------------------------
  void RunNext(const ScanParams& p, std::vector<uint8_t> v1, std::vector<uint8_t> v2) {
    if (mode == Mode::Addresses) return RunNextAddresses(p, std::move(v1));
    const unsigned workers = WorkerCount();
    if (mode == Mode::Snapshot) {
      std::vector<Slice> slices;
      std::vector<uint64_t> weight;
      for (size_t ri = 0; ri < snap.size(); ++ri) {
        const SnapRegion& s = snap[ri];
        for (size_t off = 0; off < s.data.size(); off += kItemBytes) {
          const size_t n = std::min(kItemBytes, s.data.size() - off);
          slices.push_back({s.start + off, s.start + off + n, s.start + s.data.size(), ri});
          weight.push_back(n);
        }
      }
      std::vector<PVec<uint8_t>> bufs(workers);
      for (auto& b : bufs) b.resize(kItemBytes + 64);
      std::vector<PVec<NumRec>> parts(slices.size());
      WithType(type, [&](auto tid) {
        using T = typename decltype(tid)::type;
        const size_t vs = sizeof(T);
        const size_t align = snapAlign;
        const Cmp<T> c = MakeCmp<T>(p, v1, v2);
        WithOp(p.op, [&](auto opc) {
          constexpr ScanOp OP = decltype(opc)::value;
          RunParallel(slices.size(), weight, [&](size_t i, unsigned w) {
            const Slice& sl = slices[i];
            const SnapRegion& sr = snap[sl.region];
            auto& out = parts[i];
            ReadSlice(sl, vs, bufs[w].data(), bufs[w].size(), [&](const uint8_t* d, uintptr_t at, size_t len, uintptr_t candEnd) {
              for (uintptr_t a = AlignUp(at, align); a < candEnd && a + vs <= at + len; a += align) {
                const T cur = Load<T>(d + (a - at));
                const T prev = Load<T>(sr.data.data() + (a - sr.start));
                if (!Match<T, OP>(cur, prev, c)) continue;
                if (!Claim()) return;
                out.push_back({a, Bits(cur)});
              }
            });
          });
        });
      });
      if (cancel) return;
      PVec<NumRec> merged = Concat(parts);
      std::lock_guard lock(mu);
      ClearResultsLocked();
      mode = Mode::Numeric;
      count = merged.size();
      num = std::move(merged);
      return;
    }

    // Numeric -> Numeric.
    const size_t items = (num.size() + kItemEntries - 1) / kItemEntries;
    std::vector<uint64_t> weight(items);
    for (size_t i = 0; i < items; ++i) weight[i] = std::min(kItemEntries, num.size() - i * kItemEntries);
    std::vector<PVec<uint8_t>> bufs(workers);
    for (auto& b : bufs) b.resize(kGroupSpan + 64);
    std::vector<PVec<NumRec>> parts(items);
    WithType(type, [&](auto tid) {
      using T = typename decltype(tid)::type;
      const size_t vs = sizeof(T);
      const Cmp<T> c = MakeCmp<T>(p, v1, v2);
      WithOp(p.op, [&](auto opc) {
        constexpr ScanOp OP = decltype(opc)::value;
        RunParallel(items, weight, [&](size_t item, unsigned w) {
          auto& out = parts[item];
          uint8_t* buf = bufs[w].data();
          const size_t b = item * kItemEntries, e = std::min(num.size(), b + kItemEntries);
          auto test = [&](const NumRec& r, const uint8_t* bytes) {
            const T cur = Load<T>(bytes);
            T prev;
            std::memcpy(&prev, &r.prev, sizeof(T));
            if (!Match<T, OP>(cur, prev, c)) return true;
            if (!Claim()) return false;
            out.push_back({r.addr, Bits(cur)});
            return true;
          };
          for (size_t i = b; i < e;) {
            size_t j = i + 1;
            while (j < e && num[j].addr + vs - num[i].addr <= kGroupSpan && num[j].addr >= num[i].addr) ++j;
            const uintptr_t start = num[i].addr;
            const size_t len = num[j - 1].addr + vs - start;
            if (ReadRaw(start, buf, len)) {
              for (size_t k = i; k < j; ++k)
                if (!test(num[k], buf + (num[k].addr - start))) return;
            } else {
              for (size_t k = i; k < j; ++k) {
                uint8_t tmp[8];
                if (ReadRaw(num[k].addr, tmp, vs) && !test(num[k], tmp)) return;
              }
            }
            i = j;
          }
        });
      });
    });
    if (cancel) return;
    PVec<NumRec> merged = Concat(parts);
    std::lock_guard lock(mu);
    num = std::move(merged);
    count = num.size();
  }

  void RunNextAddresses(const ScanParams& p, std::vector<uint8_t> v1) {
    const bool useNew = p.op == ScanOp::Exact || p.op == ScanOp::NotEqual;
    const std::vector<uint8_t>& ref = useNew ? v1 : pattern;
    const bool wantEqual = p.op == ScanOp::Exact || p.op == ScanOp::Unchanged;
    const size_t items = (addrs.size() + kItemEntries - 1) / kItemEntries;
    std::vector<uint64_t> weight(items);
    for (size_t i = 0; i < items; ++i) weight[i] = std::min(kItemEntries, addrs.size() - i * kItemEntries);
    std::vector<PVec<uintptr_t>> parts(items);
    RunParallel(items, weight, [&](size_t item, unsigned) {
      std::vector<uint8_t> tmp(ref.size());
      auto& out = parts[item];
      const size_t b = item * kItemEntries, e = std::min(addrs.size(), b + kItemEntries);
      for (size_t i = b; i < e; ++i) {
        if (!ReadRaw(addrs[i], tmp.data(), tmp.size())) continue;
        if ((tmp == ref) != wantEqual) continue;
        if (!Claim()) return;
        out.push_back(addrs[i]);
      }
    });
    if (cancel) return;
    PVec<uintptr_t> merged = Concat(parts);
    std::lock_guard lock(mu);
    addrs = std::move(merged);
    count = addrs.size();
    if (useNew) pattern = std::move(v1);
  }

  // Master-thread entry: never lets an exception escape.
  void Run(bool first, ScanParams p, std::vector<uint8_t> v1, std::vector<uint8_t> v2) {
    found = 0;
    truncated = false;
    failed = false;
    try {
      if (first) RunFirst(p, std::move(v1), std::move(v2));
      else RunNext(p, std::move(v1), std::move(v2));
    } catch (const std::bad_alloc&) {
      failed = true;
    } catch (...) {
      failed = true;
    }
    try {
      std::lock_guard lock(mu);
      if (failed) lastError = "scan failed (out of memory?)";
      else if (cancel) lastError = "scan cancelled";
      else if (truncated) lastError = std::format("results truncated at {} (refine the scan)", kMaxResults);
      if (!cancel && !failed) {
        generation.fetch_add(1);
        log::Info("mem", "scanner: {} scan ({}, {}) -> {} result(s)", first ? "first" : "next", ValueTypeName(p.type),
                  ScanOpName(p.op), count);
      }
    } catch (...) {
    }
    progress = 1.0f;
    busy = false;
  }

  bool Start(bool first, const ScanParams& p) {
    if (busy.load()) {
      SetError("scanner is busy");
      return false;
    }
    std::vector<uint8_t> v1, v2;
    {
      std::lock_guard lock(mu);
      lastError.clear();
      if (first) {
        if (!ScanOpValidForFirst(p.op)) {
          lastError = std::format("'{}' is not valid for a first scan", ScanOpName(p.op));
          return false;
        }
        if (!IsNumeric(p.type) && p.op != ScanOp::Exact) {
          lastError = "strings and byte arrays support exact-value scans only";
          return false;
        }
        if (p.rangeEnd <= p.rangeStart) {
          lastError = "empty address range";
          return false;
        }
      } else {
        if (mode == Mode::None) {
          lastError = "no results to refine";
          return false;
        }
        if (p.type != type) {
          lastError = "value type differs from the first scan";
          return false;
        }
        if (p.op == ScanOp::Unknown) {
          lastError = "'Unknown initial value' is only valid for a first scan";
          return false;
        }
        if (!IsNumeric(p.type) && p.op != ScanOp::Exact && p.op != ScanOp::NotEqual && p.op != ScanOp::Changed &&
            p.op != ScanOp::Unchanged) {
          lastError = std::format("'{}' is not supported for {}", ScanOpName(p.op), ValueTypeName(p.type));
          return false;
        }
      }
      if (ScanOpNeedsValue(p.op)) {
        if (!ParseValue(p.type, p.value, v1) || v1.empty()) {
          lastError = std::format("invalid value '{}'", p.value);
          return false;
        }
        if (p.op == ScanOp::Between && !ParseValue(p.type, p.value2, v2)) {
          lastError = std::format("invalid value '{}'", p.value2);
          return false;
        }
      }
      if (first) {   // free old results before the region walk so they are not scanned
        if (mode != Mode::None) generation.fetch_add(1);
        ClearResultsLocked();
      }
    }
    JoinMaster();
    cancel = false;
    progress = 0.0f;
    busy = true;
    try {
      master = std::thread(&Impl::Run, this, first, p, std::move(v1), std::move(v2));
    } catch (...) {
      busy = false;
      SetError("could not start scan thread");
      return false;
    }
    return true;
  }
};

Scanner::Scanner() : impl_(std::make_unique<Impl>()) {}

Scanner::~Scanner() {
  try {
    impl_->cancel = true;
    impl_->JoinMaster();
  } catch (...) {
  }
}

bool Scanner::FirstScan(const ScanParams& p) {
  try {
    return impl_->Start(true, p);
  } catch (...) {
    return false;
  }
}

bool Scanner::NextScan(const ScanParams& p) {
  try {
    return impl_->Start(false, p);
  } catch (...) {
    return false;
  }
}

void Scanner::Reset() {
  try {
    impl_->cancel = true;
    impl_->JoinMaster();
    std::lock_guard lock(impl_->mu);
    impl_->ClearResultsLocked();
    impl_->lastError.clear();
    impl_->progress = 0.0f;
    impl_->generation.fetch_add(1);
  } catch (...) {
  }
}

void Scanner::Cancel() { impl_->cancel = true; }
bool Scanner::Busy() const { return impl_->busy.load(); }
float Scanner::Progress() const { return impl_->progress.load(); }

bool Scanner::HasResults() const {
  try {
    std::lock_guard lock(impl_->mu);
    return impl_->mode != Impl::Mode::None;
  } catch (...) {
    return false;
  }
}

size_t Scanner::ResultCount() const {
  try {
    std::lock_guard lock(impl_->mu);
    return impl_->count;
  } catch (...) {
    return 0;
  }
}

std::string Scanner::LastError() const {
  try {
    std::lock_guard lock(impl_->mu);
    return impl_->lastError;
  } catch (...) {
    return {};
  }
}

ValueType Scanner::Type() const {
  try {
    std::lock_guard lock(impl_->mu);
    return impl_->type;
  } catch (...) {
    return ValueType::I32;
  }
}

uint64_t Scanner::Generation() const { return impl_->generation.load(); }

void Scanner::SetSnapshotLimit(uint64_t bytes) { impl_->snapshotLimit = std::max<uint64_t>(bytes, uint64_t{1} << 20); }

std::vector<Scanner::Row> Scanner::Page(size_t offset, size_t count, bool hex) const {
  std::vector<Row> rows;
  try {
    const Impl& im = *impl_;
    std::lock_guard lock(im.mu);
    if (offset >= im.count || count == 0) return rows;
    const size_t n = std::min(count, im.count - offset);
    rows.reserve(n);
    const ValueType t = im.type;
    auto live = [&](uintptr_t a, size_t len) -> std::string {
      uint8_t tmp[64];
      std::vector<uint8_t> big;
      uint8_t* dst = tmp;
      if (len > sizeof(tmp)) {
        big.resize(len);
        dst = big.data();
      }
      if (len == 0 || !ReadRaw(a, dst, len)) return "??";
      return FormatValue(t, dst, len, hex);
    };
    switch (im.mode) {
      case Impl::Mode::Numeric: {
        const size_t vs = ValueSize(t);
        for (size_t i = offset; i < offset + n; ++i) {
          const NumRec& r = im.num[i];
          rows.push_back({r.addr, live(r.addr, vs), FormatValue(t, &r.prev, vs, hex)});
        }
        break;
      }
      case Impl::Mode::Addresses: {
        const size_t len = im.pattern.size();
        const std::string prev = FormatValue(t, im.pattern.data(), len, hex);
        for (size_t i = offset; i < offset + n; ++i) rows.push_back({im.addrs[i], live(im.addrs[i], len), prev});
        break;
      }
      case Impl::Mode::Snapshot: {
        const size_t vs = ValueSize(t);
        auto it = std::upper_bound(im.snapPrefix.begin(), im.snapPrefix.end(), static_cast<uint64_t>(offset));
        size_t ri = static_cast<size_t>(it - im.snapPrefix.begin()) - 1;
        uint64_t idx = offset - im.snapPrefix[ri];
        for (size_t k = 0; k < n && ri < im.snap.size();) {
          const SnapRegion& s = im.snap[ri];
          const uint64_t cnt = s.data.size() >= vs ? (s.data.size() - vs) / im.snapAlign + 1 : 0;
          if (idx >= cnt) {
            ++ri;
            idx = 0;
            continue;
          }
          const size_t off = static_cast<size_t>(idx * im.snapAlign);
          const uintptr_t a = s.start + off;
          rows.push_back({a, live(a, vs), FormatValue(t, s.data.data() + off, vs, hex)});
          ++idx;
          ++k;
        }
        break;
      }
      case Impl::Mode::None: break;
    }
  } catch (...) {
  }
  return rows;
}

}  // namespace cg::mem
