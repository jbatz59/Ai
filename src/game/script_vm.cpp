// Havok Script (Lua 5.1 dialect) bridge: hooks the game's script tick and runs queued chunks on the
// script thread. See script_vm.h for the binding contract.
#include "game/script_vm.h"

#include <windows.h>

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

// ---- VM readiness -----------------------------------------------------------------------------
std::atomic<bool> g_seenMachine{false};
std::atomic<bool> g_ready{false};
std::atomic<uint64_t> g_generation{0};
std::atomic<uint64_t> g_tickCount{0};
std::atomic<bool> g_resetObserved{false};
std::atomic<bool> g_resetInProgress{false};
std::atomic<uint64_t> g_resetDoneTick{0};
std::atomic<bool> g_needRegister{true};

// Script-thread only.
uintptr_t g_lastL = 0;
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
  return *top;
}

void RestoreStackTop(const Api& a, uintptr_t L, uintptr_t saved, bool haveSaved) {
  if (!haveSaved) return;
  auto* slot = reinterpret_cast<uintptr_t*>(L + static_cast<uintptr_t>(a.topOff));
  // Only ever pop (never expose stale slots above the current top).
  if (*slot >= saved) *slot = saved;
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
uintptr_t ResolveState(const Api& a, void* machine) {
  if (a.stateOffset) {
    if (!machine) return 0;
    const auto p = mem::Read<uintptr_t>(reinterpret_cast<uintptr_t>(machine) + static_cast<uintptr_t>(*a.stateOffset));
    return p && mem::IsPlausiblePtr(*p) ? *p : 0;
  }
  const auto p = Bindings::Get().Addr("Lua.State");
  return p && mem::IsPlausiblePtr(*p) ? *p : 0;
}

void BumpGeneration() {
  g_generation.fetch_add(1);
  g_ready.store(false);
}

void OnTick(void* machine) {
  const uint64_t tick = g_tickCount.fetch_add(1) + 1;
  g_seenMachine.store(true);
  const Api a = ApiSnapshot();
  if (!a.loadBuffer || !a.pcall) return;

  const uintptr_t L = ResolveState(a, machine);
  if (!L) {
    if (g_lastL) {
      g_lastL = 0;
      BumpGeneration();
    }
    g_ready.store(false);
    g_sameTicks = 0;
    FailAllQueued(StatusText());
    return;
  }
  if (L != g_lastL) {
    g_lastL = L;
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
    ready = tick - g_resetDoneTick.load() >= kReadyTicks;
  } else {
    ready = g_sameTicks >= kReadyTicks && g_probeGen == gen;
    needProbe = g_sameTicks >= kReadyTicks && g_probeGen != gen && tick >= g_nextProbeTick;
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
    } else {
      g_nextProbeTick = tick + g_probeBackoff;
      if (!a.stackOffsets && g_probeBackoff < 600) g_probeBackoff *= 2;   // each failure leaks a slot
      g_ready.store(false);
      return;
    }
  }
  if (gen != g_generation.load()) return;   // reset raced with us
  g_ready.store(ready);
  if (!ready) return;

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

int PCallDetour(void* L, int nargs, int nresults, int errfunc) {
  InsideGuard guard;
  const PCallFn orig = g_origPCall.load(std::memory_order_acquire);
  if (!orig) return -1;
  if (t_inDrain || !g_pcallLockActive.load(std::memory_order_relaxed)) return orig(L, nargs, nresults, errfunc);
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
    case Status::WaitingForTick: return "Waiting for the game's script tick";
    case Status::WaitingForVm:
      return g_resetInProgress.load() ? "Script VM is resetting" : "Waiting for the script VM (loading)";
    case Status::Ready: return "Script VM ready";
  }
  return "Script VM: unknown state";
}

std::vector<std::string> MissingBindings() {
  std::vector<std::string> out;
  const auto& b = Bindings::Get();
  if (!b.Has("Game.TickHook")) out.emplace_back("Game.TickHook");
  if (!b.Has("Lua.StateOffset") && !b.Has("Lua.State")) out.emplace_back("Lua.StateOffset (or Lua.State)");
  if (!b.Has("Lua.LoadBuffer")) out.emplace_back("Lua.LoadBuffer");
  if (!b.Has("Lua.PCall")) out.emplace_back("Lua.PCall");
  return out;
}

bool Ready() { return GetStatus() == Status::Ready; }

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
