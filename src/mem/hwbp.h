#pragma once
// Hardware breakpoints (DR0-DR3) + vectored exception handler: "find out what writes/accesses
// this address" and "what executes here", Cheat-Engine style, without an external debugger.
// Breakpoints are applied to every thread of the process (except the calling one is included
// too) and re-applied by Tick() to threads created later.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cg::mem::hwbp {

enum class Kind : uint8_t { Execute = 0, Write = 1, ReadWrite = 3 };

struct Registers {
  uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp, r8, r9, r10, r11, r12, r13, r14, r15, rip, eflags;
};

struct Hit {
  uintptr_t rip = 0;          // for data breakpoints: address of the instruction AFTER the access
  uintptr_t instruction = 0;  // best-effort start of the accessing instruction (0 if unknown)
  uint64_t count = 0;
  Registers last{};           // registers at the most recent hit from this rip
};

struct Slot {
  bool active = false;
  uintptr_t address = 0;
  Kind kind = Kind::Write;
  int size = 4;               // 1, 2, 4 or 8 (Execute => 1)
  std::vector<Hit> hits;      // unique by rip, most frequent first (Snapshot copies, max 256)
};

bool Init();        // registers the VEH (idempotent)
void Shutdown();    // clears all slots on all threads, removes the VEH

int Set(uintptr_t address, Kind kind, int size);   // returns slot 0..3 or -1 (none free / bad args)
bool Clear(int slot);
void ClearAll();
void ResetHits(int slot);

std::array<Slot, 4> Snapshot();
void Tick();        // call ~1/s: applies active slots to new threads

const char* KindName(Kind k);

}  // namespace cg::mem::hwbp
