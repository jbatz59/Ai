#include "mem/siggen.h"

#include <algorithm>

#include "mem/basic_internal.h"
#include "mem/disasm.h"
#include "mem/pattern.h"

namespace cg::mem {
namespace {

constexpr size_t kMaxSignatureBytes = 4096;

// Copy of `p` without trailing full wildcards (they add length but never selectivity).
Pattern TrimTrailingWildcards(const Pattern& p) {
  Pattern t = p;
  while (!t.mask.empty() && t.mask.back() == 0) {
    t.mask.pop_back();
    t.bytes.pop_back();
  }
  return t;
}

void Wildcard(Pattern& p, size_t from, size_t count) {
  for (size_t i = from; i < from + count && i < p.Size(); ++i) {
    p.bytes[i] = 0;
    p.mask[i] = 0;
  }
}

}  // namespace

std::optional<std::string> GenerateSignature(uintptr_t address, const Module& m, size_t maxBytes) {
  try {
    const Section* sec = m.SectionOf(address);
    if (!sec || !sec->Executable()) return std::nullopt;
    maxBytes = std::min(maxBytes, kMaxSignatureBytes);
    const uintptr_t secEnd = sec->start + sec->size;

    Pattern pat;
    uintptr_t cur = address;
    for (;;) {
      Insn insn;
      if (!detail::DecodeAt(cur, insn, false)) return std::nullopt;
      if (cur + insn.length > secEnd || pat.Size() + insn.length > maxBytes) return std::nullopt;

      const size_t at = pat.Size();
      pat.bytes.insert(pat.bytes.end(), insn.bytes, insn.bytes + insn.length);
      pat.mask.insert(pat.mask.end(), insn.length, 0xFF);
      // Relocation-sensitive bytes: RIP-relative displacements, relative branch operands and
      // 4/8-byte immediates (absolute addresses, large constants). Opcodes, ModRM/SIB, small
      // immediates and ordinary displacements (struct offsets) are kept.
      if (insn.ripRelative && insn.dispSize) Wildcard(pat, at + insn.dispOffset, insn.dispSize);
      if (insn.immSize && (insn.relBranch || insn.immSize == 4 || insn.immSize == 8))
        Wildcard(pat, at + insn.immOffset, insn.immSize);
      cur += insn.length;

      const Pattern probe = TrimTrailingWildcards(pat);
      if (probe.Size() == 0) continue;
      const auto hits = FindAllInModule(probe, m, true, 2);
      if (hits.empty()) return std::nullopt;   // memory changed underneath us
      if (hits.size() == 1) {
        if (hits.front() != address) return std::nullopt;
        return probe.ToString();
      }
    }
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace cg::mem
