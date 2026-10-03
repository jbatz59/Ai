#pragma once
// Thin wrapper over MinHook's HDE64 length disassembler + a compact x64 mnemonic printer good
// enough for hex views, hardware-breakpoint hit lists and signature generation.
#include <cstdint>
#include <string>
#include <vector>

namespace cg::mem {

struct Insn {
  uintptr_t address = 0;
  uint8_t length = 0;          // 0 => decode failed / unreadable
  uint8_t bytes[16]{};
  bool ripRelative = false;    // ModRM mod=00 rm=101
  uint8_t dispOffset = 0, dispSize = 0;   // byte offset/size of displacement inside the instruction
  uint8_t immOffset = 0, immSize = 0;     // byte offset/size of immediate
  bool relBranch = false;      // call/jmp/jcc with relative operand
  uintptr_t target = 0;        // absolute target of a relative branch or RIP-relative operand
  std::string text;            // best-effort Intel syntax, e.g. "mov rax, [rip+0x1234] ; 0x7FF6..."
};

bool Decode(uintptr_t address, Insn& out);
std::vector<Insn> DecodeRange(uintptr_t address, size_t maxInstructions);

// Finds the start of the instruction that ends exactly at `nextRip` (used for data breakpoints,
// where RIP points past the faulting instruction). Tries lengths 1..15 and verifies by decoding.
uintptr_t PreviousInstruction(uintptr_t nextRip);

}  // namespace cg::mem
