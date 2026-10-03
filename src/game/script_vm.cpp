// Havok Script (Lua 5.1 dialect) bridge: hooks the game's script tick and runs queued chunks on the
// script thread. See script_vm.h for the binding contract.
#include "game/script_vm.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>

#include "core/log.h"
#include "core/tasks.h"
#include "core/util.h"
#include "game/bindings.h"
#include "mem/hook.h"
#include "mem/safe.h"

namespace cg::game::vm {
namespace {

// ---- game-side signatures ------------------------------------------------------------------
using TickFn = int64_t (*)(void* machine);
using ResetFn = int64_t (*)(void* machine, char async, uint64_t timeout);
using LoadBufferFn = int (*)(void* L, const char* buf, size_t len, const char* name);
using PCallFn = int (*)(void* L, int nargs, int nresults, int errfunc);
using CFunction = int (*)(void* L);
using PushCClosureFn = void (*)(void* L, CFunction fn, int n, const char* name, int, int);
using SetFieldFn = void (*)(void* L, int idx, const char* key);
using LStringFn = const char* (*)(void* L, int idx, size_t* len);

constexpr const char* kChannel = "vm";
constexpr int kGlobalsIndex = -10002;   // LUA_GLOBALSINDEX in HKS
constexpr size_t kMaxQueue = 256;
constexpr size_t kMaxPrinted = 2000;
constexpr uint64_t kReadyTicks = 60;
// pcall safe-point mode (no tick hook): script points are rate-limited, so fewer are needed.
constexpr uint64_t kReadyPointsPCall = 12;
constexpr uint64_t kPointIntervalMs = 15;
constexpr uint64_t kMainLRefreshMs = 100;
constexpr uint64_t kStatsFirstLogMs = 5000;    // pcall-mode telemetry while the VM is not ready
constexpr uint64_t kStatsMaxLogMs = 120000;
constexpr int kStatsMaxLogs = 12;
constexpr std::string_view kGameNotReady = "chroma: game not ready";
constexpr uint64_t kLearnFallbackMs = 10000;   // Lua.State's VM never pcall'd for this long -> learn
constexpr uint64_t kLearnFallbackSkips = 200;
constexpr uint64_t kCandidateExpireMs = 3000;  // learned VM not seen for this long -> pick again
constexpr size_t kMaxErrorLen = 4096;
constexpr DWORD kUnloadWaitMs = 2000;
constexpr char kPrintMarker = '\x01';   // __cg_emit("\1...") => print ring instead of the collector

constexpr const char* kProbeChunk =
    "if not (game ~= nil and game.game ~= nil) then error('chroma: game not ready', 0) end";

// Wraps the global print once per VM generation; keeps the original and forwards to it.
constexpr const char* kPrintWrapChunk =
    "if __cg_print_orig == nil then __cg_print_orig = print end\n"
    "print = function(...)\n"
    "  local n = select('#', ...)\n"
    "  local t = {}\n"
    "  for i = 1, n do t[i] = tostring((select(i, ...))) end\n"
    "  pcall(__cg_emit, '\\1' .. table.concat(t, '\\t'))\n"
    "  return __cg_print_orig(...)\n"
    "end";

// ---- bound API snapshot ---------------------------------------------------------------------
struct Api {
  LoadBufferFn loadBuffer = nullptr;
  PCallFn pcall = nullptr;
  PushCClosureFn pushCClosure = nullptr;
  SetFieldFn setField = nullptr;
  LStringFn checkLString = nullptr;
  LStringFn toLString = nullptr;
  std::optional<int64_t> stateOffset;   // inside the machine; else Lua.State pointer binding
  bool stackOffsets = false;
  int64_t topOff = 0, baseOff = 0, objSize = 0;
  int64_t globalOff = -1;               // lua_State -> its VM's global state (HKS: 0x10); -1 = unknown
  bool returns() const { return pushCClosure && setField && checkLString; }
};

std::mutex g_apiMutex;
Api g_api;
std::atomic<LStringFn> g_checkLString{nullptr};
std::atomic<bool> g_hasReturns{false};

Api ApiSnapshot() {
  std::lock_guard lk(g_apiMutex);
  return g_api;
}

// ---- hook state -----------------------------------------------------------------------------
std::atomic<TickFn> g_origTick{nullptr};
std::atomic<ResetFn> g_origReset{nullptr};
std::atomic<PCallFn> g_origPCall{nullptr};
std::atomic<uintptr_t> g_tickTarget{0}, g_resetTarget{0}, g_pcallTarget{0};
std::mutex g_installMutex;

std::atomic<int> g_inside{0};
struct InsideGuard {
  InsideGuard() { g_inside.fetch_add(1, std::memory_order_acq_rel); }
  ~InsideGuard() { g_inside.fetch_sub(1, std::memory_order_acq_rel); }
  InsideGuard(const InsideGuard&) = delete;
  InsideGuard& operator=(const InsideGuard&) = delete;
};

std::atomic<bool> g_hookLive{false};
std::atomic<bool> g_unloading{false};
std::atomic<bool> g_pcallLockActive{false};
std::atomic<DWORD> g_scriptTid{0};

// pcall safe-point mode: used when Game.TickHook does not resolve. Our chunks run on the game's own
// script thread right before the game calls pcall on the MAIN lua_State (an API boundary, the same
// place a C function may call back into Lua). The state pointer from the Lua.State binding is only
// trusted when the game itself passes exactly that pointer to pcall.
//
// The game rarely calls pcall on the main state itself: it runs scripts on child threads of the
// same VM. Any lua_State the game passes to pcall is valid at that moment, so every state that
// shares the main state's global state (Lua.GlobalOffset) is accepted. If Lua.State is unusable,
// Chroma attaches to whichever VM the game calls pcall on and lets the probe (game.game) confirm it.
std::atomic<bool> g_pcallMode{false};
std::atomic<uintptr_t> g_mainL{0};
std::atomic<uintptr_t> g_mainKey{0};
std::atomic<uint64_t> g_mainLRefreshMs{0};
std::atomic<uint64_t> g_lastPointMs{0};
std::atomic<uintptr_t> g_candidateKey{0};   // learning mode only (no usable Lua.State)
std::atomic<uint64_t> g_candidateSeenMs{0};
std::atomic<bool> g_learnFallback{false};    // Lua.State resolved, but its VM never reached pcall

struct PCallStats {
  std::atomic<uint64_t> calls{0}, points{0}, badLayout{0}, otherVm{0}, badStack{0}, busy{0};
  std::atomic<uintptr_t> lastL{0}, lastKey{0};
};
PCallStats g_pcs;
std::atomic<uint64_t> g_pcallSinceMs{0};
std::atomic<uint64_t> g_nextStatsMs{0};
std::atomic<int> g_statsLogs{0};
std::mutex g_probeMutex;
std::string g_lastProbeError;   // guarded by g_probeMutex
std::atomic<bool> g_gameNotReady{false};

// ---- VM readiness -----------------------------------------------------------------------------
std::atomic<bool> g_seenMachine{false};
std::atomic<bool> g_ready{false};
std::atomic<uint64_t> g_generation{0};
std::atomic<uint64_t> g_tickCount{0};
std::atomic<bool> g_resetObserved{false};
std::atomic<bool> g_resetInProgress{false};
std::atomic<uint64_t> g_resetDoneTick{0};
std::atomic<bool> g_needRegister{true};

// Script-thread only (pcall mode: under PCallLock()).
uintptr_t g_lastL = 0;
uintptr_t g_lastKey = 0;   // VM identity the generation belongs to (pcall mode: global state)
uint64_t g_readyLoggedGen = UINT64_MAX;
uint64_t g_sameTicks = 0;
uint64_t g_probeGen = UINT64_MAX;   // generation the probe succeeded for
uint64_t g_nextProbeTick = 0;
uint64_t g_probeBackoff = 30;
uint64_t g_registeredGen = UINT64_MAX;
bool g_registeredOk = false;

thread_local bool t_inDrain = false;
thread_local std::vector<std::string>* t_collector = nullptr;

struct Lock {
  CRITICAL_SECTION cs;
  Lock() { InitializeCriticalSectionAndSpinCount(&cs, 1000); }
};
Lock& PCallLock() {
  static Lock* lock = new Lock();   // intentionally leaked: may be used by game threads during unload
  return *lock;
}

// ---- queue / print ring -----------------------------------------------------------------------
struct Job {
  std::string code;
  std::string name;
  bool asExpression = false;
  Callback cb;
};
std::mutex g_queueMutex;
std::deque<Job> g_queue;

std::mutex g_printMutex;
std::deque<std::string> g_printed;

void PushPrinted(std::string line) {
  std::lock_guard lk(g_printMutex);
  g_printed.push_back(std::move(line));
  while (g_printed.size() > kMaxPrinted) g_printed.pop_front();
}

void Deliver(Callback cb, Result r) {
  if (!cb) return;
  try {
    tasks::PostRender([cb = std::move(cb), r = std::move(r)]() {
      try {
        cb(r);
      } catch (const std::exception& e) {
        log::Error(kChannel, "callback threw: {}", e.what());
      } catch (...) {
        log::Error(kChannel, "callback threw");
      }
    });
  } catch (...) {
  }
}

void Fail(Callback cb, std::string error) {
  Result r;
  r.ok = false;
  r.error = std::move(error);
  Deliver(std::move(cb), std::move(r));
}

void FailAllQueued(const std::string& error) {
  std::deque<Job> jobs;
  {
    std::lock_guard lk(g_queueMutex);
    jobs.swap(g_queue);
  }
  for (auto& j : jobs) Fail(std::move(j.cb), error);
}

// ---- C closure exposed to Lua ---------------------------------------------------------------
// luaL_checklstring may raise a Lua error (longjmp / exception), so it runs before any C++ object
// with a destructor lives in this frame. Nothing C++ may escape back into the VM.
int CgEmit(void* L) {
  LStringFn check = g_checkLString.load(std::memory_order_acquire);
  if (!check) return 0;
  size_t len = 0;
  const char* s = check(L, 1, &len);
  if (!s) return 0;
  try {
    if (len > 0 && s[0] == kPrintMarker) {
      PushPrinted(std::string(s + 1, len - 1));
    } else if (t_collector) {
      t_collector->emplace_back(s, len);
    }
  } catch (...) {
  }
  return 0;
}

// ---- chunk execution (script thread, lock held) ----------------------------------------------
uintptr_t ReadStackTop(const Api& a, uintptr_t L, bool* ok) {
  *ok = false;
  if (!a.stackOffsets) return 0;
  const auto top = mem::Read<uintptr_t>(L + static_cast<uintptr_t>(a.topOff));
  const auto base = mem::Read<uintptr_t>(L + static_cast<uintptr_t>(a.baseOff));
  if (!top || !base || *top < *base) return 0;
  if (a.objSize > 0 && ((*top - *base) % static_cast<uintptr_t>(a.objSize)) != 0) return 0;
  *ok = true;
  return *top - *base;   // relative to base: the stack may be reallocated while our chunk runs
}

void RestoreStackTop(const Api& a, uintptr_t L, uintptr_t saved, bool haveSaved) {
  if (!haveSaved) return;
  auto* slot = reinterpret_cast<uintptr_t*>(L + static_cast<uintptr_t>(a.topOff));
  const uintptr_t target = *reinterpret_cast<const uintptr_t*>(L + static_cast<uintptr_t>(a.baseOff)) + saved;
  // Only ever pop (never expose stale slots above the current top).
  if (*slot >= target) *slot = target;
}

std::string ErrorText(const Api& a, uintptr_t L, const char* what, int status) {
  if (a.toLString) {
    size_t len = 0;
    const char* s = a.toLString(reinterpret_cast<void*>(L), -1, &len);
    if (s) {
      if (len == 0) len = strnlen(s, kMaxErrorLen);
      return std::string(s, len < kMaxErrorLen ? len : kMaxErrorLen);
    }
  }
  return std::string(what) + " " + std::to_string(status);
}

enum class Phase { Ok, Compile, Runtime };

Phase ExecChunk(const Api& a, uintptr_t L, const std::string& code, const char* name, std::string* error,
                std::vector<std::string>* collector) {
  bool haveTop = false;
  const uintptr_t top = ReadStackTop(a, L, &haveTop);
  void* l = reinterpret_cast<void*>(L);
  int st = a.loadBuffer(l, code.data(), code.size(), name);
  if (st != 0) {
    if (error) *error = ErrorText(a, L, "compile error", st);
    RestoreStackTop(a, L, top, haveTop);
    return Phase::Compile;
  }
  t_collector = collector;
  st = a.pcall(l, 0, 0, 0);
  t_collector = nullptr;
  if (st != 0) {
    if (error) *error = ErrorText(a, L, "runtime error", st);
    RestoreStackTop(a, L, top, haveTop);
    return Phase::Runtime;
  }
  RestoreStackTop(a, L, top, haveTop);
  return Phase::Ok;
}

std::string Wrap(std::string_view body) {
  std::string s;
  s.reserve(body.size() + 192);
  s += "local __cg_f = function() ";
  s += body;
  s += "\nend local __cg_r = {__cg_f()} for __cg_i = 1, #__cg_r do __cg_emit(tostring(__cg_r[__cg_i])) end";
  return s;
}

void RunJob(const Api& a, uintptr_t L, Job& job, bool wrap) {
  Result r;
  const auto t0 = std::chrono::steady_clock::now();
  const char* name = job.name.empty() ? "=chroma" : job.name.c_str();
  std::string error;
  Phase ph = Phase::Compile;
  if (job.asExpression) {
    std::string expr = "return " + job.code;
    std::vector<std::string> vals;
    std::string exprError;
    ph = ExecChunk(a, L, wrap ? Wrap(expr) : expr, name, &exprError, wrap ? &vals : nullptr);
    if (ph != Phase::Compile) {
      error = std::move(exprError);
      r.values = std::move(vals);
    }
  }
  if (ph == Phase::Compile) {
    std::vector<std::string> vals;
    error.clear();
    ph = ExecChunk(a, L, wrap ? Wrap(job.code) : job.code, name, &error, wrap ? &vals : nullptr);
    r.values = std::move(vals);
  }
  r.ok = ph == Phase::Ok;
  if (!r.ok) {
    r.error = std::move(error);
    r.values.clear();
  }
  r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  Deliver(std::move(job.cb), std::move(r));
}

void RegisterEmit(const Api& a, uintptr_t L, uint64_t gen) {
  if (g_registeredGen == gen && !g_needRegister.load()) return;
  g_registeredGen = gen;
  g_needRegister.store(false);
  g_registeredOk = false;
  if (!a.returns()) return;
  bool haveTop = false;
  const uintptr_t top = ReadStackTop(a, L, &haveTop);
  void* l = reinterpret_cast<void*>(L);
  a.pushCClosure(l, &CgEmit, 0, "__cg_emit", 0, 0);
  a.setField(l, kGlobalsIndex, "__cg_emit");
  RestoreStackTop(a, L, top, haveTop);
  g_registeredOk = true;
  std::string err;
  if (ExecChunk(a, L, kPrintWrapChunk, "=cg_print", &err, nullptr) != Phase::Ok)
    log::Debug(kChannel, "print capture not installed: {}", err);
}

// ---- tick ------------------------------------------------------------------------------------
// A real HKS lua_State has a sane API stack: base <= top, both plausible, whole 16-byte objects.
bool LooksLikeState(const Api& a, uintptr_t L) {
  if (!mem::IsPlausiblePtr(L)) return false;
  if (!a.stackOffsets) return true;
  const auto top = mem::Read<uintptr_t>(L + static_cast<uintptr_t>(a.topOff));
  const auto base = mem::Read<uintptr_t>(L + static_cast<uintptr_t>(a.baseOff));
  if (!top || !base || !mem::IsPlausiblePtr(*top) || !mem::IsPlausiblePtr(*base) || *top < *base) return false;
  const uintptr_t span = *top - *base;
  return span % static_cast<uintptr_t>(a.objSize) == 0 && span / static_cast<uintptr_t>(a.objSize) < 1000000;
}

uintptr_t ResolveState(const Api& a, void* machine) {
  uintptr_t L = 0;
  if (a.stateOffset && machine) {
    const auto p = mem::Read<uintptr_t>(reinterpret_cast<uintptr_t>(machine) + static_cast<uintptr_t>(*a.stateOffset));
    L = p ? *p : 0;
  } else {
    const auto p = Bindings::Get().Addr("Lua.State");
    L = p ? *p : 0;
  }
  return LooksLikeState(a, L) ? L : 0;
}

// Which VM a state belongs to: its global state pointer, or the state itself when unknown.
uintptr_t VmKey(const Api& a, uintptr_t L) {
  if (a.globalOff < 0 || !L) return L;
  const auto g = mem::Read<uintptr_t>(L + static_cast<uintptr_t>(a.globalOff));
  return g && mem::IsPlausiblePtr(*g) ? *g : L;
}

// Main lua_State for pcall mode, re-resolved at most every 100 ms (pcall is a hot path).
uintptr_t CachedMainL(const Api& a) {
  const uint64_t now = GetTickCount64();
  if (now - g_mainLRefreshMs.load(std::memory_order_relaxed) >= kMainLRefreshMs) {
    g_mainLRefreshMs.store(now, std::memory_order_relaxed);
    const uintptr_t L = ResolveState(a, nullptr);
    g_mainL.store(L, std::memory_order_relaxed);
    g_mainKey.store(L ? VmKey(a, L) : 0, std::memory_order_relaxed);
  }
  return g_mainL.load(std::memory_order_relaxed);
}

// Learning mode: no usable Lua.State (or it never showed up), the probe picks the VM.
bool Learning() { return g_pcallMode.load() && (!g_mainKey.load() || g_learnFallback.load()); }

void SetProbeError(std::string err) {
  g_gameNotReady.store(err == kGameNotReady);
  std::lock_guard lk(g_probeMutex);
  g_lastProbeError = std::move(err);
}

std::string LastProbeError() {
  std::lock_guard lk(g_probeMutex);
  return g_lastProbeError;
}

void BumpGeneration() {
  g_generation.fetch_add(1);
  g_ready.store(false);
}

void OnScriptPoint(const Api& a, uintptr_t L, uintptr_t key) {
  const uint64_t tick = g_tickCount.fetch_add(1) + 1;
  g_seenMachine.store(true);
  if (!a.loadBuffer || !a.pcall) return;
  const uint64_t readyTicks = g_pcallMode.load() ? kReadyPointsPCall : kReadyTicks;

  if (!L) {
    if (g_lastKey) {
      g_lastL = g_lastKey = 0;
      BumpGeneration();
    }
    g_ready.store(false);
    g_sameTicks = 0;
    FailAllQueued(StatusText());
    return;
  }
  g_lastL = L;
  if (key != g_lastKey) {
    g_lastKey = key;
    g_sameTicks = 0;
    g_nextProbeTick = 0;
    g_probeBackoff = 30;
    BumpGeneration();
  } else {
    ++g_sameTicks;
  }
  const uint64_t gen = g_generation.load();

  bool ready;
  bool needProbe = false;
  if (g_resetInProgress.load()) {
    ready = false;
  } else if (g_resetObserved.load()) {
    ready = tick - g_resetDoneTick.load() >= readyTicks;
  } else {
    ready = g_sameTicks >= readyTicks && g_probeGen == gen;
    needProbe = g_sameTicks >= readyTicks && g_probeGen != gen && tick >= g_nextProbeTick;
  }
  if (!ready && !needProbe) {
    g_ready.store(false);
    FailAllQueued(StatusText());
    return;
  }

  Lock& lock = PCallLock();
  if (!TryEnterCriticalSection(&lock.cs)) return;   // a worker pcall is running; try next tick
  struct Leave {
    CRITICAL_SECTION* cs;
    ~Leave() { LeaveCriticalSection(cs); }
  } leave{&lock.cs};
  t_inDrain = true;
  struct ResetDrain {
    ~ResetDrain() { t_inDrain = false; t_collector = nullptr; }
  } resetDrain;

  if (needProbe) {
    std::string err;
    if (ExecChunk(a, L, kProbeChunk, "=cg_probe", &err, nullptr) == Phase::Ok) {
      g_probeGen = gen;
      ready = true;
      SetProbeError({});
    } else {
      g_nextProbeTick = tick + g_probeBackoff;
      if (!a.stackOffsets && g_probeBackoff < 600) g_probeBackoff *= 2;   // each failure leaks a slot
      // Learning mode: a VM without game.game may be the wrong one; let another VM be tried.
      if (Learning()) g_candidateKey.store(0);
      SetProbeError(std::move(err));
      g_ready.store(false);
      return;
    }
  }
  if (gen != g_generation.load()) return;   // reset raced with us
  g_ready.store(ready);
  if (!ready) return;
  if (g_readyLoggedGen != gen) {
    g_readyLoggedGen = gen;
    if (g_pcallMode.load()) {
      const uintptr_t mainKey = g_mainKey.load();
      log::Info(kChannel, "Script VM ready (pcall safe points; L={} vm={} {})", util::Hex(L), util::Hex(key),
                mainKey == key ? "same VM as Lua.State" : (mainKey ? "learned, Lua.State is a different VM" : "learned, Lua.State unusable"));
    } else {
      log::Info(kChannel, "Script VM ready (tick hook; L={})", util::Hex(L));
    }
  }

  RegisterEmit(a, L, gen);
  const bool wrap = g_registeredOk;

  std::deque<Job> jobs;
  {
    std::lock_guard lk(g_queueMutex);
    jobs.swap(g_queue);
  }
  while (!jobs.empty()) {
    Job job = std::move(jobs.front());
    jobs.pop_front();
    if (g_unloading.load()) {
      Fail(std::move(job.cb), "unloading");
      continue;
    }
    try {
      RunJob(a, L, job, wrap);
    } catch (const std::exception& e) {
      Fail(std::move(job.cb), std::string("internal error: ") + e.what());
    } catch (...) {
      Fail(std::move(job.cb), "internal error");
    }
  }
}

void OnTick(void* machine) {
  const Api a = ApiSnapshot();
  const uintptr_t L = ResolveState(a, machine);
  OnScriptPoint(a, L, L);
}

int64_t TickDetour(void* machine) {
  InsideGuard guard;
  g_scriptTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
  const TickFn orig = g_origTick.load(std::memory_order_acquire);
  const int64_t ret = orig ? orig(machine) : 0;
  if (g_unloading.load()) return ret;
  try {
    OnTick(machine);
  } catch (const std::exception& e) {
    t_inDrain = false;
    t_collector = nullptr;
    try {
      log::Error(kChannel, "tick: {}", e.what());
    } catch (...) {
    }
  } catch (...) {
    t_inDrain = false;
    t_collector = nullptr;
  }
  try {
    tasks::DrainGame();
  } catch (...) {
  }
  return ret;
}

int64_t ResetDetour(void* machine, char async, uint64_t timeout) {
  InsideGuard guard;
  g_resetInProgress.store(true);
  g_ready.store(false);
  const ResetFn orig = g_origReset.load(std::memory_order_acquire);
  const int64_t ret = orig ? orig(machine, async, timeout) : 0;
  if (ret == 3) {
    g_resetObserved.store(true);
    g_resetDoneTick.store(g_tickCount.load());
    g_resetInProgress.store(false);
    BumpGeneration();
  }
  return ret;
}

// Telemetry while the VM is not ready, so one log answers "why are the toggles still locked?".
void MaybeLogPCallStats(uint64_t now) {
  uint64_t due = g_nextStatsMs.load(std::memory_order_relaxed);
  if (due == 0) {
    g_nextStatsMs.compare_exchange_strong(due, now + kStatsFirstLogMs);
    return;
  }
  if (now < due || g_ready.load(std::memory_order_relaxed) || g_statsLogs.load() >= kStatsMaxLogs) return;
  const int n = g_statsLogs.load();
  const uint64_t gap = std::min<uint64_t>(kStatsFirstLogMs << std::min(n, 5), kStatsMaxLogMs);
  if (!g_nextStatsMs.compare_exchange_strong(due, now + gap)) return;   // another thread logs
  g_statsLogs.fetch_add(1);
  const std::string probe = LastProbeError();
  log::Info(kChannel,
            "pcall mode: {} pcall(s), {} safe point(s); skipped {} bad layout, {} other VM, {} unreadable stack, {} busy; "
            "last L={} vm={} | Lua.State L={} vm={}{} | {}{}{}",
            g_pcs.calls.load(), g_pcs.points.load(), g_pcs.badLayout.load(), g_pcs.otherVm.load(), g_pcs.badStack.load(),
            g_pcs.busy.load(), util::Hex(g_pcs.lastL.load()), util::Hex(g_pcs.lastKey.load()), util::Hex(g_mainL.load()),
            util::Hex(g_mainKey.load()), Learning() ? " (learning)" : "", StatusText(),
            probe.empty() ? "" : " | probe: ", probe);
}

void PCallSafePoint(void* Lp) {
  const uintptr_t L = reinterpret_cast<uintptr_t>(Lp);
  g_pcs.calls.fetch_add(1, std::memory_order_relaxed);
  g_pcs.lastL.store(L, std::memory_order_relaxed);
  const uint64_t now = GetTickCount64();
  MaybeLogPCallStats(now);
  if (now - g_lastPointMs.load(std::memory_order_relaxed) < kPointIntervalMs) return;

  const Api a = ApiSnapshot();
  if (!LooksLikeState(a, L)) {
    g_pcs.badLayout.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uintptr_t key = VmKey(a, L);
  g_pcs.lastKey.store(key, std::memory_order_relaxed);
  CachedMainL(a);
  const uintptr_t mainKey = g_learnFallback.load(std::memory_order_relaxed) ? 0 : g_mainKey.load(std::memory_order_relaxed);
  if (mainKey) {
    if (key != mainKey) {   // another VM than the game's main script machine
      const uint64_t skips = g_pcs.otherVm.fetch_add(1, std::memory_order_relaxed) + 1;
      uint64_t since = g_pcallSinceMs.load(std::memory_order_relaxed);
      if (!since) g_pcallSinceMs.compare_exchange_strong(since, now);
      else if (now - since >= kLearnFallbackMs && skips >= kLearnFallbackSkips && g_pcs.points.load() == 0 &&
               !g_learnFallback.exchange(true))
        log::Warn(kChannel, "pcall mode: the game never ran pcall on Lua.State's VM ({} vs {}); learning the VM from the "
                  "game's own pcalls instead", util::Hex(key), util::Hex(mainKey));
      return;
    }
  } else {
    uintptr_t cand = g_candidateKey.load();
    if (cand && now - g_candidateSeenMs.load(std::memory_order_relaxed) > kCandidateExpireMs &&
        g_candidateKey.compare_exchange_strong(cand, 0))
      cand = 0;
    if (!cand && g_candidateKey.compare_exchange_strong(cand, key)) cand = key;
    if (key != cand) {
      g_pcs.otherVm.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    g_candidateSeenMs.store(now, std::memory_order_relaxed);
  }
  // The game has a call staged on this stack; a chunk that leaked a slot would shift it.
  bool stackOk = false;
  ReadStackTop(a, L, &stackOk);
  if (!stackOk) {
    g_pcs.badStack.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // One thread at a time (OnScriptPoint's bookkeeping is single-threaded); recursive for the owner.
  Lock& lock = PCallLock();
  if (!TryEnterCriticalSection(&lock.cs)) {
    g_pcs.busy.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  struct Leave {
    CRITICAL_SECTION* cs;
    ~Leave() { LeaveCriticalSection(cs); }
  } leave{&lock.cs};
  g_lastPointMs.store(now, std::memory_order_relaxed);
  g_pcs.points.fetch_add(1, std::memory_order_relaxed);
  g_scriptTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
  try {
    OnScriptPoint(a, L, key);
  } catch (...) {
    t_inDrain = false;
    t_collector = nullptr;
  }
  try {
    tasks::DrainGame();
  } catch (...) {
  }
}

int PCallDetour(void* L, int nargs, int nresults, int errfunc) {
  InsideGuard guard;
  const PCallFn orig = g_origPCall.load(std::memory_order_acquire);
  if (!orig) return -1;
  if (t_inDrain) return orig(L, nargs, nresults, errfunc);
  if (g_pcallMode.load(std::memory_order_relaxed) && !g_unloading.load(std::memory_order_relaxed)) PCallSafePoint(L);
  if (!g_pcallLockActive.load(std::memory_order_relaxed)) return orig(L, nargs, nresults, errfunc);
  Lock& lock = PCallLock();
  EnterCriticalSection(&lock.cs);
  struct Leave {
    CRITICAL_SECTION* cs;
    ~Leave() { LeaveCriticalSection(cs); }
  } leave{&lock.cs};
  return orig(L, nargs, nresults, errfunc);
}

// ---- hook management ---------------------------------------------------------------------------
void WaitInside(DWORD timeoutMs) {
  if (OnScriptThread()) return;
  const uint64_t until = util::NowMs() + timeoutMs;
  while (g_inside.load() > 0 && util::NowMs() < until) Sleep(5);
}

template <class F>
void RemoveHook(const char* name, std::atomic<uintptr_t>& target, std::atomic<F>& orig) {
  auto& hooks = mem::Hooks::Get();
  if (hooks.Exists(name)) {
    hooks.SetEnabled(name, false);
    WaitInside(kUnloadWaitMs);
    hooks.Remove(name);
  }
  target.store(0);
  orig.store(nullptr);
}

template <class F>
bool EnsureHook(const char* name, uintptr_t addr, F detour, std::atomic<uintptr_t>& target, std::atomic<F>& orig) {
  auto& hooks = mem::Hooks::Get();
  if (target.load() == addr && hooks.Exists(name)) {
    hooks.SetEnabled(name, true);
    return true;
  }
  RemoveHook(name, target, orig);
  void* tramp = nullptr;
  if (!hooks.Install(name, reinterpret_cast<void*>(addr), reinterpret_cast<void*>(detour), &tramp, false) || !tramp) {
    log::Warn(kChannel, "failed to hook {} at {}", name, util::Hex(addr));
    return false;
  }
  orig.store(reinterpret_cast<F>(tramp), std::memory_order_release);
  if (!hooks.SetEnabled(name, true)) {
    log::Warn(kChannel, "failed to enable {}", name);
    hooks.Remove(name);
    orig.store(nullptr);
    return false;
  }
  target.store(addr);
  return true;
}

template <class F>
F FnOf(const Bindings& b, std::string_view name) {
  const auto a = b.Addr(name);
  return a && *a ? reinterpret_cast<F>(*a) : nullptr;
}

}  // namespace

// ---- public API ---------------------------------------------------------------------------------
Status GetStatus() {
  if (!g_hookLive.load()) return Status::Unbound;
  if (!g_seenMachine.load()) return Status::WaitingForTick;
  if (!g_ready.load()) return Status::WaitingForVm;
  return Status::Ready;
}

const char* StatusText() {
  switch (GetStatus()) {
    case Status::Unbound: return "Script VM unavailable (required bindings missing)";
    case Status::WaitingForTick:
      return g_pcallMode.load() ? "Waiting for the game to run a script (load a save or start a mission)"
                                : "Waiting for the game's script tick";
    case Status::WaitingForVm:
      if (g_resetInProgress.load()) return "Script VM is resetting";
      return g_gameNotReady.load() ? "Waiting for the game world (load a save or start a mission)"
                                   : "Waiting for the script VM (loading)";
    case Status::Ready: return "Script VM ready";
  }
  return "Script VM: unknown state";
}

std::vector<std::string> MissingBindings() {
  std::vector<std::string> out;
  const auto& b = Bindings::Get();
  // Without a tick hook the VM runs at pcall safe points, which needs the Lua.State pointer path.
  const bool stack = b.Has("Lua.ApiTopOffset") && b.Has("Lua.ApiBaseOffset") && b.Has("Lua.ObjectSize");
  const bool pcallMode = !b.Has("Game.TickHook") && b.Has("Lua.State") && b.Has("Lua.PCall") && stack;
  if (!b.Has("Game.TickHook") && !pcallMode)
    out.emplace_back("Game.TickHook (or Lua.State + Lua.ApiTopOffset/ApiBaseOffset/ObjectSize for pcall mode)");
  if (!b.Has("Lua.StateOffset") && !b.Has("Lua.State")) out.emplace_back("Lua.StateOffset (or Lua.State)");
  if (!b.Has("Lua.LoadBuffer")) out.emplace_back("Lua.LoadBuffer");
  if (!b.Has("Lua.PCall")) out.emplace_back("Lua.PCall");
  return out;
}

bool Ready() { return GetStatus() == Status::Ready; }

bool PCallMode() { return g_pcallMode.load(); }

bool HasReturnValues() { return g_hasReturns.load(); }

void Run(std::string code, Callback cb, std::string chunkName, bool asExpression) {
  if (!Ready() || g_unloading.load()) {
    Fail(std::move(cb), g_unloading.load() ? "unloading" : StatusText());
    return;
  }
  {
    std::lock_guard lk(g_queueMutex);
    if (g_queue.size() < kMaxQueue) {
      g_queue.push_back(Job{std::move(code), std::move(chunkName), asExpression, std::move(cb)});
      return;
    }
  }
  Fail(std::move(cb), "script queue full (256 pending chunks)");
}

std::vector<std::string> TakePrintedLines() {
  std::lock_guard lk(g_printMutex);
  std::vector<std::string> out(std::make_move_iterator(g_printed.begin()), std::make_move_iterator(g_printed.end()));
  g_printed.clear();
  return out;
}

uint64_t Generation() { return g_generation.load(); }

void Install() {
  try {
    std::lock_guard installLock(g_installMutex);
    g_unloading.store(false);
    const auto& b = Bindings::Get();

    Api a;
    a.loadBuffer = FnOf<LoadBufferFn>(b, "Lua.LoadBuffer");
    a.pcall = FnOf<PCallFn>(b, "Lua.PCall");
    a.pushCClosure = FnOf<PushCClosureFn>(b, "Lua.PushCClosure");
    a.setField = FnOf<SetFieldFn>(b, "Lua.SetField");
    a.checkLString = FnOf<LStringFn>(b, "Lua.CheckLString");
    a.toLString = FnOf<LStringFn>(b, "Lua.ToLString");
    if (b.Has("Lua.StateOffset")) a.stateOffset = b.Offset("Lua.StateOffset");
    const auto top = b.Offset("Lua.ApiTopOffset");
    const auto base = b.Offset("Lua.ApiBaseOffset");
    const auto obj = b.Offset("Lua.ObjectSize");
    if (top && base && obj && *top >= 0 && *base >= 0 && *obj > 0) {
      a.stackOffsets = true;
      a.topOff = *top;
      a.baseOff = *base;
      a.objSize = *obj;
    }
    if (const auto g = b.Offset("Lua.GlobalOffset"); g && *g >= 0) a.globalOff = *g;
    {
      std::lock_guard lk(g_apiMutex);
      g_api = a;
    }
    g_checkLString.store(a.checkLString, std::memory_order_release);
    g_hasReturns.store(a.returns());
    g_needRegister.store(true);

    auto& hooks = mem::Hooks::Get();
    hooks.Init();

    const auto tick = b.Addr("Game.TickHook");
    const bool haveState = a.stateOffset.has_value() || b.Has("Lua.State");
    const bool pcallMode = (!tick || !*tick) && b.Has("Lua.State") && a.loadBuffer && a.pcall && a.stackOffsets;
    if (pcallMode) {
      // No tick hook on this build: run at pcall safe points on the main lua_State instead. The
      // global pcall lock is not used here (it can deadlock engine threads); chunks only run on the
      // script thread, at the same boundary where the game itself calls into Lua.
      {
        std::lock_guard lk(g_apiMutex);
        g_api.stateOffset.reset();   // resolve the state through the Lua.State pointer path
      }
      RemoveHook("vm.Tick", g_tickTarget, g_origTick);
      if (const auto reset = b.Addr("Lua.ResetState"); reset && *reset) {
        EnsureHook("vm.ResetState", *reset, &ResetDetour, g_resetTarget, g_origReset);
      } else {
        RemoveHook("vm.ResetState", g_resetTarget, g_origReset);
      }
      g_pcallLockActive.store(false);
      g_mainLRefreshMs.store(0);
      g_candidateKey.store(0);
      g_learnFallback.store(false);
      g_pcallSinceMs.store(0);
      g_nextStatsMs.store(0);
      g_statsLogs.store(0);
      g_pcallMode.store(true);
      const bool live =
          EnsureHook("vm.PCallLock", reinterpret_cast<uintptr_t>(a.pcall), &PCallDetour, g_pcallTarget, g_origPCall);
      g_hookLive.store(live);
      tasks::SetGameThreadHookActive(live);
      log::Info(kChannel, "script VM: Game.TickHook not needed, using pcall safe points instead ({}; returns: {}, stack restore: {}, VM identity: {})",
                live ? "hooked" : "hook FAILED", a.returns() ? "yes" : "no", a.stackOffsets ? "yes" : "no",
                a.globalOff >= 0 ? "global state" : "state pointer");
      return;
    }
    g_pcallMode.store(false);
    if (!tick || !*tick || !a.loadBuffer || !a.pcall || !haveState) {
      std::string missing;
      for (const auto& m : MissingBindings()) missing += (missing.empty() ? "" : ", ") + m;
      log::Warn(kChannel, "script VM disabled; missing bindings: {}", missing.empty() ? "(unresolved)" : missing);
      g_hookLive.store(false);
      g_pcallLockActive.store(false);
      RemoveHook("vm.PCallLock", g_pcallTarget, g_origPCall);
      RemoveHook("vm.ResetState", g_resetTarget, g_origReset);
      RemoveHook("vm.Tick", g_tickTarget, g_origTick);
      tasks::SetGameThreadHookActive(false);
      return;
    }

    // Optional: reset tracking.
    if (const auto reset = b.Addr("Lua.ResetState"); reset && *reset) {
      EnsureHook("vm.ResetState", *reset, &ResetDetour, g_resetTarget, g_origReset);
    } else {
      RemoveHook("vm.ResetState", g_resetTarget, g_origReset);
    }

    // Optional: serialize worker-thread pcalls against our chunks.
    const auto lockConst = b.Offset("Lua.PCallLock");
    const bool wantLock = !lockConst || *lockConst != 0;
    if (wantLock) {
      PCallLock();   // initialize before the detour can run
      const bool ok =
          EnsureHook("vm.PCallLock", reinterpret_cast<uintptr_t>(a.pcall), &PCallDetour, g_pcallTarget, g_origPCall);
      g_pcallLockActive.store(ok);
    } else {
      g_pcallLockActive.store(false);
      RemoveHook("vm.PCallLock", g_pcallTarget, g_origPCall);
    }

    const bool live = EnsureHook("vm.Tick", *tick, &TickDetour, g_tickTarget, g_origTick);
    g_hookLive.store(live);
    tasks::SetGameThreadHookActive(live);
    if (live)
      log::Info(kChannel, "script VM hooks installed (returns: {}, stack restore: {}, pcall lock: {})",
                a.returns() ? "yes" : "no", a.stackOffsets ? "yes" : "no", g_pcallLockActive.load() ? "yes" : "no");
  } catch (const std::exception& e) {
    log::Error(kChannel, "Install failed: {}", e.what());
  } catch (...) {
    log::Error(kChannel, "Install failed");
  }
}

void Uninstall() {
  try {
    std::lock_guard installLock(g_installMutex);
    g_unloading.store(true);
    g_ready.store(false);
    auto& hooks = mem::Hooks::Get();
    for (const char* name : {"vm.Tick", "vm.ResetState", "vm.PCallLock"})
      if (hooks.Exists(name)) hooks.SetEnabled(name, false);
    g_pcallLockActive.store(false);
    g_pcallMode.store(false);
    WaitInside(kUnloadWaitMs);
    for (const char* name : {"vm.Tick", "vm.ResetState", "vm.PCallLock"})
      if (hooks.Exists(name)) hooks.Remove(name);
    g_tickTarget.store(0);
    g_resetTarget.store(0);
    g_pcallTarget.store(0);
    g_hookLive.store(false);
    g_seenMachine.store(false);
    FailAllQueued("unloading");
    tasks::SetGameThreadHookActive(false);
    if (g_inside.load() > 0) log::Warn(kChannel, "{} thread(s) still inside VM detours after unload", g_inside.load());
  } catch (...) {
  }
}

int ThreadsInside() { return g_inside.load(); }

bool OnScriptThread() {
  const DWORD tid = g_scriptTid.load(std::memory_order_relaxed);
  return tid != 0 && tid == GetCurrentThreadId();
}

}  // namespace cg::game::vm
