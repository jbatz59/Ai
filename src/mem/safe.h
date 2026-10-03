#pragma once
// Crash-proof memory access. Every gameplay read/write in Chroma goes through here.
// Validation uses VirtualQuery with a small per-thread region cache, so hot paths stay cheap.
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>

namespace cg::mem {

// Plausible user-mode pointer: 0x10000 <= p < 0x7FFF'FFFF'0000 (and not null).
inline bool IsPlausiblePtr(uintptr_t p) { return p >= 0x10000 && p < 0x7FFFFFFF0000ull; }

bool IsReadable(uintptr_t addr, size_t size);
bool IsWritable(uintptr_t addr, size_t size);     // committed + writable (incl. copy-on-write)
bool IsExecutable(uintptr_t addr);

bool ReadRaw(uintptr_t addr, void* out, size_t size);
// Writes even to protected pages (VirtualProtect + restore); flushes the I-cache when the
// target is executable. Refuses (returns false) while mp_guard::Blocked().
bool WriteRaw(uintptr_t addr, const void* in, size_t size);
void InvalidateRegionCache();   // call after VirtualAlloc/VirtualFree-heavy operations

template <class T> std::optional<T> Read(uintptr_t addr) {
  T v;
  if (!ReadRaw(addr, &v, sizeof(T))) return std::nullopt;
  return v;
}
template <class T> T ReadOr(uintptr_t addr, T fallback) {
  T v;
  return ReadRaw(addr, &v, sizeof(T)) ? v : fallback;
}
template <class T> bool Write(uintptr_t addr, const T& v) { return WriteRaw(addr, &v, sizeof(T)); }

// Reads a pointer and validates it is plausible.
std::optional<uintptr_t> ReadPtr(uintptr_t addr);

// Pointer chain, Cheat-Engine semantics: addr = base; for each offset except the last:
// addr = *(addr + off); final = addr + last. Example Deref(b, {0x10, 0x8}) == *(b+0x10) + 0x8.
// Empty offsets returns base. Fails on any unreadable / implausible hop.
std::optional<uintptr_t> Deref(uintptr_t base, std::span<const int64_t> offsets);
inline std::optional<uintptr_t> Deref(uintptr_t base, std::initializer_list<int64_t> offsets) {
  return Deref(base, std::span<const int64_t>(offsets.begin(), offsets.size()));
}

std::string ReadCString(uintptr_t addr, size_t maxLen = 256);    // stops at NUL / unreadable
std::wstring ReadWString(uintptr_t addr, size_t maxLen = 256);

}  // namespace cg::mem
