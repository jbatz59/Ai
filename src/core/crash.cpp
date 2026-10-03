#include "core/crash.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include "cg_version.h"
#include "core/log.h"
#include "core/log_internal.h"
#include "core/paths.h"
#include "core/util.h"

// Design notes
//  * Handlers run on the faulting thread, possibly with an exhausted stack (stack overflow) or with
//    the heap/loader lock held. They only copy the exception record + context into static storage
//    and hand off to a reporter thread that was created at Install() time; that thread formats the
//    text report, walks the stack and writes the minidump (MiniDumpWriteDump is documented to work
//    best from a thread other than the faulting one).
//  * The faulting thread waits for the reporter with a timeout, so a reporter blocked on a lock the
//    crashed thread holds can delay the crash but never hang the game forever.
//  * Every handler returns EXCEPTION_CONTINUE_SEARCH (or the chained filter's verdict): crash
//    reporting never changes whether or how an exception is handled.

namespace cg::crash {
namespace {

constexpr DWORD kReportTimeoutMs = 30'000;
constexpr DWORD kUninstallWaitMs = 60'000;
constexpr int kMaxReports = 4;
constexpr int kMaxFrames = 64;
constexpr size_t kMaxSeenPcs = 16;
constexpr size_t kLogTailEntries = 40;
constexpr size_t kStackDumpBytes = 256;

// dbghelp's minidump API, declared locally (MinGW's dbghelp.h does not ship it). Layout matches
// MINIDUMP_EXCEPTION_INFORMATION, which dbghelp.h declares under 4-byte packing.
#pragma pack(push, 4)
struct MiniDumpExceptionInfo {
  DWORD ThreadId;
  EXCEPTION_POINTERS* ExceptionPointers;
  BOOL ClientPointers;
};
#pragma pack(pop)
static_assert(sizeof(MiniDumpExceptionInfo) == 16);
constexpr DWORD kMiniDumpWithDataSegs = 0x00000001;
constexpr DWORD kMiniDumpWithIndirectlyReferencedMemory = 0x00000040;
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, DWORD, const MiniDumpExceptionInfo*, const void*,
                                          const void*);

enum class Job : uint8_t {
  Report,          // write a new report for g_req
  MarkUnhandled,   // the exception last reported first-chance went unhandled: note it in that report
};

struct Request {
  Job job;
  EXCEPTION_RECORD record;
  CONTEXT context;
  EXCEPTION_POINTERS pointers;   // points at the copies above
  DWORD threadId;
  uintptr_t stackBase;           // NT_TIB::StackBase of the faulting thread (exclusive upper bound)
  bool unhandled;                // from the unhandled-exception filter (vs. first-chance VEH)
  bool reported;                 // set by the reporter: a report was written
};

struct WalkResult {
  uintptr_t pcs[kMaxFrames];
  int count;
  bool coveredByTry;   // an __except scope in our own module will handle this (MSVC builds)
  bool aborted;        // the walk hit unreadable memory
};

// --- static state (plain data: safe to touch from exception handlers) ----------------------------
std::atomic<bool> g_installed{false};
std::atomic<DWORD> g_reporterTid{0};
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_prevFilter{nullptr};
std::atomic<bool> g_busy{false};
std::atomic<int> g_reportCount{0};
PVOID g_veh = nullptr;
HANDLE g_thread = nullptr;
HANDLE g_requestEvent = nullptr;
HANDLE g_doneEvent = nullptr;
HANDLE g_quitEvent = nullptr;
HMODULE g_dbghelp = nullptr;
MiniDumpWriteDumpFn g_miniDumpWriteDump = nullptr;
uintptr_t g_selfBase = 0;
uintptr_t g_selfEnd = 0;
uintptr_t g_tryHandler = 0;   // our module's __try language handler (MSVC builds only)

// Owned by whichever thread holds g_busy.
Request g_req;
uintptr_t g_seenPcs[kMaxSeenPcs];
size_t g_seenCount = 0;
// The last exception a report was written for. The unhandled filter can run for an exception the
// VEH already reported (and some hosts call it more than once), so it is matched on these fields.
bool g_haveLast = false;
bool g_lastUnhandled = false;   // the last report already says the exception went unhandled
DWORD g_lastThread = 0;
DWORD g_lastCode = 0;
uintptr_t g_lastAddress = 0;

// Path of the last text report (reporter thread only). Leaked: no exit-time destructor.
std::wstring& LastReportPath() {
  static std::wstring* p = new std::wstring();
  return *p;
}

std::mutex& InstallMutex() {
  static std::mutex* m = new std::mutex();
  return *m;
}

// --- memory validation for the stack walk --------------------------------------------------------

bool IsReadableRange(uintptr_t addr, size_t size) {
  if (size == 0) return true;
  if (addr == 0 || addr + size < addr) return false;
  uintptr_t cur = addr;
  const uintptr_t end = addr + size;
  while (cur < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & kReadable)) return false;
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (regionEnd <= cur) return false;
    cur = regionEnd;
  }
  return true;
}

struct StackBounds {
  uintptr_t low;    // reservation base
  uintptr_t high;   // StackBase
  bool Contains(uintptr_t a, size_t n) const { return a >= low && a + n >= a && a + n <= high; }
};

DWORD64 ContextRegister(const CONTEXT& c, unsigned index) {
  switch (index) {
    case 0: return c.Rax;
    case 1: return c.Rcx;
    case 2: return c.Rdx;
    case 3: return c.Rbx;
    case 4: return c.Rsp;
    case 5: return c.Rbp;
    case 6: return c.Rsi;
    case 7: return c.Rdi;
    case 8: return c.R8;
    case 9: return c.R9;
    case 10: return c.R10;
    case 11: return c.R11;
    case 12: return c.R12;
    case 13: return c.R13;
    case 14: return c.R14;
    case 15: return c.R15;
    default: return 0;
  }
}

// RtlVirtualUnwind dereferences RSP (and the frame register, if the function uses one) without any
// validation. Refuse to unwind a frame whose base registers do not point into the thread's stack.
bool FrameBasePlausible(DWORD64 imageBase, const RUNTIME_FUNCTION* fn, uintptr_t pc, const CONTEXT& ctx,
                        const StackBounds& stack) {
  if (!stack.Contains(ctx.Rsp, 8) || !IsReadableRange(ctx.Rsp, 8)) return false;
  const RUNTIME_FUNCTION* f = fn;
  const uintptr_t pcOffset = pc - (static_cast<uintptr_t>(imageBase) + fn->BeginAddress);
  for (int depth = 0; depth < 8 && f; ++depth) {
    const uintptr_t ui = static_cast<uintptr_t>(imageBase) + f->UnwindData;
    if (!IsReadableRange(ui, 4)) return false;
    const auto* bytes = reinterpret_cast<const uint8_t*>(ui);
    const uint8_t flags = bytes[0] >> 3;
    const uint8_t prologSize = bytes[1];
    const uint8_t codeCount = bytes[2];
    const unsigned frameReg = bytes[3] & 0x0F;
    const unsigned frameOffset = (bytes[3] >> 4) * 16u;
    // Inside the prolog the frame register is not established yet and is not used by the unwinder.
    if (frameReg != 0 && (depth > 0 || pcOffset >= prologSize)) {
      const DWORD64 base = ContextRegister(ctx, frameReg);
      if (base < frameOffset || !stack.Contains(static_cast<uintptr_t>(base - frameOffset), 16)) return false;
    }
    if (!(flags & UNW_FLAG_CHAININFO)) break;
    const uintptr_t chained = ui + 4 + ((codeCount + 1u) & ~1u) * 2u;
    if (!IsReadableRange(chained, sizeof(RUNTIME_FUNCTION))) return false;
    f = reinterpret_cast<const RUNTIME_FUNCTION*>(chained);
  }
  return true;
}

// MSVC __try/__except scope table (the language-specific data of __C_specific_handler).
bool TryScopeCovers(const void* handlerData, uintptr_t pcRva) {
  const auto base = reinterpret_cast<uintptr_t>(handlerData);
  if (base < g_selfBase || base + 4 > g_selfEnd || !IsReadableRange(base, 4)) return false;
  const DWORD count = *reinterpret_cast<const DWORD*>(base);
  if (count == 0 || count > 1024) return false;
  const size_t bytes = 4 + static_cast<size_t>(count) * 16;
  if (base + bytes > g_selfEnd || !IsReadableRange(base, bytes)) return false;
  const auto* records = reinterpret_cast<const DWORD*>(base + 4);
  for (DWORD i = 0; i < count; ++i) {
    const DWORD begin = records[i * 4 + 0], end = records[i * 4 + 1], jumpTarget = records[i * 4 + 3];
    if (jumpTarget != 0 && pcRva >= begin && pcRva < end) return true;   // __except (not __finally)
  }
  return false;
}

// Plain-data stack walk (no objects with destructors: it may be abandoned mid-way on a fault).
void WalkStackRaw(const CONTEXT* start, const StackBounds* stack, WalkResult* out) {
  CONTEXT ctx;
  std::memcpy(&ctx, start, sizeof(ctx));
  for (int i = 0; i < kMaxFrames; ++i) {
    const uintptr_t pc = static_cast<uintptr_t>(ctx.Rip);
    if (pc == 0) return;
    out->pcs[out->count++] = pc;
    const uintptr_t prevRsp = static_cast<uintptr_t>(ctx.Rsp);
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
    if (!fn) {
      // Leaf function (or a call through a bad pointer): the return address is at [RSP].
      if (!stack->Contains(prevRsp, 8) || !IsReadableRange(prevRsp, 8)) {
        out->aborted = true;
        return;
      }
      ctx.Rip = *reinterpret_cast<const DWORD64*>(prevRsp);
      ctx.Rsp = prevRsp + 8;
    } else {
      if (!FrameBasePlausible(imageBase, fn, pc, ctx, *stack)) {
        out->aborted = true;
        return;
      }
      PVOID handlerData = nullptr;
      DWORD64 establisher = 0;
      const PEXCEPTION_ROUTINE handler =
          RtlVirtualUnwind(UNW_FLAG_EHANDLER, imageBase, pc, fn, &ctx, &handlerData, &establisher, nullptr);
      if (g_tryHandler && handler && static_cast<uintptr_t>(imageBase) == g_selfBase &&
          reinterpret_cast<uintptr_t>(handler) == g_tryHandler && TryScopeCovers(handlerData, pc - g_selfBase))
        out->coveredByTry = true;
    }
    if (static_cast<uintptr_t>(ctx.Rsp) <= prevRsp || static_cast<uintptr_t>(ctx.Rsp) > stack->high) return;
  }
}

#if defined(_MSC_VER)

void WalkStack(const CONTEXT* start, const StackBounds* stack, WalkResult* out) {
  __try {
    WalkStackRaw(start, stack, out);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out->aborted = true;
  }
}

// The language handler MSVC emits for __try in this module, read from WalkStack's own unwind data.
uintptr_t FindTryHandler() {
  auto fnAddr = reinterpret_cast<uintptr_t>(&WalkStack);
  // Incremental-link thunks: follow `jmp rel32`.
  for (int i = 0; i < 4 && IsReadableRange(fnAddr, 5) && *reinterpret_cast<const uint8_t*>(fnAddr) == 0xE9; ++i)
    fnAddr = fnAddr + 5 + *reinterpret_cast<const int32_t*>(fnAddr + 1);
  DWORD64 imageBase = 0;
  const PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(fnAddr, &imageBase, nullptr);
  if (!fn) return 0;
  const uintptr_t ui = static_cast<uintptr_t>(imageBase) + fn->UnwindData;
  if (!IsReadableRange(ui, 4)) return 0;
  const auto* bytes = reinterpret_cast<const uint8_t*>(ui);
  if (!((bytes[0] >> 3) & UNW_FLAG_EHANDLER)) return 0;
  const uintptr_t handlerRvaAt = ui + 4 + ((bytes[2] + 1u) & ~1u) * 2u;
  if (!IsReadableRange(handlerRvaAt, 4)) return 0;
  return static_cast<uintptr_t>(imageBase) + *reinterpret_cast<const DWORD*>(handlerRvaAt);
}

#elif defined(__GNUC__)

// GCC has no SEH. A fault on the reporter thread while it is walking is redirected by our own
// vectored handler to WalkRecovery(), which longjmps back into WalkStack.
void* g_walkJmp[5];
std::atomic<bool> g_walkArmed{false};

[[noreturn]] void WalkRecovery() { __builtin_longjmp(g_walkJmp, 1); }

__attribute__((noinline)) void WalkStack(const CONTEXT* start, const StackBounds* stack, WalkResult* out) {
  if (__builtin_setjmp(g_walkJmp) != 0) {
    g_walkArmed.store(false, std::memory_order_relaxed);
    out->aborted = true;
    return;
  }
  g_walkArmed.store(true, std::memory_order_release);
  WalkStackRaw(start, stack, out);
  g_walkArmed.store(false, std::memory_order_release);
}

uintptr_t FindTryHandler() { return 0; }

#else

void WalkStack(const CONTEXT* start, const StackBounds* stack, WalkResult* out) { WalkStackRaw(start, stack, out); }
uintptr_t FindTryHandler() { return 0; }

#endif

// --- report formatting ---------------------------------------------------------------------------

struct ModuleInfo {
  std::string name;
  uintptr_t base;
  size_t size;
};

std::vector<ModuleInfo> SnapshotModules() {
  std::vector<ModuleInfo> mods;
  std::vector<HMODULE> handles(512);
  DWORD needed = 0;
  const HANDLE process = GetCurrentProcess();
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (!EnumProcessModules(process, handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed))
      return mods;
    if (needed <= handles.size() * sizeof(HMODULE)) break;
    handles.resize(std::min<size_t>(needed / sizeof(HMODULE) + 32, 8192));
  }
  const size_t count = std::min<size_t>(needed / sizeof(HMODULE), handles.size());
  mods.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    MODULEINFO mi{};
    if (!GetModuleInformation(process, handles[i], &mi, sizeof(mi))) continue;
    wchar_t name[MAX_PATH];
    const DWORD len = GetModuleBaseNameW(process, handles[i], name, MAX_PATH);
    mods.push_back({len ? util::Narrow(std::wstring_view(name, len)) : std::string("?"),
                    reinterpret_cast<uintptr_t>(mi.lpBaseOfDll), mi.SizeOfImage});
  }
  return mods;
}

std::string DescribeAddress(const std::vector<ModuleInfo>& mods, uintptr_t addr) {
  for (const ModuleInfo& m : mods)
    if (addr >= m.base && addr < m.base + m.size) return std::format("{}+0x{:X}", m.name, addr - m.base);
  return std::format("0x{:016X}", addr);
}

const char* ExceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND: return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT: return "EXCEPTION_FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW: return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK: return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW: return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_GUARD_PAGE: return "EXCEPTION_GUARD_PAGE";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION: return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_INVALID_HANDLE: return "EXCEPTION_INVALID_HANDLE";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP: return "EXCEPTION_SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case 0xC0000374: return "STATUS_HEAP_CORRUPTION";
    case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN (fail-fast)";
    case 0xC0000420: return "STATUS_ASSERTION_FAILURE";
    case 0x40000015: return "STATUS_FATAL_APP_EXIT";
    case 0xE06D7363: return "C++ exception (MSVC)";
    case 0x20474343: return "C++ exception (GCC)";
    default: return "unknown exception";
  }
}

std::string ExceptionDetail(const EXCEPTION_RECORD& rec) {
  if ((rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || rec.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
      rec.NumberParameters >= 2) {
    const ULONG_PTR op = rec.ExceptionInformation[0];
    const char* what = op == 0 ? "read from" : op == 1 ? "write to" : op == 8 ? "execute (DEP) at" : "access to";
    std::string s = std::format(" ({} 0x{:016X})", what, rec.ExceptionInformation[1]);
    if (rec.ExceptionCode == EXCEPTION_IN_PAGE_ERROR && rec.NumberParameters >= 3)
      s += std::format(" NTSTATUS 0x{:08X}", static_cast<uint32_t>(rec.ExceptionInformation[2]));
    return s;
  }
  return {};
}

std::string FormatRegisters(const CONTEXT& c) {
  std::string s;
  s += std::format("RAX={:016X} RBX={:016X} RCX={:016X}\r\n", c.Rax, c.Rbx, c.Rcx);
  s += std::format("RDX={:016X} RSI={:016X} RDI={:016X}\r\n", c.Rdx, c.Rsi, c.Rdi);
  s += std::format("RBP={:016X} RSP={:016X} RIP={:016X}\r\n", c.Rbp, c.Rsp, c.Rip);
  s += std::format(" R8={:016X}  R9={:016X} R10={:016X}\r\n", c.R8, c.R9, c.R10);
  s += std::format("R11={:016X} R12={:016X} R13={:016X}\r\n", c.R11, c.R12, c.R13);
  s += std::format("R14={:016X} R15={:016X} EFL={:08X}\r\n", c.R14, c.R15, static_cast<uint32_t>(c.EFlags));
  return s;
}

std::string FormatStackMemory(uintptr_t rsp, const StackBounds& stack) {
  std::string s;
  uint64_t words[kStackDumpBytes / 8];
  const uintptr_t end = std::min<uintptr_t>(rsp + kStackDumpBytes, stack.high);
  if (rsp >= end || !stack.Contains(rsp, 8)) return "  (RSP outside the thread stack)\r\n";
  SIZE_T got = 0;
  // ReadProcessMemory on our own process fails cleanly instead of faulting on bad memory.
  if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(rsp), words, end - rsp, &got) || got < 8)
    return "  (stack memory unreadable)\r\n";
  for (size_t i = 0; i < got / 8; ++i)
    s += std::format("  [RSP+0x{:03X}] {:016X}\r\n", i * 8, words[i]);
  return s;
}

std::string LocalTimestamp(const SYSTEMTIME& st) {
  return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                     st.wSecond);
}

bool WriteAll(HANDLE h, const std::string& data) {
  size_t done = 0;
  while (done < data.size()) {
    DWORD n = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 20));
    if (!WriteFile(h, data.data() + done, chunk, &n, nullptr) || n == 0) return false;
    done += n;
  }
  return true;
}

// Creates crash-YYYYMMDD-HHMMSS[-N].txt exclusively; returns its handle and the shared base path.
HANDLE CreateReportFile(const SYSTEMTIME& st, std::filesystem::path& basePath) {
  const std::filesystem::path dir = paths::Data(L"crash");
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::wstring stamp = util::Widen(std::format("crash-{:04}{:02}{:02}-{:02}{:02}{:02}", st.wYear, st.wMonth,
                                                     st.wDay, st.wHour, st.wMinute, st.wSecond));
  for (int n = 1; n <= 50; ++n) {
    std::filesystem::path base = dir / (n == 1 ? stamp : stamp + L"-" + std::to_wstring(n));
    std::filesystem::path txt = base;
    txt += L".txt";
    const HANDLE h = CreateFileW(txt.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      basePath = std::move(base);
      return h;
    }
    if (GetLastError() != ERROR_FILE_EXISTS) return INVALID_HANDLE_VALUE;
  }
  return INVALID_HANDLE_VALUE;
}

bool WriteMiniDump(const std::filesystem::path& file, std::string& outcome) {
  if (!g_miniDumpWriteDump) {
    outcome = "not written (dbghelp.dll unavailable)";
    return false;
  }
  const HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    outcome = std::format("not written (CreateFile error {})", GetLastError());
    return false;
  }
  MiniDumpExceptionInfo mei{g_req.threadId, &g_req.pointers, FALSE};
  const BOOL ok = g_miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                                      kMiniDumpWithDataSegs | kMiniDumpWithIndirectlyReferencedMemory, &mei, nullptr,
                                      nullptr);
  const DWORD err = ok ? 0 : GetLastError();
  CloseHandle(h);
  if (!ok) {
    DeleteFileW(file.c_str());
    // MiniDumpWriteDump reports HRESULTs through GetLastError.
    outcome = std::format("not written (MiniDumpWriteDump error 0x{:08X})", err);
    return false;
  }
  outcome = util::Narrow(file.filename().wstring());
  return true;
}

// Runs on the reporter thread. Returns true if a report was written.
bool WriteReport() {
  try {
    const Request& r = g_req;
    const EXCEPTION_RECORD& rec = r.record;
    const CONTEXT& ctx = r.context;

    StackBounds stack{0, r.stackBase};
    MEMORY_BASIC_INFORMATION mbi;
    if (r.stackBase && VirtualQuery(reinterpret_cast<LPCVOID>(r.stackBase - 1), &mbi, sizeof(mbi)) == sizeof(mbi))
      stack.low = reinterpret_cast<uintptr_t>(mbi.AllocationBase);

    WalkResult walk{};
    if (stack.low && stack.high > stack.low) WalkStack(&ctx, &stack, &walk);
    else walk.aborted = true;
    // A first-chance fault that one of our own __try/__except blocks handles is not a crash.
    if (!r.unhandled && walk.coveredByTry) return false;

    const std::vector<ModuleInfo> mods = SnapshotModules();
    SYSTEMTIME st{};
    GetLocalTime(&st);

    std::string text;
    text.reserve(16 * 1024);
    text += "Consigliere crash report\r\n========================\r\n";
    text += std::format("Version:    {} {}\r\n", CG_NAME, CG_VERSION);
    text += std::format("Time:       {}\r\n", LocalTimestamp(st));
    text += std::format("Process:    {} (pid {})\r\n", util::Narrow(paths::GameExe().wstring()), GetCurrentProcessId());
    text += std::format("Thread:     {}\r\n", r.threadId);
    text += std::format("Consigliere: base 0x{:016X}, size 0x{:X}\r\n", g_selfBase, g_selfEnd - g_selfBase);
    text += r.unhandled ? "Kind:       unhandled exception (the process is terminating)\r\n"
                        : "Kind:       first-chance exception inside Consigliere code\r\n";
    text += "\r\n";
    text += std::format("Exception:  0x{:08X} {}{}\r\n", static_cast<uint32_t>(rec.ExceptionCode),
                        ExceptionName(rec.ExceptionCode), ExceptionDetail(rec));
    const auto faultAddr = reinterpret_cast<uintptr_t>(rec.ExceptionAddress);
    text += std::format("Address:    0x{:016X} {}\r\n", faultAddr, DescribeAddress(mods, faultAddr));
    if (rec.ExceptionFlags & EXCEPTION_NONCONTINUABLE) text += "Flags:      noncontinuable\r\n";
    text += "\r\nRegisters:\r\n";
    text += FormatRegisters(ctx);

    text += "\r\nStack trace:\r\n";
    for (int i = 0; i < walk.count; ++i)
      text += std::format("  #{:02} 0x{:016X} {}\r\n", i, walk.pcs[i], DescribeAddress(mods, walk.pcs[i]));
    if (walk.aborted) text += "  (walk stopped: unreadable or implausible frame)\r\n";
    else if (walk.count == kMaxFrames) text += "  (truncated at 64 frames)\r\n";

    text += "\r\nStack memory:\r\n";
    text += FormatStackMemory(static_cast<uintptr_t>(ctx.Rsp), stack);

    std::vector<log::Entry> tail;
    text += "\r\nRecent log:\r\n";
    if (log::internal::TryCopyTail(tail, kLogTailEntries)) {
      for (const log::Entry& e : tail)
        text += std::format("  [{}] [{}] {}\r\n", log::LevelName(e.level), e.channel, e.text);
    } else {
      text += "  (log busy)\r\n";
    }

    text += "\r\nLoaded modules:\r\n";
    for (const ModuleInfo& m : mods)
      text += std::format("  0x{:016X} 0x{:08X} {}\r\n", m.base, static_cast<uint64_t>(m.size), m.name);

    std::filesystem::path base;
    const HANDLE h = CreateReportFile(st, base);
    if (h == INVALID_HANDLE_VALUE) {
      log::internal::TryWriteNow(log::Level::Error, "crash", "could not create a crash report file");
      return false;
    }
    WriteAll(h, text);
    FlushFileBuffers(h);

    std::filesystem::path dmp = base;
    dmp += L".dmp";
    std::string dumpOutcome;
    WriteMiniDump(dmp, dumpOutcome);
    WriteAll(h, "\r\nMinidump: " + dumpOutcome + "\r\n");
    CloseHandle(h);

    std::filesystem::path txt = base;
    txt += L".txt";
    LastReportPath() = txt.wstring();
    log::internal::TryWriteNow(log::Level::Error, "crash",
                               std::format("{} 0x{:08X} at {} - report: {}", ExceptionName(rec.ExceptionCode),
                                           static_cast<uint32_t>(rec.ExceptionCode),
                                           DescribeAddress(mods, faultAddr), util::Narrow(txt.wstring())));
    return true;
  } catch (...) {
    return false;
  }
}

void MarkLastReportUnhandled() {
  try {
    const std::wstring& path = LastReportPath();
    if (path.empty()) return;
    const HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteAll(h, "\r\nOutcome: nothing handled this exception; the process is terminating.\r\n");
    CloseHandle(h);
    log::internal::TryWriteNow(log::Level::Error, "crash", "the reported exception was not handled; the game is closing");
  } catch (...) {
  }
}

DWORD WINAPI ReporterMain(LPVOID) {
  using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  if (const auto setDesc = reinterpret_cast<SetThreadDescriptionFn>(
          reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))))
    setDesc(GetCurrentThread(), L"Consigliere crash reporter");
  const HANDLE waits[2] = {g_requestEvent, g_quitEvent};
  for (;;) {
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) return 0;
    if (g_req.job == Job::MarkUnhandled) {
      MarkLastReportUnhandled();
      g_req.reported = false;
    } else {
      g_req.reported = WriteReport();
    }
    SetEvent(g_doneEvent);
  }
}

// --- handlers (faulting thread) ------------------------------------------------------------------

// Hands g_req to the reporter thread and waits. False on timeout: the reporter may still be reading
// g_req, so the caller must keep the slot claimed.
bool RunJob() {
  ResetEvent(g_doneEvent);
  SetEvent(g_requestEvent);
  return WaitForSingleObject(g_doneEvent, kReportTimeoutMs) == WAIT_OBJECT_0;
}

void Report(EXCEPTION_POINTERS* ep, bool unhandled) {
  if (!g_installed.load(std::memory_order_acquire)) return;
  const DWORD tid = GetCurrentThreadId();
  if (tid == g_reporterTid.load(std::memory_order_relaxed)) return;
  bool expected = false;
  if (!g_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

  const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
  const auto pc = static_cast<uintptr_t>(ep->ContextRecord->Rip);
  const auto address = reinterpret_cast<uintptr_t>(rec->ExceptionAddress);

  if (unhandled && g_haveLast && tid == g_lastThread && rec->ExceptionCode == g_lastCode && address == g_lastAddress) {
    // The exception the VEH reported first-chance reached the unhandled filter: it is a real crash.
    if (!g_lastUnhandled) {
      g_req.job = Job::MarkUnhandled;
      if (!RunJob()) return;
      g_lastUnhandled = true;
    }
    g_busy.store(false, std::memory_order_release);
    return;
  }

  bool skip = g_reportCount.load(std::memory_order_relaxed) >= kMaxReports;
  // A first-chance fault that something up the stack keeps handling must not report every time.
  if (!unhandled)
    for (size_t i = 0; i < g_seenCount && !skip; ++i)
      if (g_seenPcs[i] == pc) skip = true;
  if (skip) {
    g_busy.store(false, std::memory_order_release);
    return;
  }

  g_req.job = Job::Report;
  std::memcpy(&g_req.record, rec, sizeof(EXCEPTION_RECORD));
  g_req.record.ExceptionRecord = nullptr;   // never chase the nested-record chain
  std::memcpy(&g_req.context, ep->ContextRecord, sizeof(CONTEXT));
  g_req.pointers.ExceptionRecord = &g_req.record;
  g_req.pointers.ContextRecord = &g_req.context;
  g_req.threadId = tid;
  g_req.stackBase = reinterpret_cast<uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
  g_req.unhandled = unhandled;
  g_req.reported = false;
  if (!RunJob()) return;

  if (g_req.reported) {
    g_reportCount.fetch_add(1, std::memory_order_relaxed);
    g_haveLast = true;
    g_lastUnhandled = unhandled;
    g_lastThread = tid;
    g_lastCode = rec->ExceptionCode;
    g_lastAddress = address;
    if (!unhandled && g_seenCount < kMaxSeenPcs) g_seenPcs[g_seenCount++] = pc;
  }
  g_busy.store(false, std::memory_order_release);
}

LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* ep) {
  if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
#if defined(__GNUC__) && !defined(_MSC_VER)
  if (g_walkArmed.load(std::memory_order_acquire) && GetCurrentThreadId() == g_reporterTid.load() &&
      (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)) {
    // The reporter's stack walk touched bad memory: resume it in WalkRecovery() on a fresh,
    // 16-byte aligned spot of its own stack (entry RSP must be 8 mod 16, as after a call).
    g_walkArmed.store(false, std::memory_order_relaxed);
    CONTEXT* c = ep->ContextRecord;
    c->Rsp = ((c->Rsp & ~static_cast<DWORD64>(15)) - 128) - 8;
    c->Rip = reinterpret_cast<DWORD64>(&WalkRecovery);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
#endif
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW)
    return EXCEPTION_CONTINUE_SEARCH;
  const auto pc = static_cast<uintptr_t>(ep->ContextRecord->Rip);
  if (pc < g_selfBase || pc >= g_selfEnd) return EXCEPTION_CONTINUE_SEARCH;
  Report(ep, /*unhandled=*/false);
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
  if (ep && ep->ExceptionRecord && ep->ContextRecord) Report(ep, /*unhandled=*/true);
  const LPTOP_LEVEL_EXCEPTION_FILTER prev = g_prevFilter.load(std::memory_order_acquire);
  if (prev && prev != &UnhandledFilter) return prev(ep);
  return EXCEPTION_CONTINUE_SEARCH;
}

void CloseHandles() {
  for (HANDLE* h : {&g_thread, &g_requestEvent, &g_doneEvent, &g_quitEvent}) {
    if (*h) CloseHandle(*h);
    *h = nullptr;
  }
}

}  // namespace

void Install() {
  try {
    std::lock_guard lock(InstallMutex());
    if (g_installed.load(std::memory_order_acquire)) return;

    const auto self = reinterpret_cast<uintptr_t>(paths::Self());
    if (!self) return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(self + static_cast<uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    g_selfBase = self;
    g_selfEnd = self + nt->OptionalHeader.SizeOfImage;
    g_tryHandler = FindTryHandler();

    if (!g_dbghelp) {
      wchar_t sysDir[MAX_PATH];
      const UINT n = GetSystemDirectoryW(sysDir, MAX_PATH);
      if (n > 0 && n < MAX_PATH) {
        // Load from System32 explicitly: a dbghelp.dll next to the game exe must not be picked up.
        const std::wstring path = std::wstring(sysDir, n) + L"\\dbghelp.dll";
        g_dbghelp = LoadLibraryW(path.c_str());
      }
      if (g_dbghelp)
        g_miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(
            reinterpret_cast<void*>(GetProcAddress(g_dbghelp, "MiniDumpWriteDump")));
      else
        log::Warn("crash", "dbghelp.dll not available; crash reports will not include a minidump");
    }

    g_requestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_quitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD tid = 0;
    if (g_requestEvent && g_doneEvent && g_quitEvent)
      g_thread = CreateThread(nullptr, 512 * 1024, ReporterMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
    if (!g_thread) {
      log::Error("crash", "could not start the crash reporter thread (error {})", GetLastError());
      CloseHandles();
      return;
    }
    g_reporterTid.store(tid, std::memory_order_relaxed);
    g_busy.store(false, std::memory_order_relaxed);
    g_installed.store(true, std::memory_order_release);

    g_veh = AddVectoredExceptionHandler(0, &VectoredHandler);
    g_prevFilter.store(SetUnhandledExceptionFilter(&UnhandledFilter), std::memory_order_release);
    log::Info("crash", "crash reporting active (reports go to {})", util::Narrow(paths::Data(L"crash").wstring()));
  } catch (...) {
  }
}

void Uninstall() {
  try {
    std::lock_guard lock(InstallMutex());
    if (!g_installed.load(std::memory_order_acquire)) return;
    g_installed.store(false, std::memory_order_release);

    if (g_veh) {
      RemoveVectoredExceptionHandler(g_veh);
      g_veh = nullptr;
    }
    const LPTOP_LEVEL_EXCEPTION_FILTER prev = g_prevFilter.load(std::memory_order_acquire);
    const LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(prev);
    // Someone installed a filter after us: leave theirs in place rather than reverting it.
    if (current != &UnhandledFilter) SetUnhandledExceptionFilter(current);

    // Take the request slot so no handler that slipped past the g_installed check is still using the
    // events; a handler that timed out keeps the slot forever, and then the handles must stay open.
    bool claimed = false;
    for (DWORD waited = 0; waited <= kUninstallWaitMs; waited += 10) {
      bool expected = false;
      if (g_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        claimed = true;
        break;
      }
      Sleep(10);
    }
    if (!claimed) {
      log::Warn("crash", "a crash report is still in progress; reporter left running");
      return;
    }
    SetEvent(g_quitEvent);
    if (WaitForSingleObject(g_thread, kUninstallWaitMs) != WAIT_OBJECT_0) {
      log::Warn("crash", "crash reporter thread did not stop in time");
      return;
    }
    g_reporterTid.store(0, std::memory_order_relaxed);
    CloseHandles();
    g_busy.store(false, std::memory_order_release);
    if (g_dbghelp) {
      FreeLibrary(g_dbghelp);
      g_dbghelp = nullptr;
      g_miniDumpWriteDump = nullptr;
    }
  } catch (...) {
  }
}

}  // namespace cg::crash
