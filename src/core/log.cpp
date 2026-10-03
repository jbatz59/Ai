#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <system_error>

#include <windows.h>

#include "core/log_internal.h"
#include "core/util.h"

namespace cg::log {
namespace {

constexpr size_t kRingCapacity = 4096;
constexpr size_t kMaxTextBytes = 4096;
constexpr size_t kMaxChannelBytes = 32;
constexpr size_t kMaxPendingBytes = 64 * 1024;
constexpr uint64_t kFlushIntervalMs = 1000;
constexpr uint64_t kMaxFileBytes = 64ull * 1024 * 1024;   // a runaway Trace loop must not fill the disk

#ifdef NDEBUG
constexpr Level kDefaultMinLevel = Level::Info;
#else
constexpr Level kDefaultMinLevel = Level::Debug;
#endif

struct State {
  std::mutex mutex;
  std::vector<Entry> ring;   // circular once full; `head` is the oldest entry
  size_t head = 0;
  HANDLE file = INVALID_HANDLE_VALUE;
  std::string pending;       // formatted lines not yet handed to WriteFile
  uint64_t lastFlushMs = 0;
  uint64_t fileBytes = 0;
  bool fileFull = false;
};

// Intentionally leaked: the logger must outlive every static destructor that might still log, and
// must not run destructors at process exit while other (already killed) threads held its mutex.
State& S() {
  static State* s = new State();
  return *s;
}

std::atomic<uint64_t> g_generation{0};
std::atomic<uint8_t> g_minLevel{static_cast<uint8_t>(kDefaultMinLevel)};

// Re-entrancy guard: a crash/assert path that logs while this thread is already inside the logger
// must not self-deadlock on the (non-recursive) mutex.
thread_local bool t_inLogger = false;

struct ReentryGuard {
  bool acquired;
  ReentryGuard() : acquired(!t_inLogger) { if (acquired) t_inLogger = true; }
  ~ReentryGuard() { if (acquired) t_inLogger = false; }
};

std::string Truncated(std::string_view s, size_t maxBytes) {
  if (s.size() <= maxBytes) return std::string(s);
  size_t cut = maxBytes;
  // Do not split a UTF-8 sequence: back up over continuation bytes.
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  std::string out(s.substr(0, cut));
  out += "...";
  return out;
}

void LocalTimeOf(uint64_t timeMs, SYSTEMTIME& out) {
  FILETIME nowUtc;
  GetSystemTimeAsFileTime(&nowUtc);
  ULARGE_INTEGER t;
  t.LowPart = nowUtc.dwLowDateTime;
  t.HighPart = nowUtc.dwHighDateTime;
  const uint64_t now = util::NowMs();
  const uint64_t ageMs = now > timeMs ? now - timeMs : 0;
  const uint64_t ageTicks = ageMs * 10000ull;
  t.QuadPart = t.QuadPart > ageTicks ? t.QuadPart - ageTicks : 0;
  FILETIME utc{t.LowPart, t.HighPart}, local{};
  if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &out)) GetLocalTime(&out);
}

std::string FormatLine(const Entry& e) {
  SYSTEMTIME st{};
  LocalTimeOf(e.timeMs, st);
  std::string line = std::format("[{:02}:{:02}:{:02}.{:03}] [{}] [{}] ", st.wHour, st.wMinute, st.wSecond,
                                 st.wMilliseconds, LevelName(e.level), e.channel);
  line += e.text;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  line += "\r\n";
  return line;
}

// Caller holds the mutex.
void FlushLocked(State& s) {
  s.lastFlushMs = util::NowMs();
  if (s.pending.empty()) return;
  if (s.file == INVALID_HANDLE_VALUE || s.fileFull) {
    s.pending.clear();
    return;
  }
  if (s.fileBytes + s.pending.size() > kMaxFileBytes) {
    s.fileFull = true;
    s.pending = "--- log file size limit reached; further entries are kept in memory only ---\r\n";
  }
  s.fileBytes += s.pending.size();
  const char* p = s.pending.data();
  size_t left = s.pending.size();
  while (left > 0) {
    DWORD written = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(left, 1u << 20));
    if (!WriteFile(s.file, p, chunk, &written, nullptr) || written == 0) break;
    p += written;
    left -= written;
  }
  // On a write failure the data is dropped rather than accumulated without bound.
  s.pending.clear();
}

// Caller holds the mutex.
void AppendLocked(State& s, Entry&& e, const std::string& line) {
  const Level lvl = e.level;
  if (s.ring.capacity() < kRingCapacity) s.ring.reserve(kRingCapacity);
  if (s.ring.size() < kRingCapacity) {
    s.ring.push_back(std::move(e));
  } else {
    s.ring[s.head] = std::move(e);
    s.head = (s.head + 1) % kRingCapacity;
  }
  if (s.file == INVALID_HANDLE_VALUE || s.fileFull) return;
  s.pending += line;
  if (lvl >= Level::Warn || s.pending.size() >= kMaxPendingBytes || util::NowMs() - s.lastFlushMs >= kFlushIntervalMs)
    FlushLocked(s);
}

void DebugOut(const std::string& line) {
  std::string msg = "Consigliere ";
  msg += line;
  OutputDebugStringA(msg.c_str());
}

// Shared implementation of Write / TryWriteNow. Returns false if `blocking` is false and the lock was busy.
bool WriteImpl(Level lvl, std::string_view channel, std::string_view text, bool blocking, bool forceFlush) {
  ReentryGuard guard;
  if (!guard.acquired) return false;
  try {
    Entry e{util::NowMs(), lvl, Truncated(channel, kMaxChannelBytes), Truncated(text, kMaxTextBytes)};
    const std::string line = FormatLine(e);
    State& s = S();
    std::unique_lock lock(s.mutex, std::defer_lock);
    if (blocking) {
      lock.lock();
    } else if (!lock.try_lock()) {
      DebugOut(line);
      return false;
    }
    AppendLocked(s, std::move(e), line);
    if (forceFlush) FlushLocked(s);
    lock.unlock();
    g_generation.fetch_add(1, std::memory_order_relaxed);
    DebugOut(line);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace

void Init(const std::filesystem::path& file) {
  ReentryGuard guard;
  if (!guard.acquired) return;
  try {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    State& s = S();
    std::lock_guard lock(s.mutex);
    if (s.file != INVALID_HANDLE_VALUE) return;
    const HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      DebugOut(std::format("[log] cannot open log file (error {}); logging to memory only\r\n", GetLastError()));
      return;
    }
    s.file = h;
    s.fileBytes = 0;
    s.fileFull = false;
    // Entries logged before Init (memory only) are written out so the file tells the whole story.
    s.pending.clear();
    const size_t n = s.ring.size();
    for (size_t i = 0; i < n; ++i) {
      s.pending += FormatLine(s.ring[(s.head + i) % n]);
      if (s.pending.size() >= kMaxPendingBytes) FlushLocked(s);
    }
    FlushLocked(s);
  } catch (...) {
  }
}

void Shutdown() {
  ReentryGuard guard;
  if (!guard.acquired) return;
  try {
    State& s = S();
    std::lock_guard lock(s.mutex);
    FlushLocked(s);
    if (s.file != INVALID_HANDLE_VALUE) {
      FlushFileBuffers(s.file);
      CloseHandle(s.file);
      s.file = INVALID_HANDLE_VALUE;
    }
  } catch (...) {
  }
}

void SetMinLevel(Level lvl) { g_minLevel.store(static_cast<uint8_t>(lvl), std::memory_order_relaxed); }

Level MinLevel() { return static_cast<Level>(g_minLevel.load(std::memory_order_relaxed)); }

void Write(Level lvl, std::string_view channel, std::string_view text) {
  if (lvl < MinLevel()) return;
  WriteImpl(lvl, channel, text, /*blocking=*/true, /*forceFlush=*/false);
}

uint64_t Generation() { return g_generation.load(std::memory_order_relaxed); }

std::vector<Entry> Copy() {
  ReentryGuard guard;
  if (!guard.acquired) return {};
  try {
    State& s = S();
    std::lock_guard lock(s.mutex);
    std::vector<Entry> out;
    const size_t n = s.ring.size();
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(s.ring[(s.head + i) % n]);
    return out;
  } catch (...) {
    return {};
  }
}

void Clear() {
  ReentryGuard guard;
  if (!guard.acquired) return;
  try {
    State& s = S();
    std::lock_guard lock(s.mutex);
    s.ring.clear();
    s.head = 0;
  } catch (...) {
  }
  g_generation.fetch_add(1, std::memory_order_relaxed);
}

const char* LevelName(Level lvl) {
  switch (lvl) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
  }
  return "?";
}

namespace internal {

bool TryWriteNow(Level lvl, std::string_view channel, std::string_view text) {
  return WriteImpl(lvl, channel, text, /*blocking=*/false, /*forceFlush=*/true);
}

bool TryCopyTail(std::vector<Entry>& out, size_t maxEntries) {
  ReentryGuard guard;
  if (!guard.acquired) return false;
  try {
    State& s = S();
    std::unique_lock lock(s.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    const size_t n = s.ring.size();
    const size_t take = std::min(n, maxEntries);
    out.clear();
    out.reserve(take);
    for (size_t i = n - take; i < n; ++i) out.push_back(s.ring[(s.head + i) % n]);
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace internal
}  // namespace cg::log
