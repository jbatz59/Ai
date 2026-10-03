#pragma once
// IDA-style byte patterns ("48 8B 05 ? ? ? ? 48 85 C0", "?" or "??" = wildcard, nibble wildcards
// like "4?" also allowed) plus helpers for resolving RIP-relative operands and finding xrefs.
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mem/module.h"

namespace cg::mem {

struct Pattern {
  std::vector<uint8_t> bytes;
  std::vector<uint8_t> mask;   // per byte: 0xFF exact, 0x00 any, 0xF0/0x0F nibble masks

  static std::optional<Pattern> Parse(std::string_view ida);
  std::string ToString() const;
  size_t Size() const { return bytes.size(); }
  bool Matches(const uint8_t* p) const;
};

// Scans only readable memory inside [start, start+size). Optimised: anchors on the rarest fully
// masked byte using memchr. Never touches unreadable pages.
std::optional<uintptr_t> FindFirst(const Pattern& p, uintptr_t start, size_t size);
std::vector<uintptr_t> FindAll(const Pattern& p, uintptr_t start, size_t size, size_t maxResults = SIZE_MAX,
                               const std::atomic<bool>* cancel = nullptr);

// Module helpers. execOnly => only executable sections (default for code signatures).
std::optional<uintptr_t> FindInModule(const Pattern& p, const Module& m, bool execOnly = true);
std::vector<uintptr_t> FindAllInModule(const Pattern& p, const Module& m, bool execOnly = true, size_t maxResults = SIZE_MAX);

// RIP-relative: result = instr + instrLen + *(int32_t*)(instr + dispOffset).
// e.g. for "48 8B 05 xx xx xx xx" (mov rax,[rip+x]) use ResolveRip(at, 3, 7).
std::optional<uintptr_t> ResolveRip(uintptr_t instr, int dispOffset, int instrLen);
// For E8/E9 call/jmp rel32 at `instr`: instr + 5 + rel32.
std::optional<uintptr_t> ResolveCall(uintptr_t instr);

// Locates a NUL-terminated ASCII (or UTF-16 when wide) string literal in the module's data sections.
std::optional<uintptr_t> FindString(const Module& m, std::string_view text, bool wide = false);
// Every instruction in executable sections whose RIP-relative operand (or rel32 call/jmp) targets
// `target`. Returns instruction start addresses (validated with the length disassembler).
std::vector<uintptr_t> FindXrefs(const Module& m, uintptr_t target, size_t maxResults = 256,
                                 const std::atomic<bool>* cancel = nullptr);

// Best-effort function start: walks back from `addr` to the nearest RUNTIME_FUNCTION entry
// (x64 .pdata via RtlLookupFunctionEntry). Reliable for MSVC binaries.
std::optional<uintptr_t> FunctionStart(uintptr_t addr);

}  // namespace cg::mem
