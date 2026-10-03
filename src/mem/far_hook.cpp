#include "mem/far_hook.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstring>
#include <vector>

#include "hde64.h"

namespace cg::mem::farhook {
namespace {

void WriteJmpAbs(uint8_t* at, uintptr_t to) {
  at[0] = 0xFF;   // jmp qword ptr [rip+0]
  at[1] = 0x25;
  at[2] = at[3] = at[4] = at[5] = 0;
  std::memcpy(at + 6, &to, sizeof(to));
}

// Suspends every other thread of the process and moves any whose RIP lies in [from, from+len) to
// the same offset in `to` (the bytes there are identical and position-independent).
class Freeze {
 public:
  Freeze(uintptr_t from, uintptr_t to, size_t len) {
    // Allocate before suspending anything: a suspended thread may hold the heap lock.
    std::vector<DWORD> ids;
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
      THREADENTRY32 te{};
      te.dwSize = sizeof(te);
      if (Thread32First(snap, &te)) {
        do {
          if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID) &&
              te.th32OwnerProcessID == pid && te.th32ThreadID != self)
            ids.push_back(te.th32ThreadID);
          te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
      }
      CloseHandle(snap);
    }
    handles_.reserve(ids.size());
    for (DWORD id : ids) {
      HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, id);
      if (!t) continue;
      if (SuspendThread(t) == static_cast<DWORD>(-1)) {
        CloseHandle(t);
        continue;
      }
      handles_.push_back(t);
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(t, &ctx) && ctx.Rip >= from && ctx.Rip < from + len) {
        ctx.Rip = to + (ctx.Rip - from);
        SetThreadContext(t, &ctx);
      }
    }
  }
  ~Freeze() {
    for (HANDLE t : handles_) {
      ResumeThread(t);
      CloseHandle(t);
    }
  }
  Freeze(const Freeze&) = delete;
  Freeze& operator=(const Freeze&) = delete;

 private:
  std::vector<HANDLE> handles_;
};

}  // namespace

bool Create(uintptr_t target, void* detour, Hook& out, std::string& error) {
  const auto* code = reinterpret_cast<const uint8_t*>(target);
  size_t len = 0;
  while (len < kJmpSize) {
    hde64s hs{};
    const unsigned n = hde64_disasm(code + len, &hs);
    if (n == 0 || (hs.flags & F_ERROR)) {
      error = "cannot decode the instruction at +" + std::to_string(len);
      return false;
    }
    if (hs.flags & F_RELATIVE) {
      error = "relative branch in the first 14 bytes";
      return false;
    }
    if ((hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5) {
      error = "rip-relative operand in the first 14 bytes";
      return false;
    }
    const uint8_t op = hs.opcode;
    const bool indirectJmp = op == 0xFF && (hs.flags & F_MODRM) && (hs.modrm_reg == 4 || hs.modrm_reg == 5);
    if (op == 0xC3 || op == 0xC2 || op == 0xCB || op == 0xCA || op == 0xCC || indirectJmp) {
      error = "function too short (it jumps or returns within 14 bytes)";
      return false;
    }
    len += n;
  }
  if (len > kMaxStolen) {
    error = "prologue instructions too long";
    return false;
  }
  auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, len + kJmpSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!tramp) {
    error = "VirtualAlloc failed (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  std::memcpy(tramp, code, len);
  WriteJmpAbs(tramp + len, target + len);
  FlushInstructionCache(GetCurrentProcess(), tramp, len + kJmpSize);

  out = Hook{};
  out.target = target;
  out.trampoline = tramp;
  out.len = len;
  std::memcpy(out.original, code, len);
  WriteJmpAbs(out.patch, reinterpret_cast<uintptr_t>(detour));
  std::memset(out.patch + kJmpSize, 0xCC, len - kJmpSize);   // never executed: threads are moved out
  return true;
}

bool SetEnabled(Hook& h, bool enabled, std::string& error) {
  if (h.enabled == enabled) return true;
  auto* code = reinterpret_cast<uint8_t*>(h.target);
  if (std::memcmp(code, enabled ? h.original : h.patch, h.len) != 0) {
    error = "the target bytes were changed by someone else";
    return false;
  }
  DWORD old = 0;
  if (!VirtualProtect(code, h.len, PAGE_EXECUTE_READWRITE, &old)) {
    error = "VirtualProtect failed (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  {
    const auto tramp = reinterpret_cast<uintptr_t>(h.trampoline);
    Freeze freeze(enabled ? h.target : tramp, enabled ? tramp : h.target, h.len);
    std::memcpy(code, enabled ? h.patch : h.original, h.len);
    FlushInstructionCache(GetCurrentProcess(), code, h.len);
  }
  DWORD unused = 0;
  VirtualProtect(code, h.len, old, &unused);
  h.enabled = enabled;
  return true;
}

}  // namespace cg::mem::farhook
