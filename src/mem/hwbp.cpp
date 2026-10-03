#include "mem/hwbp.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include <windows.h>
#include <tlhelp32.h>

#include "core/log.h"
#include "mem/disasm.h"

namespace cg::mem::hwbp {

namespace {

constexpr size_t kMaxHits = 256;
constexpr size_t kRegCount = 18;
constexpr DWORD kHelperTimeoutMs = 5000;

struct HitEntry {
  std::atomic<uintptr_t> rip{0};
  std::atomic<uint64_t> count{0};
  std::atomic<uint64_t> regs[kRegCount] = {};
};

struct SlotState {
  std::atomic<bool> active{false};
  std::atomic<uintptr_t> address{0};
  std::atomic<uint8_t> kind{static_cast<uint8_t>(Kind::Write)};
  std::atomic<int> size{4};
  std::atomic<uint32_t> used{0};   // claimed entries (may exceed kMaxHits; clamp when reading)
  HitEntry hits[kMaxHits];
};

SlotState g_slots[4];
std::atomic<uint32_t> g_ownedMask{0};   // slots armed at least once since Init (stale hits are still ours)
std::atomic<uint64_t> g_version{1};     // bumps on every configuration change

std::mutex g_cfgMutex;                  // serialises Init/Shutdown/Set/Clear/Tick
PVOID g_veh = nullptr;

struct AppliedRec {
  uint64_t creation = 0;
  uint64_t version = 0;
};
std::unordered_map<DWORD, AppliedRec> g_applied;   // guarded by g_cfgMutex

void StoreRegs(HitEntry& e, const CONTEXT* c) {
  const uint64_t v[kRegCount] = {c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp, c->R8,
                                 c->R9,  c->R10, c->R11, c->R12, c->R13, c->R14, c->R15, c->Rip, c->EFlags};
  for (size_t i = 0; i < kRegCount; ++i) e.regs[i].store(v[i], std::memory_order_relaxed);
}

void RecordHit(SlotState& s, const CONTEXT* c) {
  const uintptr_t rip = c->Rip;
  const uint32_t used = std::min<uint32_t>(s.used.load(std::memory_order_acquire), kMaxHits);
  for (uint32_t j = 0; j < used; ++j) {
    HitEntry& e = s.hits[j];
    if (e.rip.load(std::memory_order_acquire) == rip) {
      e.count.fetch_add(1, std::memory_order_relaxed);
      StoreRegs(e, c);
      return;
    }
  }
  const uint32_t idx = s.used.fetch_add(1, std::memory_order_acq_rel);
  if (idx >= kMaxHits) return;   // table full: drop
  HitEntry& e = s.hits[idx];
  StoreRegs(e, c);
  e.count.store(1, std::memory_order_relaxed);
  e.rip.store(rip, std::memory_order_release);
}

// No allocation, locks or logging in here: it runs on arbitrary game threads.
LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* ep) {
  if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* c = ep->ContextRecord;
  const uint32_t owned = g_ownedMask.load(std::memory_order_acquire);
  const uint32_t fired = static_cast<uint32_t>(c->Dr6) & 0xF & owned;
  if (!fired) return EXCEPTION_CONTINUE_SEARCH;
  bool exec = false;
  for (int i = 0; i < 4; ++i) {
    if (!(fired & (1u << i))) continue;
    SlotState& s = g_slots[i];
    if (s.active.load(std::memory_order_acquire)) {
      RecordHit(s, c);
      if (s.kind.load(std::memory_order_relaxed) == static_cast<uint8_t>(Kind::Execute)) exec = true;
    } else {
      // Stale breakpoint on a thread not yet updated: disarm it in this thread's context.
      c->Dr7 &= ~(3ull << (2 * i));
      const uint64_t rw = (c->Dr7 >> (16 + 4 * i)) & 3;
      if (rw == 0) exec = true;
    }
  }
  c->Dr6 = 0;
  if (exec) c->EFlags |= 0x10000;   // RF: resume without re-triggering the execute breakpoint
  return EXCEPTION_CONTINUE_EXECUTION;
}

struct Desired {
  uint64_t dr[4] = {};
  uint64_t dr7 = 0;   // our bits only
};

constexpr uint64_t kOurDr7Mask = 0xFFFF00FFull;   // L0-3/G0-3 + RW/LEN fields

uint64_t LenBits(int size) {
  switch (size) {
    case 2: return 1;
    case 4: return 3;
    case 8: return 2;
    default: return 0;
  }
}

Desired ComputeDesired() {
  Desired d;
  for (int i = 0; i < 4; ++i) {
    const SlotState& s = g_slots[i];
    if (!s.active.load()) continue;
    const uint64_t kind = s.kind.load();
    d.dr[i] = s.address.load();
    d.dr7 |= 1ull << (2 * i);
    d.dr7 |= (kind & 3) << (16 + 4 * i);
    d.dr7 |= (kind == 0 ? 0 : LenBits(s.size.load())) << (18 + 4 * i);
  }
  return d;
}

struct ThreadJob {
  HANDLE handle = nullptr;
  DWORD tid = 0;
  uint64_t creation = 0;
  bool ok = false;
};

struct HelperArgs {
  Desired desired;
  ThreadJob* jobs = nullptr;
  size_t count = 0;
};

// Runs on a short-lived helper thread so that every other thread (including the caller) can be
// suspended. Must not allocate while a thread is suspended.
DWORD WINAPI HelperProc(LPVOID param) {
  auto* a = static_cast<HelperArgs*>(param);
  const DWORD self = GetCurrentThreadId();
  for (size_t i = 0; i < a->count; ++i) {
    ThreadJob& j = a->jobs[i];
    if (j.tid == self || !j.handle) continue;
    if (SuspendThread(j.handle) == static_cast<DWORD>(-1)) continue;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(j.handle, &ctx)) {
      ctx.Dr0 = a->desired.dr[0];
      ctx.Dr1 = a->desired.dr[1];
      ctx.Dr2 = a->desired.dr[2];
      ctx.Dr3 = a->desired.dr[3];
      ctx.Dr7 = (ctx.Dr7 & ~kOurDr7Mask) | a->desired.dr7;
      ctx.Dr6 = 0;
      ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      j.ok = SetThreadContext(j.handle, &ctx) != FALSE;
    }
    ResumeThread(j.handle);
  }
  return 0;
}

uint64_t ThreadCreation(HANDLE h) {
  FILETIME c{}, e{}, k{}, u{};
  if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
  return (static_cast<uint64_t>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
}

// Applies the current slots to every thread of the process (force) or only to threads that have
// not seen the current configuration yet. Caller holds g_cfgMutex.
void ApplyLocked(bool force) {
  const uint64_t version = g_version.load();
  const DWORD pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  std::vector<ThreadJob> jobs;
  std::unordered_map<DWORD, bool> seen;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    seen[te.th32ThreadID] = true;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                          FALSE, te.th32ThreadID);
    if (!h) continue;
    const uint64_t creation = ThreadCreation(h);
    if (!force) {
      auto it = g_applied.find(te.th32ThreadID);
      if (it != g_applied.end() && it->second.creation == creation && it->second.version == version) {
        CloseHandle(h);
        continue;
      }
    }
    jobs.push_back(ThreadJob{h, te.th32ThreadID, creation, false});
  }
  CloseHandle(snap);

  for (auto it = g_applied.begin(); it != g_applied.end();) it = seen.count(it->first) ? std::next(it) : g_applied.erase(it);
  if (jobs.empty()) return;

  auto* args = new HelperArgs{ComputeDesired(), jobs.data(), jobs.size()};
  HANDLE helper = CreateThread(nullptr, 0, HelperProc, args, 0, nullptr);
  if (!helper) {
    delete args;
    for (ThreadJob& j : jobs) CloseHandle(j.handle);
    log::Warn("mem", "hwbp: could not create helper thread ({})", GetLastError());
    return;
  }
  if (WaitForSingleObject(helper, kHelperTimeoutMs) != WAIT_OBJECT_0) {
    // Helper stuck: leak its arguments and handles rather than free memory it may still use.
    CloseHandle(helper);
    new std::vector<ThreadJob>(std::move(jobs));
    log::Warn("mem", "hwbp: helper thread timed out");
    return;
  }
  CloseHandle(helper);
  delete args;
  size_t ok = 0;
  for (ThreadJob& j : jobs) {
    if (j.ok) {
      g_applied[j.tid] = AppliedRec{j.creation, version};
      ++ok;
    }
    CloseHandle(j.handle);
  }
  if (force) log::Debug("mem", "hwbp: applied to {}/{} threads", ok, jobs.size());
}

bool InitLocked() {
  if (g_veh) return true;
  g_veh = AddVectoredExceptionHandler(1, VectoredHandler);
  if (!g_veh) {
    log::Warn("mem", "hwbp: AddVectoredExceptionHandler failed");
    return false;
  }
  log::Info("mem", "hwbp: vectored handler registered");
  return true;
}

void ResetSlotHits(SlotState& s) {
  s.used.store(0, std::memory_order_release);
  for (HitEntry& e : s.hits) {
    e.rip.store(0, std::memory_order_relaxed);
    e.count.store(0, std::memory_order_relaxed);
  }
}

}  // namespace

bool Init() {
  try {
    std::lock_guard lock(g_cfgMutex);
    return InitLocked();
  } catch (...) {
    return false;
  }
}

void Shutdown() {
  try {
    std::lock_guard lock(g_cfgMutex);
    bool any = false;
    for (SlotState& s : g_slots) {
      any |= s.active.exchange(false);
      ResetSlotHits(s);
    }
    g_version.fetch_add(1);
    if (any || g_ownedMask.load()) ApplyLocked(true);
    g_applied.clear();
    if (g_veh) {
      RemoveVectoredExceptionHandler(g_veh);
      g_veh = nullptr;
      log::Info("mem", "hwbp: shut down");
    }
    g_ownedMask.store(0);
  } catch (...) {
  }
}

int Set(uintptr_t address, Kind kind, int size) {
  try {
    if (kind != Kind::Execute && kind != Kind::Write && kind != Kind::ReadWrite) return -1;
    if (kind == Kind::Execute) size = 1;
    if (size != 1 && size != 2 && size != 4 && size != 8) return -1;
    if (address < 0x10000 || address >= 0x7FFFFFFF0000ull || (address & static_cast<uintptr_t>(size - 1)) != 0) return -1;
    std::lock_guard lock(g_cfgMutex);
    if (!InitLocked()) return -1;
    for (int i = 0; i < 4; ++i) {
      SlotState& s = g_slots[i];
      if (s.active.load()) {
        if (s.address.load() == address && s.kind.load() == static_cast<uint8_t>(kind) && s.size.load() == size) return i;
        continue;
      }
      ResetSlotHits(s);
      s.address.store(address);
      s.kind.store(static_cast<uint8_t>(kind));
      s.size.store(size);
      s.active.store(true, std::memory_order_release);
      g_ownedMask.fetch_or(1u << i);
      g_version.fetch_add(1);
      ApplyLocked(true);
      log::Info("mem", "hwbp: slot {} = {} {:#x} ({} bytes)", i, KindName(kind), address, size);
      return i;
    }
    return -1;
  } catch (...) {
    return -1;
  }
}

bool Clear(int slot) {
  try {
    if (slot < 0 || slot > 3) return false;
    std::lock_guard lock(g_cfgMutex);
    if (!g_slots[slot].active.exchange(false)) return false;
    g_version.fetch_add(1);
    ApplyLocked(true);
    log::Info("mem", "hwbp: slot {} cleared", slot);
    return true;
  } catch (...) {
    return false;
  }
}

void ClearAll() {
  try {
    std::lock_guard lock(g_cfgMutex);
    bool any = false;
    for (SlotState& s : g_slots) any |= s.active.exchange(false);
    if (!any) return;
    g_version.fetch_add(1);
    ApplyLocked(true);
    log::Info("mem", "hwbp: all slots cleared");
  } catch (...) {
  }
}

void ResetHits(int slot) {
  if (slot < 0 || slot > 3) return;
  ResetSlotHits(g_slots[slot]);
}

std::array<Slot, 4> Snapshot() {
  std::array<Slot, 4> out{};
  try {
    for (int i = 0; i < 4; ++i) {
      const SlotState& s = g_slots[i];
      Slot& o = out[i];
      o.active = s.active.load(std::memory_order_acquire);
      o.address = s.address.load();
      o.kind = static_cast<Kind>(s.kind.load());
      o.size = s.size.load();
      const uint32_t used = std::min<uint32_t>(s.used.load(std::memory_order_acquire), kMaxHits);
      for (uint32_t j = 0; j < used; ++j) {
        const HitEntry& e = s.hits[j];
        const uintptr_t rip = e.rip.load(std::memory_order_acquire);
        if (!rip) continue;
        const uint64_t count = e.count.load(std::memory_order_relaxed);
        uint64_t r[kRegCount];
        for (size_t k = 0; k < kRegCount; ++k) r[k] = e.regs[k].load(std::memory_order_relaxed);
        const Registers regs{r[0], r[1], r[2],  r[3],  r[4],  r[5],  r[6],  r[7],  r[8],
                             r[9], r[10], r[11], r[12], r[13], r[14], r[15], r[16], r[17]};
        auto it = std::find_if(o.hits.begin(), o.hits.end(), [&](const Hit& h) { return h.rip == rip; });
        if (it != o.hits.end()) {
          it->count += count;
          it->last = regs;
          continue;
        }
        Hit h;
        h.rip = rip;
        h.count = count;
        h.last = regs;
        o.hits.push_back(h);
      }
      for (Hit& h : o.hits) h.instruction = o.kind == Kind::Execute ? h.rip : PreviousInstruction(h.rip);
      std::stable_sort(o.hits.begin(), o.hits.end(), [](const Hit& a, const Hit& b) { return a.count > b.count; });
    }
  } catch (...) {
  }
  return out;
}

void Tick() {
  try {
    bool any = false;
    for (const SlotState& s : g_slots) any |= s.active.load(std::memory_order_relaxed);
    if (!any) return;
    std::lock_guard lock(g_cfgMutex);
    ApplyLocked(false);
  } catch (...) {
  }
}

const char* KindName(Kind k) {
  switch (k) {
    case Kind::Execute: return "execute";
    case Kind::Write: return "write";
    case Kind::ReadWrite: return "read/write";
  }
  return "?";
}

}  // namespace cg::mem::hwbp
