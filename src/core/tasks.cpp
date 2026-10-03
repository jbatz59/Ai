#include "core/tasks.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "core/log.h"
#include "core/util.h"

namespace cg::tasks {
namespace {

// A queue that nobody drains (e.g. the game thread stalls on a loading screen) must not grow forever.
constexpr size_t kMaxQueued = 65536;
constexpr size_t kMaxAsyncQueued = 4096;
constexpr uint64_t kOverflowLogIntervalMs = 10'000;

struct Queue {
  const char* name;
  std::mutex mutex;
  std::vector<Fn> items;
  std::atomic<uint64_t> lastOverflowLogMs{0};
  explicit Queue(const char* n) : name(n) {}
};

// Leaked on purpose (see Pool): no destructors may run at process exit while threads are frozen.
Queue& RenderQueue() {
  static Queue* q = new Queue("render");
  return *q;
}
Queue& GameQueue() {
  static Queue* q = new Queue("game");
  return *q;
}

std::atomic<bool> g_gameHookActive{false};

void LogOverflow(std::atomic<uint64_t>& last, const char* name, size_t limit) {
  const uint64_t now = util::NowMs();
  uint64_t prev = last.load(std::memory_order_relaxed);
  if (prev != 0 && now - prev < kOverflowLogIntervalMs) return;
  if (!last.compare_exchange_strong(prev, now, std::memory_order_relaxed)) return;
  log::Error("tasks", "{} queue is full ({} pending tasks); dropping new work", name, limit);
}

void RunGuarded(Fn& fn, const char* where) {
  try {
    fn();
  } catch (const std::exception& e) {
    log::Error("tasks", "{} task threw: {}", where, e.what());
  } catch (...) {
    log::Error("tasks", "{} task threw an unknown exception", where);
  }
}

void Post(Queue& q, Fn&& fn) {
  if (!fn) return;
  bool full = false;
  try {
    std::lock_guard lock(q.mutex);
    if (q.items.size() >= kMaxQueued) full = true;
    else q.items.push_back(std::move(fn));
  } catch (...) {
    full = true;
  }
  if (full) LogOverflow(q.lastOverflowLogMs, q.name, kMaxQueued);
}

void Drain(Queue& q) {
  std::vector<Fn> batch;
  try {
    std::lock_guard lock(q.mutex);
    batch.swap(q.items);
  } catch (...) {
    return;
  }
  for (Fn& fn : batch) RunGuarded(fn, q.name);
}

void NameCurrentThread(const wchar_t* name) {
  using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  static const auto fn = reinterpret_cast<SetThreadDescriptionFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
  if (fn) fn(GetCurrentThread(), name);
}

struct Pool {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<Fn> queue;
  std::vector<std::thread> threads;
  bool started = false;
  bool stopping = false;
  std::atomic<uint64_t> lastOverflowLogMs{0};
};

// Leaked on purpose: if the process exits without ShutdownPool (game closed with the menu loaded),
// destroying joinable std::threads would call std::terminate.
Pool& P() {
  static Pool* p = new Pool();
  return *p;
}

void WorkerMain(Pool* pool, int index) {
  try {
    NameCurrentThread((L"Consigliere worker " + std::to_wstring(index)).c_str());
  } catch (...) {
  }
  for (;;) {
    Fn fn;
    {
      std::unique_lock lock(pool->mutex);
      pool->cv.wait(lock, [pool] { return pool->stopping || !pool->queue.empty(); });
      if (pool->stopping) return;
      fn = std::move(pool->queue.front());
      pool->queue.pop_front();
    }
    RunGuarded(fn, "async");
  }
}

int WorkerCount() {
  const int hw = static_cast<int>(std::thread::hardware_concurrency());
  return std::max(2, std::min(4, hw - 1));
}

// Caller holds pool.mutex.
void StartLocked(Pool& pool) {
  pool.started = true;
  const int n = WorkerCount();
  for (int i = 0; i < n; ++i) {
    try {
      pool.threads.emplace_back(WorkerMain, &pool, i + 1);
    } catch (const std::exception& e) {
      log::Error("tasks", "could not start worker thread {}: {}", i + 1, e.what());
    }
  }
}

}  // namespace

void PostRender(Fn fn) { Post(RenderQueue(), std::move(fn)); }
void PostGame(Fn fn) { Post(GameQueue(), std::move(fn)); }

void DrainRender() { Drain(RenderQueue()); }
void DrainGame() { Drain(GameQueue()); }

void SetGameThreadHookActive(bool active) { g_gameHookActive.store(active, std::memory_order_release); }
bool GameThreadHookActive() { return g_gameHookActive.load(std::memory_order_acquire); }

void RunAsync(Fn fn) {
  if (!fn) return;
  Pool& pool = P();
  enum class Outcome { Queued, Stopped, Full, NoThreads } outcome = Outcome::Queued;
  try {
    std::lock_guard lock(pool.mutex);
    if (pool.stopping) {
      outcome = Outcome::Stopped;
    } else {
      if (!pool.started) StartLocked(pool);
      if (pool.threads.empty()) outcome = Outcome::NoThreads;
      else if (pool.queue.size() >= kMaxAsyncQueued) outcome = Outcome::Full;
      else pool.queue.push_back(std::move(fn));
    }
  } catch (...) {
    outcome = Outcome::Full;
  }
  switch (outcome) {
    case Outcome::Queued:
      pool.cv.notify_one();
      break;
    case Outcome::Stopped:
      log::Warn("tasks", "background task rejected: worker pool is shut down");
      break;
    case Outcome::Full:
      LogOverflow(pool.lastOverflowLogMs, "async", kMaxAsyncQueued);
      break;
    case Outcome::NoThreads:
      log::Error("tasks", "no worker threads available; background task dropped");
      break;
  }
}

void ShutdownPool() {
  Pool& pool = P();
  std::vector<std::thread> threads;
  std::deque<Fn> dropped;
  try {
    std::lock_guard lock(pool.mutex);
    pool.stopping = true;
    threads.swap(pool.threads);
    dropped.swap(pool.queue);
  } catch (...) {
    return;
  }
  pool.cv.notify_all();
  if (!dropped.empty()) log::Info("tasks", "worker pool stopping; {} queued task(s) discarded", dropped.size());
  dropped.clear();
  const auto self = std::this_thread::get_id();
  for (std::thread& t : threads) {
    if (!t.joinable()) continue;
    try {
      // A task that shuts the pool down cannot join its own thread; it exits when the task returns.
      if (t.get_id() == self) t.detach();
      else t.join();
    } catch (const std::exception& e) {
      log::Error("tasks", "joining worker failed: {}", e.what());
    }
  }
}

}  // namespace cg::tasks
