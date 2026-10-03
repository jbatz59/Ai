#pragma once
// Fallback inline hook for targets MinHook cannot reach. MinHook jumps from the target to its
// trampoline with a 5-byte rel32 jump, so it needs free memory within ±1 GB of the target; big
// games (Mafia: DE) reserve all of the address space around their image. This writes a 14-byte
// absolute jump instead, so the trampoline can live anywhere.
//
// Only position-independent prologues are accepted (no rip-relative operand, relative branch, jump
// or return in the stolen bytes): the stolen instructions are copied verbatim at the same offsets,
// which also lets threads caught inside them be moved 1:1 between target and trampoline.
#include <cstddef>
#include <cstdint>
#include <string>

namespace cg::mem::farhook {

constexpr size_t kJmpSize = 14;   // FF 25 00 00 00 00 + absolute 64-bit address
constexpr size_t kMaxStolen = 32;

struct Hook {
  uintptr_t target = 0;
  uint8_t* trampoline = nullptr;   // stolen bytes + jump back; never freed (a thread may be inside)
  size_t len = 0;                  // stolen bytes, >= kJmpSize
  uint8_t original[kMaxStolen]{};
  uint8_t patch[kMaxStolen]{};
  bool enabled = false;
};

// Builds the trampoline (does not touch the target). `error` explains a refusal.
bool Create(uintptr_t target, void* detour, Hook& out, std::string& error);
// Writes or restores the jump with every other thread suspended.
bool SetEnabled(Hook& h, bool enabled, std::string& error);

}  // namespace cg::mem::farhook
