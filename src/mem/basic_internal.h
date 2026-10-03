#pragma once
// Private helpers shared by the basic memory layer (module/safe/pattern/disasm/siggen/expr).
// Not a public API: other modules must use the headers in src/mem/*.h instead.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <windows.h>

#include "mem/disasm.h"

namespace cg::mem::detail {

inline constexpr uintptr_t kUserMin = 0x10000;
inline constexpr uintptr_t kUserMax = 0x7FFFFFFF0000ull;   // exclusive
inline constexpr size_t kPageSize = 0x1000;

// Protection predicates for a committed region's MEMORY_BASIC_INFORMATION::Protect.
inline bool ProtReadable(DWORD p) {
  if (p & PAGE_GUARD) return false;
  switch (p & 0xFF) {
    case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
  }
}
inline bool ProtWritable(DWORD p) {
  if (p & PAGE_GUARD) return false;
  switch (p & 0xFF) {
    case PAGE_READWRITE: case PAGE_WRITECOPY: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
  }
}
inline bool ProtExecutable(DWORD p) {
  if (p & PAGE_GUARD) return false;
  switch (p & 0xFF) {
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
  }
}

// module.cpp: handle of a loaded module by file name (case-insensitive). Tries the name as given,
// then with ".dll" and ".exe" appended when it has neither extension. nullptr if not loaded.
HMODULE FindModuleHandle(std::string_view name);

// disasm.cpp: decodes one instruction from `avail` bytes of `data`, which live at `address` in the
// target. Fails (length 0) when the bytes are not a valid instruction or it would exceed `avail`.
bool DecodeBytes(const uint8_t* data, size_t avail, uintptr_t address, Insn& out, bool wantText);
// Same, reading the instruction bytes from memory through ReadRaw.
bool DecodeAt(uintptr_t address, Insn& out, bool wantText);

}  // namespace cg::mem::detail
