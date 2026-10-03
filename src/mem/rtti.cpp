#include "mem/rtti.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <windows.h>

#include "core/log.h"
#include "mem/safe.h"

namespace cg::mem::rtti {

namespace {

#pragma pack(push, 4)
struct RawCOL {
  uint32_t signature;
  uint32_t offset;
  uint32_t cdOffset;
  int32_t pTypeDescriptor;
  int32_t pClassDescriptor;
  int32_t pSelf;
};
struct RawCHD {
  uint32_t signature;
  uint32_t attributes;
  uint32_t numBaseClasses;
  int32_t pBaseClassArray;
};
struct RawBCD {
  int32_t pTypeDescriptor;
  uint32_t numContainedBases;
  int32_t mdisp, pdisp, vdisp;
  uint32_t attributes;
  int32_t pClassDescriptor;
};
#pragma pack(pop)
static_assert(sizeof(RawCOL) == 24 && sizeof(RawCHD) == 16 && sizeof(RawBCD) == 28);

constexpr size_t kTdNameOffset = 0x10;
constexpr size_t kMaxNameLen = 1024;
constexpr uint32_t kMaxBases = 512;
constexpr size_t kMaxVFuncs = 8192;
constexpr size_t kCopyChunk = 64 * 1024;

// ---------------------------------------------------------------------------------------------
// Demangler (best effort): handles nested names, simple templates with common argument types and
// name back-references. Anything it does not understand falls back to a plain '@' split.

struct Demangler {
  std::string_view s;
  size_t pos = 0;
  bool ok = true;
  std::vector<std::string> backrefs;

  bool AtEnd() const { return pos >= s.size(); }
  char Peek() const { return AtEnd() ? '\0' : s[pos]; }
  bool Eat(char c) {
    if (Peek() != c) return false;
    ++pos;
    return true;
  }

  std::string Number() {
    bool neg = Eat('?');
    uint64_t v = 0;
    char c = Peek();
    if (c >= '0' && c <= '9') {
      ++pos;
      v = static_cast<uint64_t>(c - '0') + 1;
    } else {
      while (!AtEnd() && Peek() != '@') {
        c = s[pos++];
        if (c < 'A' || c > 'P') {
          ok = false;
          return {};
        }
        v = v * 16 + static_cast<uint64_t>(c - 'A');
      }
      if (!Eat('@')) ok = false;
    }
    return (neg ? "-" : "") + std::to_string(v);
  }

  std::string SimpleName() {
    const size_t at = s.find('@', pos);
    if (at == std::string_view::npos || at == pos) {
      ok = false;
      return {};
    }
    std::string n(s.substr(pos, at - pos));
    pos = at + 1;
    return n;
  }

  std::string NamePart() {
    const char c = Peek();
    if (c >= '0' && c <= '9') {
      ++pos;
      const size_t i = static_cast<size_t>(c - '0');
      if (i >= backrefs.size()) {
        ok = false;
        return {};
      }
      return backrefs[i];
    }
    if (c == '?' && pos + 1 < s.size() && s[pos + 1] == '$') {
      pos += 2;
      Demangler inner{s, pos};
      std::string name = inner.SimpleName();
      inner.backrefs.push_back(name);
      std::string args;
      while (inner.ok && !inner.AtEnd() && inner.Peek() != '@') {
        std::string a = inner.Type();
        if (!inner.ok) break;
        if (!args.empty()) args += ',';
        args += a;
      }
      if (!inner.ok || !inner.Eat('@')) {
        ok = false;
        return {};
      }
      pos = inner.pos;
      if (!args.empty() && args.back() == '>') args += ' ';
      std::string full = name + "<" + args + ">";
      if (backrefs.size() < 10) backrefs.push_back(full);
      return full;
    }
    if (c == '?') {   // anonymous namespace / special names: keep raw
      std::string n = SimpleName();
      return n == "?A0x" ? "`anonymous namespace'" : n;
    }
    std::string n = SimpleName();
    if (ok && backrefs.size() < 10) backrefs.push_back(n);
    return n;
  }

  // Qualified name terminated by '@' (the "@@" end marker's second '@').
  std::string QualifiedName() {
    std::vector<std::string> parts;
    while (ok && !AtEnd() && Peek() != '@') parts.push_back(NamePart());
    if (!ok || !Eat('@')) {
      ok = false;
      return {};
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
      if (!out.empty()) out += "::";
      out += *it;
    }
    return out;
  }

  std::string Type() {
    const char c = Peek();
    if (AtEnd()) {
      ok = false;
      return {};
    }
    ++pos;
    switch (c) {
      case 'C': return "signed char";
      case 'D': return "char";
      case 'E': return "unsigned char";
      case 'F': return "short";
      case 'G': return "unsigned short";
      case 'H': return "int";
      case 'I': return "unsigned int";
      case 'J': return "long";
      case 'K': return "unsigned long";
      case 'M': return "float";
      case 'N': return "double";
      case 'O': return "long double";
      case 'X': return "void";
      case 'V': return QualifiedName();
      case 'U': return QualifiedName();
      case 'W':
        if (!Eat('4')) break;
        return QualifiedName();
      case '_': {
        const char d = Peek();
        ++pos;
        switch (d) {
          case 'N': return "bool";
          case 'J': return "__int64";
          case 'K': return "unsigned __int64";
          case 'W': return "wchar_t";
          case 'S': return "char16_t";
          case 'U': return "char32_t";
          default: break;
        }
        break;
      }
      case 'P':
      case 'Q':
      case 'A': {
        Eat('E');
        const char cv = Peek();
        if (cv < 'A' || cv > 'D') break;
        ++pos;
        std::string t = Type();
        if (cv == 'B' || cv == 'D') t += " const";
        return t + (c == 'A' ? "&" : "*");
      }
      case '$': {
        const char d = Peek();
        ++pos;
        if (d == '0') return Number();
        if (d == '$' && Peek() == 'V') {   // empty pack
          ++pos;
          return {};
        }
        break;
      }
      default: break;
    }
    ok = false;
    return {};
  }
};

std::string SimpleDemangle(std::string_view body) {
  while (!body.empty() && body.back() == '@') body.remove_suffix(1);
  std::vector<std::string_view> parts;
  size_t start = 0;
  while (start <= body.size()) {
    const size_t at = body.find('@', start);
    const size_t end = at == std::string_view::npos ? body.size() : at;
    if (end > start) parts.push_back(body.substr(start, end - start));
    if (at == std::string_view::npos) break;
    start = at + 1;
  }
  std::string out;
  for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
    if (!out.empty()) out += "::";
    out += *it;
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Local copy of the module's non-executable sections (never dereference game memory directly).

struct Seg {
  uint32_t rva = 0;
  std::vector<uint8_t> data;
};

struct ImageCopy {
  uintptr_t base = 0;
  std::vector<Seg> segs;

  const Seg* SegOf(uint32_t rva, size_t size) const {
    for (const Seg& s : segs)
      if (rva >= s.rva && static_cast<uint64_t>(rva) + size <= static_cast<uint64_t>(s.rva) + s.data.size()) return &s;
    return nullptr;
  }
  bool Get(uint32_t rva, void* out, size_t size) const {
    if (const Seg* s = SegOf(rva, size)) {
      std::memcpy(out, s->data.data() + (rva - s->rva), size);
      return true;
    }
    return ReadRaw(base + rva, out, size);
  }
  template <class T> bool Get(uint32_t rva, T& out) const { return Get(rva, &out, sizeof(T)); }

  std::string Name(uint32_t rva) const {
    std::string out;
    if (const Seg* s = SegOf(rva, 1)) {
      const size_t off = rva - s->rva;
      const size_t max = std::min(kMaxNameLen, s->data.size() - off);
      const char* p = reinterpret_cast<const char*>(s->data.data() + off);
      const size_t len = strnlen(p, max);
      if (len == max) return {};
      out.assign(p, len);
      return out;
    }
    return ReadCString(base + rva, kMaxNameLen);
  }
};

bool ValidMangledName(std::string_view n) {
  if (n.size() < 7 || n.substr(0, 3) != ".?A" || (n[3] != 'V' && n[3] != 'U') || n.substr(n.size() - 2) != "@@") return false;
  for (char c : n)
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) return false;
  return true;
}

bool CopySections(const Module& m, ImageCopy& img) {
  img.base = m.base;
  for (const Section& sec : m.sections) {
    if (sec.Executable() || sec.size == 0 || sec.start < m.base) continue;
    if (sec.size > 0x40000000) continue;   // sanity: 1 GiB
    Seg seg;
    seg.rva = static_cast<uint32_t>(sec.start - m.base);
    seg.data.assign(sec.size, 0);
    for (size_t off = 0; off < sec.size; off += kCopyChunk) {
      const size_t n = std::min(kCopyChunk, sec.size - off);
      if (!ReadRaw(sec.start + off, seg.data.data() + off, n)) {
        for (size_t p = 0; p < n; p += 0x1000) {   // page fallback, unreadable pages stay zero
          const size_t pn = std::min<size_t>(0x1000, n - p);
          ReadRaw(sec.start + off + p, seg.data.data() + off + p, pn);
        }
      }
    }
    img.segs.push_back(std::move(seg));
  }
  return !img.segs.empty();
}

std::vector<ClassInfo> BuildIndex(const Module& m) {
  std::vector<ClassInfo> classes;
  ImageCopy img;
  if (!CopySections(m, img)) return classes;

  // 1) TypeDescriptors by their ".?AV"/".?AU" names.
  std::unordered_map<uint32_t, size_t> tdIndex;   // TD rva -> classes index
  for (const Seg& seg : img.segs) {
    const uint8_t* d = seg.data.data();
    const size_t n = seg.data.size();
    for (size_t i = 0; i + 4 <= n; ++i) {
      if (d[i] != '.' || d[i + 1] != '?' || d[i + 2] != 'A' || (d[i + 3] != 'V' && d[i + 3] != 'U')) continue;
      const uint32_t nameRva = seg.rva + static_cast<uint32_t>(i);
      if (nameRva < kTdNameOffset) continue;
      const size_t max = std::min(kMaxNameLen, n - i);
      const char* p = reinterpret_cast<const char*>(d + i);
      const size_t len = strnlen(p, max);
      if (len == max) continue;
      std::string_view name(p, len);
      if (!ValidMangledName(name)) continue;
      const uint32_t tdRva = nameRva - static_cast<uint32_t>(kTdNameOffset);
      if (tdIndex.count(tdRva)) continue;
      ClassInfo ci;
      ci.mangled.assign(name);
      ci.name = Demangle(name);
      ci.typeDescriptor = m.base + tdRva;
      tdIndex.emplace(tdRva, classes.size());
      classes.push_back(std::move(ci));
      i += len;
    }
  }

  // 2) CompleteObjectLocators referencing known TypeDescriptors.
  struct ColRec {
    size_t cls;
    uint32_t offset;
    int32_t chd;
  };
  std::unordered_map<uint32_t, ColRec> cols;   // COL rva -> record
  for (const Seg& seg : img.segs) {
    const size_t n = seg.data.size();
    const size_t first = (4 - (seg.rva & 3)) & 3;
    for (size_t i = first; i + sizeof(RawCOL) <= n; i += 4) {
      RawCOL col;
      std::memcpy(&col, seg.data.data() + i, sizeof(col));
      if (col.signature != 1) continue;
      const uint32_t rva = seg.rva + static_cast<uint32_t>(i);
      if (col.pSelf != static_cast<int32_t>(rva)) continue;
      auto td = tdIndex.find(static_cast<uint32_t>(col.pTypeDescriptor));
      if (td == tdIndex.end()) continue;
      cols.emplace(rva, ColRec{td->second, col.offset, col.pClassDescriptor});
    }
  }

  // 3) VTables: qword == &COL, the table starts right after it.
  auto isCode = [&](uint64_t v) {
    if (v < m.base || v >= m.base + m.size) return false;
    const Section* s = m.SectionOf(static_cast<uintptr_t>(v));
    return s && s->Executable();
  };
  size_t vtableCount = 0;
  if (!cols.empty()) {
    for (const Seg& seg : img.segs) {
      const size_t n = seg.data.size();
      const size_t first = (8 - (seg.rva & 7)) & 7;
      for (size_t i = first; i + 16 <= n; i += 8) {
        uint64_t q;
        std::memcpy(&q, seg.data.data() + i, 8);
        if (q < m.base || q >= m.base + m.size) continue;
        auto col = cols.find(static_cast<uint32_t>(q - m.base));
        if (col == cols.end()) continue;
        VTableInfo vt;
        vt.address = m.base + seg.rva + i + 8;
        vt.offset = col->second.offset;
        for (size_t j = i + 8; j + 8 <= n && vt.functionCount < kMaxVFuncs; j += 8) {
          uint64_t f;
          std::memcpy(&f, seg.data.data() + j, 8);
          if (!isCode(f)) break;
          ++vt.functionCount;
        }
        classes[col->second.cls].vtables.push_back(vt);
        ++vtableCount;
      }
    }
  }

  // 4) Base classes from the ClassHierarchyDescriptor of any COL of the class.
  std::vector<int32_t> chdOf(classes.size(), 0);
  for (const auto& [rva, rec] : cols)
    if (!chdOf[rec.cls] && rec.chd > 0) chdOf[rec.cls] = rec.chd;
  for (size_t c = 0; c < classes.size(); ++c) {
    ClassInfo& ci = classes[c];
    std::sort(ci.vtables.begin(), ci.vtables.end(), [](const VTableInfo& a, const VTableInfo& b) {
      return a.offset != b.offset ? a.offset < b.offset : a.address < b.address;
    });
    if (!chdOf[c]) continue;
    RawCHD chd;
    if (!img.Get(static_cast<uint32_t>(chdOf[c]), chd)) continue;
    if (chd.numBaseClasses == 0 || chd.numBaseClasses > kMaxBases || chd.pBaseClassArray <= 0) continue;
    for (uint32_t b = 1; b < chd.numBaseClasses; ++b) {
      int32_t bcdRva = 0;
      RawBCD bcd;
      if (!img.Get(static_cast<uint32_t>(chd.pBaseClassArray) + b * 4, bcdRva) || bcdRva <= 0) break;
      if (!img.Get(static_cast<uint32_t>(bcdRva), bcd) || bcd.pTypeDescriptor <= 0) break;
      std::string baseName;
      auto it = tdIndex.find(static_cast<uint32_t>(bcd.pTypeDescriptor));
      if (it != tdIndex.end()) {
        baseName = classes[it->second].name;
      } else {
        const std::string mangled = img.Name(static_cast<uint32_t>(bcd.pTypeDescriptor) + kTdNameOffset);
        if (!ValidMangledName(mangled)) continue;
        baseName = Demangle(mangled);
      }
      if (baseName.empty() || baseName == ci.name) continue;
      if (std::find(ci.bases.begin(), ci.bases.end(), baseName) == ci.bases.end()) ci.bases.push_back(std::move(baseName));
    }
  }

  log::Info("mem", "rtti: {} classes, {} locators, {} vtables in {}", classes.size(), cols.size(), vtableCount, m.name);
  return classes;
}

std::mutex g_mutex;
std::map<uintptr_t, std::unique_ptr<std::vector<ClassInfo>>> g_cache;
std::vector<std::unique_ptr<std::vector<ClassInfo>>> g_retired;   // keeps references handed out valid

const std::vector<ClassInfo>& Empty() {
  static const std::vector<ClassInfo> empty;
  return empty;
}

// Live-object helpers, fully validated through ReadRaw.
struct LiveCol {
  uintptr_t imageBase = 0;
  RawCOL col{};
};

std::optional<LiveCol> ColOf(uintptr_t object) {
  if (!IsPlausiblePtr(object)) return std::nullopt;
  auto vt = ReadPtr(object);
  if (!vt || (*vt & 7) != 0) return std::nullopt;
  auto colAddr = ReadPtr(*vt - 8);
  if (!colAddr || (*colAddr & 3) != 0) return std::nullopt;
  LiveCol out;
  if (!ReadRaw(*colAddr, &out.col, sizeof(RawCOL))) return std::nullopt;
  if (out.col.signature != 1 || out.col.pSelf <= 0 || out.col.pTypeDescriptor <= 0) return std::nullopt;
  if (*colAddr < static_cast<uintptr_t>(out.col.pSelf)) return std::nullopt;
  out.imageBase = *colAddr - static_cast<uint32_t>(out.col.pSelf);
  if (!IsPlausiblePtr(out.imageBase) || (out.imageBase & 0xFFFF) != 0) return std::nullopt;
  return out;
}

std::string MangledAt(uintptr_t typeDescriptor) {
  std::string n = ReadCString(typeDescriptor + kTdNameOffset, kMaxNameLen);
  return ValidMangledName(n) ? n : std::string{};
}

}  // namespace

std::string Demangle(std::string_view mangled) {
  try {
    std::string_view body = mangled;
    if (body.size() >= 4 && body.substr(0, 3) == ".?A" && (body[3] == 'V' || body[3] == 'U')) body.remove_prefix(4);
    else if (body.size() >= 5 && body.substr(0, 4) == ".?AW") body.remove_prefix(body[4] == '4' ? 5 : 4);
    if (body.empty()) return std::string(mangled);
    Demangler d{body};
    std::string out = d.QualifiedName();
    if (d.ok && d.AtEnd() && !out.empty()) return out;
    return SimpleDemangle(body);
  } catch (...) {
    return std::string(mangled);
  }
}

const std::vector<ClassInfo>& Index(const Module& m) {
  try {
    if (!m.Valid()) return Empty();
    std::lock_guard lock(g_mutex);
    auto it = g_cache.find(m.base);
    if (it != g_cache.end()) return *it->second;
    const auto t0 = std::chrono::steady_clock::now();
    auto built = std::make_unique<std::vector<ClassInfo>>(BuildIndex(m));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    log::Info("mem", "rtti: index of {} built in {} ms", m.name, static_cast<long long>(ms));
    auto& ref = *built;
    g_cache.emplace(m.base, std::move(built));
    return ref;
  } catch (...) {
    log::Warn("mem", "rtti: index build failed");
    return Empty();
  }
}

void InvalidateIndex() {
  try {
    std::lock_guard lock(g_mutex);
    for (auto& [base, v] : g_cache) g_retired.push_back(std::move(v));
    g_cache.clear();
  } catch (...) {
  }
}

const ClassInfo* FindClass(const Module& m, std::string_view nameOrMangled) {
  try {
    if (nameOrMangled.empty()) return nullptr;
    const auto& classes = Index(m);
    const bool mangled = nameOrMangled.size() > 2 && nameOrMangled.substr(0, 2) == ".?";
    for (const ClassInfo& c : classes)
      if ((mangled ? c.mangled : c.name) == nameOrMangled) return &c;
    return nullptr;
  } catch (...) {
    return nullptr;
  }
}

std::optional<uintptr_t> PrimaryVTable(const Module& m, std::string_view name) {
  const ClassInfo* c = FindClass(m, name);
  if (!c) return std::nullopt;
  for (const VTableInfo& v : c->vtables)
    if (v.offset == 0) return v.address;
  return std::nullopt;
}

std::string ClassNameOf(uintptr_t object) {
  try {
    auto col = ColOf(object);
    if (!col) return {};
    const std::string mangled = MangledAt(col->imageBase + static_cast<uint32_t>(col->col.pTypeDescriptor));
    return mangled.empty() ? std::string{} : Demangle(mangled);
  } catch (...) {
    return {};
  }
}

bool IsA(uintptr_t object, std::string_view className) {
  try {
    if (className.empty()) return false;
    auto col = ColOf(object);
    if (!col) return false;
    auto matches = [&](uintptr_t td) {
      const std::string mangled = MangledAt(td);
      return !mangled.empty() && (mangled == className || Demangle(mangled) == className);
    };
    const uintptr_t base = col->imageBase;
    if (matches(base + static_cast<uint32_t>(col->col.pTypeDescriptor))) return true;
    if (col->col.pClassDescriptor <= 0) return false;
    RawCHD chd;
    if (!ReadRaw(base + static_cast<uint32_t>(col->col.pClassDescriptor), &chd, sizeof(chd))) return false;
    if (chd.numBaseClasses == 0 || chd.numBaseClasses > kMaxBases || chd.pBaseClassArray <= 0) return false;
    for (uint32_t b = 1; b < chd.numBaseClasses; ++b) {
      int32_t bcdRva = 0;
      if (!ReadRaw(base + static_cast<uint32_t>(chd.pBaseClassArray) + b * 4ull, &bcdRva, sizeof(bcdRva)) || bcdRva <= 0) return false;
      RawBCD bcd;
      if (!ReadRaw(base + static_cast<uint32_t>(bcdRva), &bcd, sizeof(bcd)) || bcd.pTypeDescriptor <= 0) return false;
      if (matches(base + static_cast<uint32_t>(bcd.pTypeDescriptor))) return true;
    }
    return false;
  } catch (...) {
    return false;
  }
}

std::vector<uintptr_t> FindInstances(uintptr_t vtable, size_t maxResults, const std::atomic<bool>* cancel,
                                     std::atomic<float>* progress) {
  std::vector<uintptr_t> results;
  try {
    if (progress) progress->store(0.0f);
    if (!vtable || maxResults == 0) {
      if (progress) progress->store(1.0f);
      return results;
    }
    struct Region {
      uintptr_t start;
      size_t size;
    };
    std::vector<Region> regions;
    uint64_t total = 0;
    uintptr_t addr = 0x10000;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < 0x7FFFFFFF0000ull && VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
      const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
      const uintptr_t next = start + mbi.RegionSize;
      if (next <= addr) break;
      const DWORD prot = mbi.Protect;
      const bool rw = !(prot & (PAGE_GUARD | PAGE_NOACCESS)) &&
                      ((prot & 0xFF) == PAGE_READWRITE || (prot & 0xFF) == PAGE_EXECUTE_READWRITE);
      if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rw) {
        regions.push_back({start, mbi.RegionSize});
        total += mbi.RegionSize;
      }
      addr = next;
    }

    constexpr size_t kChunk = 1 << 20;
    std::vector<uint8_t> buf(kChunk);
    const uintptr_t bufLo = reinterpret_cast<uintptr_t>(buf.data());
    const uintptr_t bufHi = bufLo + buf.size();
    uint64_t done = 0;
    auto scan = [&](uintptr_t at, const uint8_t* data, size_t n) {
      for (size_t i = 0; i + 8 <= n; i += 8) {
        uint64_t q;
        std::memcpy(&q, data + i, 8);
        if (q != vtable) continue;
        const uintptr_t hit = at + i;
        if (hit >= bufLo && hit < bufHi) continue;   // our own copy buffer
        results.push_back(hit);
        if (results.size() >= maxResults) return true;
      }
      return false;
    };
    for (const Region& r : regions) {
      for (size_t off = 0; off < r.size; off += kChunk) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return results;
        const size_t n = std::min(kChunk, r.size - off);
        const uintptr_t at = r.start + off;
        if (ReadRaw(at, buf.data(), n)) {
          if (scan(at, buf.data(), n)) return results;
        } else {
          for (size_t p = 0; p < n; p += 0x1000) {
            const size_t pn = std::min<size_t>(0x1000, n - p);
            if (ReadRaw(at + p, buf.data(), pn) && scan(at + p, buf.data(), pn)) return results;
          }
        }
        done += n;
        if (progress && total) progress->store(static_cast<float>(static_cast<double>(done) / static_cast<double>(total)));
      }
    }
    if (progress) progress->store(1.0f);
  } catch (...) {
  }
  return results;
}

}  // namespace cg::mem::rtti
