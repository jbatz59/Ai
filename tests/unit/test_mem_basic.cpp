// Unit tests for the basic memory layer: module, safe, pattern, value, expr, disasm, siggen.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>

#include "core/mp_guard.h"
#include "mem/disasm.h"
#include "mem/expr.h"
#include "mem/module.h"
#include "mem/pattern.h"
#include "mem/safe.h"
#include "mem/siggen.h"
#include "mem/value.h"
#include "testing.h"

#ifdef _MSC_VER
#define CG_TEST_NOINLINE __declspec(noinline)
#else
#define CG_TEST_NOINLINE __attribute__((noinline))
#endif

namespace mem = cg::mem;

namespace {

CG_TEST_NOINLINE uint32_t SigTargetFn(uint32_t x) {
  uint32_t h = x ^ 0x5A17u;
  for (uint32_t i = 0; i < (x & 7u) + 3u; ++i) h = (h << 5) ^ (h >> 3) ^ (i * 0x3Du);
  return h * 0x2F1u + 0x11u;
}

alignas(64) const char kXrefMarker[] = "cg-mem-basic-xref-marker-5e1d";
alignas(64) const wchar_t kWideMarker[] = L"cg-mem-basic-wide-marker-77";

CG_TEST_NOINLINE const char* XrefMarkerUser() { return kXrefMarker; }
CG_TEST_NOINLINE const wchar_t* WideMarkerUser() { return kWideMarker; }

uintptr_t Addr(const void* p) { return reinterpret_cast<uintptr_t>(p); }

// MSVC incremental linking hands out "jmp rel32" thunks for function addresses: follow them.
template <class F> uintptr_t CodeAddress(F* fn) {
  const uintptr_t a = reinterpret_cast<uintptr_t>(fn);
  uint8_t first = 0;
  if (mem::ReadRaw(a, &first, 1) && first == 0xE9)
    if (auto t = mem::ResolveCall(a)) return *t;
  return a;
}

std::string ToLowerAscii(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

struct PageBlock {
  explicit PageBlock(size_t pages, DWORD protect)
      : size(pages * 0x1000), base(static_cast<uint8_t*>(VirtualAlloc(nullptr, pages * 0x1000, MEM_COMMIT | MEM_RESERVE, protect))) {}
  ~PageBlock() {
    if (base) VirtualFree(base, 0, MEM_RELEASE);
  }
  PageBlock(const PageBlock&) = delete;
  PageBlock& operator=(const PageBlock&) = delete;
  size_t size;
  uint8_t* base;
};

DWORD ProtectionOf(const void* p) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return 0;
  return mbi.Protect;
}

}  // namespace

// ---- module ----------------------------------------------------------------------------------

CG_TEST(mem_module_main_and_lookup) {
  const mem::Module& main = mem::Module::Main();
  REQUIRE(main.Valid());
  CHECK_EQ(main.base, Addr(GetModuleHandleW(nullptr)));
  CHECK(main.size > 0);
  CHECK(main.name.size() > 4);
  CHECK_EQ(main.name, ToLowerAscii(main.name));
  CHECK(main.name.ends_with(".exe"));
  CHECK(main.FindSection(".text") != nullptr);

  const uintptr_t code = CodeAddress(&SigTargetFn);
  CHECK(main.Contains(code));
  const mem::Section* sec = main.SectionOf(code);
  REQUIRE(sec != nullptr);
  CHECK(sec->Executable());
  CHECK(!sec->Writable());

  auto found = mem::Module::Find(main.name);
  REQUIRE(found.has_value());
  CHECK_EQ(found->base, main.base);
  std::string upper = main.name;
  for (char& c : upper)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  auto foundUpper = mem::Module::Find(upper);
  REQUIRE(foundUpper.has_value());
  CHECK_EQ(foundUpper->base, main.base);

  auto k32 = mem::Module::Find("kernel32.dll");
  REQUIRE(k32.has_value());
  CHECK_EQ(k32->name, std::string("kernel32.dll"));
  CHECK(!mem::Module::Find("cg-definitely-not-loaded-module.dll").has_value());

  auto containing = mem::Module::Containing(code);
  REQUIRE(containing.has_value());
  CHECK_EQ(containing->base, main.base);
  CHECK(!mem::Module::Containing(0x1234).has_value());

  bool sawMain = false, sawKernel32 = false;
  for (const mem::Module& m : mem::Module::All()) {
    sawMain = sawMain || m.base == main.base;
    sawKernel32 = sawKernel32 || m.name == "kernel32.dll";
  }
  CHECK(sawMain);
  CHECK(sawKernel32);

  CHECK_EQ(mem::Describe(main.base + 0x1000), main.name + "+0x1000");
  CHECK_EQ(mem::Describe(0x1234), std::string("0x1234"));

  auto exp = mem::FindExport("kernel32.dll", "GetTickCount64");
  REQUIRE(exp.has_value());
  CHECK_EQ(*exp, reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTickCount64")));
  CHECK(mem::FindExport("kernel32", "GetTickCount64").has_value());
  CHECK(!mem::FindExport("kernel32.dll", "CgNoSuchExport").has_value());
  CHECK(!mem::FindExport("cg-not-loaded.dll", "Sleep").has_value());
}

CG_TEST(mem_module_from_handle_rejects_garbage) {
  CHECK(!mem::Module::FromHandle(nullptr).Valid());
  std::vector<uint8_t> junk(4096, 0x41);
  CHECK(!mem::Module::FromHandle(reinterpret_cast<HMODULE>(junk.data())).Valid());
  // A fake DOS header whose e_lfanew points at garbage must be rejected too.
  IMAGE_DOS_HEADER dos{};
  dos.e_magic = IMAGE_DOS_SIGNATURE;
  dos.e_lfanew = 0x80;
  std::memcpy(junk.data(), &dos, sizeof(dos));
  CHECK(!mem::Module::FromHandle(reinterpret_cast<HMODULE>(junk.data())).Valid());
}

// ---- safe ------------------------------------------------------------------------------------

CG_TEST(mem_safe_rejects_bad_addresses) {
  int v = 0;
  CHECK(!mem::ReadRaw(0, &v, sizeof(v)));
  CHECK(!mem::ReadRaw(1, &v, sizeof(v)));
  CHECK(!mem::Read<int>(0).has_value());
  CHECK(!mem::Read<int>(1).has_value());
  CHECK_EQ(mem::ReadOr<int>(0x10, 7), 7);
  CHECK(!mem::ReadPtr(0).has_value());
  CHECK(!mem::IsReadable(0, 1));
  CHECK(!mem::IsReadable(0x7FFFFFFFFFFFull, 16));
  CHECK(!mem::ReadRaw(UINTPTR_MAX - 2, &v, sizeof(v)));
  CHECK(!mem::Write<int>(0, 1));
  CHECK(!mem::Write<int>(1, 1));
}

CG_TEST(mem_safe_region_queries) {
  int local = 5;
  CHECK(mem::IsReadable(Addr(&local), sizeof(local)));
  CHECK(mem::IsWritable(Addr(&local), sizeof(local)));
  CHECK(!mem::IsExecutable(Addr(&local)));
  const uintptr_t code = CodeAddress(&SigTargetFn);
  CHECK(mem::IsExecutable(code));
  CHECK(mem::IsReadable(code, 16));
  CHECK(!mem::IsWritable(code, 16));

  PageBlock noAccess(1, PAGE_NOACCESS);
  REQUIRE(noAccess.base != nullptr);
  CHECK(!mem::IsReadable(Addr(noAccess.base), 1));
  uint8_t b = 0;
  CHECK(!mem::ReadRaw(Addr(noAccess.base), &b, 1));

  // A read spanning a readable page and an unreadable one must fail as a whole.
  PageBlock two(2, PAGE_READWRITE);
  REQUIRE(two.base != nullptr);
  DWORD old = 0;
  REQUIRE(VirtualProtect(two.base + 0x1000, 0x1000, PAGE_NOACCESS, &old));
  mem::InvalidateRegionCache();
  CHECK(mem::IsReadable(Addr(two.base), 0x1000));
  CHECK(!mem::IsReadable(Addr(two.base) + 0xFF0, 0x20));
  uint8_t buf[0x20];
  CHECK(!mem::ReadRaw(Addr(two.base) + 0xFF0, buf, sizeof(buf)));
  CHECK(mem::ReadRaw(Addr(two.base) + 0xFE0, buf, sizeof(buf)));

  // Guard pages are never touched (touching would consume the guard).
  PageBlock guard(1, PAGE_READWRITE | PAGE_GUARD);
  REQUIRE(guard.base != nullptr);
  CHECK(!mem::ReadRaw(Addr(guard.base), &b, 1));
  CHECK((ProtectionOf(guard.base) & PAGE_GUARD) != 0);
}

CG_TEST(mem_safe_write_read_roundtrip) {
  auto* heap = new int64_t(5);
  const uintptr_t a = Addr(heap);
  if (cg::mp_guard::Blocked()) {
    CHECK(!mem::Write<int64_t>(a, 77));
    delete heap;
    return;
  }
  CHECK(mem::Write<int64_t>(a, 0x1122334455667788ll));
  CHECK_EQ(*heap, 0x1122334455667788ll);
  auto r = mem::Read<int64_t>(a);
  REQUIRE(r.has_value());
  CHECK_EQ(*r, 0x1122334455667788ll);
  delete heap;

  // Read-only and executable pages are written through a temporary protection change that is
  // restored afterwards.
  PageBlock ro(1, PAGE_READONLY);
  REQUIRE(ro.base != nullptr);
  CHECK(mem::Write<uint32_t>(Addr(ro.base) + 8, 0xC0FFEEu));
  CHECK_EQ(mem::ReadOr<uint32_t>(Addr(ro.base) + 8, 0), 0xC0FFEEu);
  CHECK_EQ(ProtectionOf(ro.base), static_cast<DWORD>(PAGE_READONLY));

  PageBlock rx(1, PAGE_EXECUTE_READ);
  REQUIRE(rx.base != nullptr);
  const uint8_t ret[] = {0xC3};
  CHECK(mem::WriteRaw(Addr(rx.base), ret, sizeof(ret)));
  CHECK_EQ(mem::ReadOr<uint8_t>(Addr(rx.base), 0), static_cast<uint8_t>(0xC3));
  CHECK_EQ(ProtectionOf(rx.base), static_cast<DWORD>(PAGE_EXECUTE_READ));

  PageBlock na(1, PAGE_NOACCESS);
  REQUIRE(na.base != nullptr);
  CHECK(!mem::Write<uint32_t>(Addr(na.base), 1));
  CHECK_EQ(ProtectionOf(na.base), static_cast<DWORD>(PAGE_NOACCESS));

  CHECK(mem::WriteRaw(a, nullptr, 0));
}

CG_TEST(mem_safe_deref_chain) {
  struct C { int32_t pad; int32_t value; };
  struct B { uint8_t pad[0x10]; C* c; };
  struct A { uint8_t pad[0x8]; B* b; };
  C c{0, 1234};
  B b{};
  b.c = &c;
  A a{};
  a.b = &b;

  CHECK_EQ(mem::Deref(Addr(&a), {}).value_or(0), Addr(&a));
  CHECK_EQ(mem::Deref(Addr(&a), {0x8}).value_or(0), Addr(&a) + 0x8);
  CHECK_EQ(mem::Deref(Addr(&a), {0x8, 0x10}).value_or(0), Addr(&b) + 0x10);
  const auto field = mem::Deref(Addr(&a), {0x8, 0x10, 0x4});
  REQUIRE(field.has_value());
  CHECK_EQ(*field, Addr(&c.value));
  CHECK_EQ(mem::ReadOr<int32_t>(*field, 0), 1234);
  const std::vector<int64_t> offs = {0x8, 0x10, 0x4};
  CHECK_EQ(mem::Deref(Addr(&a), std::span<const int64_t>(offs)).value_or(0), Addr(&c.value));
  CHECK_EQ(mem::Deref(Addr(&b) + 0x20, {-0x10, 4}).value_or(0), Addr(&c.value));

  b.c = nullptr;
  CHECK(!mem::Deref(Addr(&a), {0x8, 0x10, 0x4}).has_value());
  b.c = reinterpret_cast<C*>(uintptr_t{0x20});
  CHECK(!mem::Deref(Addr(&a), {0x8, 0x10, 0x4}).has_value());
}

CG_TEST(mem_safe_strings) {
  const char s[] = "hello world";
  CHECK_EQ(mem::ReadCString(Addr(s)), std::string("hello world"));
  CHECK_EQ(mem::ReadCString(Addr(s), 5), std::string("hello"));
  CHECK_EQ(mem::ReadCString(0), std::string());
  const wchar_t w[] = L"wide text";
  CHECK(mem::ReadWString(Addr(w)) == std::wstring(L"wide text"));
  CHECK(mem::ReadWString(Addr(w), 4) == std::wstring(L"wide"));

  // Unterminated strings that run into an unreadable page end at the page boundary.
  PageBlock two(2, PAGE_READWRITE);
  REQUIRE(two.base != nullptr);
  DWORD old = 0;
  REQUIRE(VirtualProtect(two.base + 0x1000, 0x1000, PAGE_NOACCESS, &old));
  mem::InvalidateRegionCache();
  std::memcpy(two.base + 0x1000 - 3, "abc", 3);
  CHECK_EQ(mem::ReadCString(Addr(two.base) + 0x1000 - 3, 100), std::string("abc"));
  std::memcpy(two.base + 0x1000 - 4, L"xy", 4);
  CHECK(mem::ReadWString(Addr(two.base) + 0x1000 - 4, 100) == std::wstring(L"xy"));
}

// ---- pattern ---------------------------------------------------------------------------------

CG_TEST(mem_pattern_parse_and_tostring) {
  auto p = mem::Pattern::Parse("48 8B ?? ? 4? ?F");
  REQUIRE(p.has_value());
  CHECK_EQ(p->Size(), size_t{6});
  CHECK(p->bytes == (std::vector<uint8_t>{0x48, 0x8B, 0x00, 0x00, 0x40, 0x0F}));
  CHECK(p->mask == (std::vector<uint8_t>{0xFF, 0xFF, 0x00, 0x00, 0xF0, 0x0F}));
  CHECK_EQ(p->ToString(), std::string("48 8B ?? ?? 4? ?F"));

  auto lower = mem::Pattern::Parse("  e8\t?? ?? ?? ??\n c3 ");
  REQUIRE(lower.has_value());
  CHECK_EQ(lower->ToString(), std::string("E8 ?? ?? ?? ?? C3"));
  auto roundTrip = mem::Pattern::Parse(p->ToString());
  REQUIRE(roundTrip.has_value());
  CHECK(roundTrip->bytes == p->bytes);
  CHECK(roundTrip->mask == p->mask);
}

CG_TEST(mem_pattern_rejects_malformed) {
  CHECK(!mem::Pattern::Parse("").has_value());
  CHECK(!mem::Pattern::Parse("   ").has_value());
  CHECK(!mem::Pattern::Parse("4").has_value());
  CHECK(!mem::Pattern::Parse("48 8").has_value());
  CHECK(!mem::Pattern::Parse("123").has_value());
  CHECK(!mem::Pattern::Parse("GG").has_value());
  CHECK(!mem::Pattern::Parse("48,8B").has_value());
  CHECK(!mem::Pattern::Parse("??? 48").has_value());
  CHECK(!mem::Pattern::Parse("0x48").has_value());
}

CG_TEST(mem_pattern_matches_nibbles) {
  auto p = mem::Pattern::Parse("48 8B ?? 4? ?F");
  REQUIRE(p.has_value());
  const uint8_t yes1[] = {0x48, 0x8B, 0x12, 0x4A, 0x0F};
  const uint8_t yes2[] = {0x48, 0x8B, 0xFF, 0x40, 0xFF};
  const uint8_t no1[] = {0x48, 0x8B, 0x12, 0x5A, 0x0F};
  const uint8_t no2[] = {0x48, 0x8B, 0x12, 0x4A, 0x0E};
  const uint8_t no3[] = {0x49, 0x8B, 0x12, 0x4A, 0x0F};
  CHECK(p->Matches(yes1));
  CHECK(p->Matches(yes2));
  CHECK(!p->Matches(no1));
  CHECK(!p->Matches(no2));
  CHECK(!p->Matches(no3));
}

CG_TEST(mem_pattern_find_all_heap_and_chunk_boundary) {
  constexpr size_t kMiB = 1u << 20;
  std::vector<uint8_t> buf(3 * kMiB + 123, 0);
  const uint8_t sig[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x13, 0x37};
  const size_t offsets[] = {0, 100, kMiB - 3, kMiB + 8, 2 * kMiB - 1, buf.size() - sizeof(sig)};
  for (size_t off : offsets) std::memcpy(buf.data() + off, sig, sizeof(sig));

  auto p = mem::Pattern::Parse("DE AD ?? EF 1? ?7");
  REQUIRE(p.has_value());
  const uintptr_t base = Addr(buf.data());
  auto hits = mem::FindAll(*p, base, buf.size());
  REQUIRE(hits.size() == std::size(offsets));
  for (size_t i = 0; i < hits.size(); ++i) CHECK_EQ(hits[i], base + offsets[i]);

  auto limited = mem::FindAll(*p, base, buf.size(), 2);
  CHECK_EQ(limited.size(), size_t{2});
  auto first = mem::FindFirst(*p, base + 1, buf.size() - 1);
  REQUIRE(first.has_value());
  CHECK_EQ(*first, base + 100);
  // A match that does not fit entirely inside the range is not reported.
  CHECK(!mem::FindFirst(*p, base + 1, 100 + sizeof(sig) - 2).has_value());

  std::atomic<bool> cancel{true};
  CHECK(mem::FindAll(*p, base, buf.size(), SIZE_MAX, &cancel).empty());

  // Wildcard-only patterns still work (no memchr anchor available).
  auto any = mem::Pattern::Parse("?? ??");
  REQUIRE(any.has_value());
  CHECK_EQ(mem::FindAll(*any, base, 10).size(), size_t{9});

  // Scanning over unreadable memory is safe and skips it.
  PageBlock two(2, PAGE_READWRITE);
  REQUIRE(two.base != nullptr);
  std::memcpy(two.base + 0x10, sig, sizeof(sig));
  DWORD old = 0;
  REQUIRE(VirtualProtect(two.base + 0x1000, 0x1000, PAGE_NOACCESS, &old));
  auto guarded = mem::FindAll(*p, Addr(two.base), two.size);
  REQUIRE(guarded.size() == 1);
  CHECK_EQ(guarded[0], Addr(two.base) + 0x10);
  CHECK(mem::FindAll(*p, 0, 0x10000).empty());
}

CG_TEST(mem_pattern_resolve_rip_and_call) {
  uint8_t code[32] = {0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00,   // mov rax, [rip+0x10]
                      0xE8, 0xF0, 0xFF, 0xFF, 0xFF,               // call -0x10
                      0xE9, 0x00, 0x01, 0x00, 0x00,               // jmp +0x100
                      0x90};
  const uintptr_t a = Addr(code);
  CHECK_EQ(mem::ResolveRip(a, 3, 7).value_or(0), a + 7 + 0x10);
  CHECK_EQ(mem::ResolveCall(a + 7).value_or(0), a + 7 + 5 - 0x10);
  CHECK_EQ(mem::ResolveCall(a + 12).value_or(0), a + 12 + 5 + 0x100);
  CHECK(!mem::ResolveCall(a).has_value());
  CHECK(!mem::ResolveCall(0).has_value());
  CHECK(!mem::ResolveRip(0, 3, 7).has_value());
  CHECK(!mem::ResolveRip(a, 5, 7).has_value());
}

CG_TEST(mem_pattern_module_strings_and_xrefs) {
  const mem::Module& main = mem::Module::Main();
  const char* (*volatile user)() = &XrefMarkerUser;
  CHECK(user() == kXrefMarker);
  const wchar_t* (*volatile wideUser)() = &WideMarkerUser;
  CHECK(wideUser() == kWideMarker);
  const uintptr_t marker = Addr(kXrefMarker);

  uint8_t before = 1;
  if (mem::ReadRaw(marker - 1, &before, 1) && before == 0) {
    auto found = mem::FindString(main, kXrefMarker);
    REQUIRE(found.has_value());
    CHECK_EQ(mem::ReadCString(*found), std::string(kXrefMarker));
  }
  uint16_t wideBefore = 1;
  if (mem::ReadRaw(Addr(kWideMarker) - 2, &wideBefore, 2) && wideBefore == 0) {
    auto found = mem::FindString(main, "cg-mem-basic-wide-marker-77", true);
    REQUIRE(found.has_value());
    CHECK(mem::ReadWString(*found) == std::wstring(kWideMarker));
  }
  // Built at run time so the searched text itself is not a literal in the binary.
  std::string absent = "Xg-this-string-is-not-in-the-binary-91";
  volatile char first = 'c';
  absent[0] = first;
  CHECK(!mem::FindString(main, absent).has_value());
  CHECK(!mem::FindString(main, "").has_value());

  const auto xrefs = mem::FindXrefs(main, marker);
  REQUIRE(!xrefs.empty());
  const uintptr_t userCode = CodeAddress(&XrefMarkerUser);
  bool inUser = false;
  for (uintptr_t x : xrefs) {
    mem::Insn insn;
    REQUIRE(mem::Decode(x, insn));
    CHECK_EQ(insn.target, marker);
    inUser = inUser || (x >= userCode && x < userCode + 32);
  }
  CHECK(inUser);
  std::atomic<bool> cancel{true};
  CHECK(mem::FindXrefs(main, marker, 256, &cancel).empty());
}

CG_TEST(mem_pattern_function_start) {
  const uintptr_t fn = CodeAddress(&SigTargetFn);
  CHECK_EQ(mem::FunctionStart(fn).value_or(0), fn);
  CHECK_EQ(mem::FunctionStart(fn + 1).value_or(0), fn);
  CHECK(!mem::FunctionStart(0).has_value());
  int local = 0;
  CHECK(!mem::FunctionStart(Addr(&local)).has_value());
}

// ---- value -----------------------------------------------------------------------------------

CG_TEST(mem_value_names_and_sizes) {
  for (mem::ValueType t : mem::kAllValueTypes) {
    auto back = mem::ParseValueType(mem::ValueTypeName(t));
    REQUIRE(back.has_value());
    CHECK(*back == t);
  }
  CHECK(mem::ParseValueType("I32") == mem::ValueType::I32);
  CHECK(mem::ParseValueType("WString") == mem::ValueType::WString);
  CHECK(!mem::ParseValueType("int").has_value());
  CHECK_EQ(mem::ValueSize(mem::ValueType::I16), size_t{2});
  CHECK_EQ(mem::ValueSize(mem::ValueType::F64), size_t{8});
  CHECK_EQ(mem::ValueSize(mem::ValueType::Bytes), size_t{0});
  CHECK(mem::IsNumeric(mem::ValueType::U8));
  CHECK(!mem::IsNumeric(mem::ValueType::String));
  CHECK(mem::IsFloat(mem::ValueType::F32));
  CHECK(!mem::IsFloat(mem::ValueType::I64));
}

CG_TEST(mem_value_roundtrip_every_type) {
  struct Case {
    mem::ValueType type;
    const char* text;
  };
  const Case cases[] = {
      {mem::ValueType::I8, "-5"},       {mem::ValueType::I16, "-1234"},
      {mem::ValueType::I32, "123456"},  {mem::ValueType::I64, "-9000000000"},
      {mem::ValueType::U8, "200"},      {mem::ValueType::U16, "65535"},
      {mem::ValueType::U32, "4000000000"}, {mem::ValueType::U64, "18446744073709551615"},
      {mem::ValueType::F32, "1.5"},     {mem::ValueType::F64, "-2.25"},
      {mem::ValueType::String, "hello"}, {mem::ValueType::WString, "h\xC3\xA9llo"},
      {mem::ValueType::Bytes, "90 90 C3"},
  };
  static_assert(std::size(cases) == std::size(mem::kAllValueTypes));
  for (const Case& c : cases) {
    std::vector<uint8_t> bytes;
    REQUIRE(mem::ParseValue(c.type, c.text, bytes));
    if (mem::IsNumeric(c.type)) CHECK_EQ(bytes.size(), mem::ValueSize(c.type));
    CHECK_EQ(mem::FormatValue(c.type, bytes.data(), bytes.size()), std::string(c.text));
    if (mem::IsNumeric(c.type) && !mem::IsFloat(c.type)) {
      const std::string hex = mem::FormatValue(c.type, bytes.data(), bytes.size(), true);
      std::vector<uint8_t> again;
      REQUIRE(mem::ParseValue(c.type, hex, again));
      CHECK(again == bytes);
    }
  }
  std::vector<uint8_t> w;
  REQUIRE(mem::ParseValue(mem::ValueType::WString, "ab", w));
  CHECK(w == (std::vector<uint8_t>{'a', 0, 'b', 0}));
}

CG_TEST(mem_value_parse_rules) {
  std::vector<uint8_t> b;
  CHECK(mem::ParseValue(mem::ValueType::I32, " 0x10 ", b));
  CHECK_EQ(mem::FormatValue(mem::ValueType::I32, b.data(), b.size()), std::string("16"));
  CHECK(mem::ParseValue(mem::ValueType::I8, "0xFF", b));
  CHECK_EQ(mem::FormatValue(mem::ValueType::I8, b.data(), b.size()), std::string("-1"));
  CHECK_EQ(mem::FormatValue(mem::ValueType::I8, b.data(), b.size(), true), std::string("0xFF"));
  CHECK(mem::ParseValue(mem::ValueType::I8, "-128", b));
  CHECK(!mem::ParseValue(mem::ValueType::I8, "128", b));
  CHECK(!mem::ParseValue(mem::ValueType::I8, "-129", b));
  CHECK(!mem::ParseValue(mem::ValueType::U8, "256", b));
  CHECK(!mem::ParseValue(mem::ValueType::U32, "-1", b));
  CHECK(!mem::ParseValue(mem::ValueType::I32, "12abc", b));
  CHECK(!mem::ParseValue(mem::ValueType::I32, "", b));
  CHECK(!mem::ParseValue(mem::ValueType::F32, "abc", b));
  CHECK(!mem::ParseValue(mem::ValueType::F32, "1e40", b));
  CHECK(mem::ParseValue(mem::ValueType::F32, "+2.5", b));
  CHECK(!mem::ParseValue(mem::ValueType::Bytes, "9", b));
  CHECK(!mem::ParseValue(mem::ValueType::Bytes, "zz", b));
  CHECK(!mem::ParseValue(mem::ValueType::Bytes, "90 ??", b));
  CHECK(mem::ParseValue(mem::ValueType::Bytes, "9090 c3", b));
  CHECK(b == (std::vector<uint8_t>{0x90, 0x90, 0xC3}));
  CHECK(!mem::ParseValue(mem::ValueType::String, "", b));
}

CG_TEST(mem_value_format_floats_and_live_memory) {
  const float f1 = 100.0f, f2 = 1.5f, f3 = 0.000123f;
  CHECK_EQ(mem::FormatValue(mem::ValueType::F32, &f1, 4), std::string("100"));
  CHECK_EQ(mem::FormatValue(mem::ValueType::F32, &f2, 4), std::string("1.5"));
  CHECK_EQ(mem::FormatValue(mem::ValueType::F32, &f3, 4), std::string("0.000123"));
  const double d1 = 1234.5678, d2 = -0.0, d3 = 1.0e-9;
  CHECK_EQ(mem::FormatValue(mem::ValueType::F64, &d1, 8), std::string("1234.5678"));
  CHECK_EQ(mem::FormatValue(mem::ValueType::F64, &d2, 8), std::string("0"));
  CHECK_EQ(mem::FormatValue(mem::ValueType::F64, &d3, 8), std::string("1e-09"));
  CHECK_EQ(mem::FormatValue(mem::ValueType::F64, &d1, 4), std::string("??"));

  int32_t live = -42;
  CHECK_EQ(mem::ReadFormatted(Addr(&live), mem::ValueType::I32), std::string("-42"));
  CHECK_EQ(mem::ReadFormatted(Addr(&live), mem::ValueType::I32, 0, true), std::string("0xFFFFFFD6"));
  CHECK_EQ(mem::ReadFormatted(0, mem::ValueType::I32), std::string("??"));
  const char text[] = "abc\x01z";
  CHECK_EQ(mem::ReadFormatted(Addr(text), mem::ValueType::String, 16), std::string("abc.z"));
  CHECK_EQ(mem::ReadFormatted(Addr(text), mem::ValueType::Bytes, 3), std::string("61 62 63"));
  CHECK_EQ(mem::ReadFormatted(0, mem::ValueType::String), std::string("??"));
  const wchar_t wtext[] = L"wé";
  CHECK_EQ(mem::ReadFormatted(Addr(wtext), mem::ValueType::WString, 16), std::string("w\xC3\xA9"));

  PageBlock two(2, PAGE_READWRITE);
  REQUIRE(two.base != nullptr);
  DWORD old = 0;
  two.base[0xFFF] = 0xAB;
  REQUIRE(VirtualProtect(two.base + 0x1000, 0x1000, PAGE_NOACCESS, &old));
  mem::InvalidateRegionCache();
  CHECK_EQ(mem::ReadFormatted(Addr(two.base) + 0xFFF, mem::ValueType::Bytes, 3), std::string("AB ?? ??"));
}

// ---- expr ------------------------------------------------------------------------------------

CG_TEST(mem_expr_numbers_and_arithmetic) {
  auto ev = [](const char* e) { return mem::EvalAddress(e); };
  CHECK_EQ(ev("10").value, uintptr_t{0x10});
  CHECK_EQ(ev("0x7FF6A1B20000").value, uintptr_t{0x7FF6A1B20000});
  CHECK_EQ(ev("7FF6A1B20000").value, uintptr_t{0x7FF6A1B20000});
  CHECK_EQ(ev("#16").value, uintptr_t{16});
  CHECK_EQ(ev("2+3*4").value, uintptr_t{14});
  CHECK_EQ(ev("(2+3)*4").value, uintptr_t{20});
  CHECK_EQ(ev("a + 16*4 - 0x10").value, uintptr_t{0xA + 0x16 * 4 - 0x10});
  CHECK_EQ(ev("#10 * #10").value, uintptr_t{100});
  CHECK_EQ(ev("-1").value, ~uintptr_t{0});
  CHECK_EQ(ev("10 - -2").value, uintptr_t{0x12});
  CHECK(ev("  1  ").ok);
}

CG_TEST(mem_expr_deref_and_resolver) {
  uintptr_t inner = 0x1234;
  uintptr_t outer = Addr(&inner);
  const std::string innerHex = std::format("{:X}", Addr(&inner));
  const std::string outerHex = std::format("{:X}", Addr(&outer));

  auto r1 = mem::EvalAddress("[" + innerHex + "]+8");
  REQUIRE(r1.ok);
  CHECK_EQ(r1.value, uintptr_t{0x123C});
  auto r2 = mem::EvalAddress("[[" + outerHex + "]] + #2");
  REQUIRE(r2.ok);
  CHECK_EQ(r2.value, uintptr_t{0x1236});

  mem::SymbolResolver resolver = [&](std::string_view name) -> std::optional<uintptr_t> {
    if (name == "Player.Ptr") return Addr(&outer);
    if (name == "dead") return uintptr_t{0x42};   // resolver symbols win over bare hex
    return std::nullopt;
  };
  auto r3 = mem::EvalAddress("[[Player.Ptr]]+0x78", resolver);
  REQUIRE(r3.ok);
  CHECK_EQ(r3.value, uintptr_t{0x1234 + 0x78});
  CHECK_EQ(mem::EvalAddress("dead", resolver).value, uintptr_t{0x42});
  CHECK_EQ(mem::EvalAddress("dead").value, uintptr_t{0xDEAD});
  CHECK_EQ(mem::EvalAddress("\"Player.Ptr\"", resolver).value, Addr(&outer));

  mem::SymbolResolver throwing = [](std::string_view) -> std::optional<uintptr_t> { throw std::runtime_error("boom"); };
  auto r4 = mem::EvalAddress("Foo", throwing);
  CHECK(!r4.ok);
  CHECK(!r4.error.empty());
}

CG_TEST(mem_expr_module_names) {
  const mem::Module& main = mem::Module::Main();
  REQUIRE(main.Valid());
  auto r1 = mem::EvalAddress(main.name);
  REQUIRE(r1.ok);
  CHECK_EQ(r1.value, main.base);
  auto r2 = mem::EvalAddress(main.name + "+1A2B");
  REQUIRE(r2.ok);
  CHECK_EQ(r2.value, main.base + 0x1A2B);
  auto r3 = mem::EvalAddress("\"" + main.name + "\" + #16");
  REQUIRE(r3.ok);
  CHECK_EQ(r3.value, main.base + 16);
  const std::string stem = main.name.substr(0, main.name.size() - 4);
  auto r4 = mem::EvalAddress(stem);
  REQUIRE(r4.ok);
  CHECK_EQ(r4.value, main.base);
  auto r5 = mem::EvalAddress("KERNEL32.DLL");
  REQUIRE(r5.ok);
  CHECK_EQ(r5.value, Addr(GetModuleHandleW(L"kernel32.dll")));
}

CG_TEST(mem_expr_errors) {
  const char* bad[] = {"", "   ", "1+", "(1", "[1", "1)", "#", "#12a", "zz_not_a_symbol_qq", "0xZZ", "1 2", "[0]",
                       "\"unterminated", "\"cg-not-loaded.dll\"", "12345678901234567", "%", "*3"};
  for (const char* e : bad) {
    auto r = mem::EvalAddress(e);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
  }
  auto r = mem::EvalAddress("1 + qqq_unknown");
  CHECK(!r.ok);
  CHECK(r.error.find("qqq_unknown") != std::string::npos);
  CHECK(r.error.find("column 5") != std::string::npos);
  CHECK(!mem::EvalAddress(std::string(5000, '1')).ok);
  CHECK(!mem::EvalAddress(std::string(200, '(') + "1" + std::string(200, ')')).ok);
  CHECK(mem::EvalAddress(std::string(20, '(') + "1" + std::string(20, ')')).ok);
}

// ---- disasm ----------------------------------------------------------------------------------

CG_TEST(mem_disasm_basic_instructions) {
  uint8_t code[64] = {};
  const uint8_t movRip[] = {0x48, 0x8B, 0x05, 0x10, 0x20, 0x00, 0x00};
  std::memcpy(code, movRip, sizeof(movRip));
  mem::Insn insn;
  REQUIRE(mem::Decode(Addr(code), insn));
  CHECK_EQ(insn.length, uint8_t{7});
  CHECK(insn.ripRelative);
  CHECK(!insn.relBranch);
  CHECK_EQ(insn.dispOffset, uint8_t{3});
  CHECK_EQ(insn.dispSize, uint8_t{4});
  CHECK_EQ(insn.immSize, uint8_t{0});
  CHECK_EQ(insn.target, Addr(code) + 7 + 0x2010);
  CHECK(insn.text.find("mov") != std::string::npos);
  CHECK(insn.text.starts_with("mov rax, [rip+0x2010] ; 0x"));
  CHECK(std::memcmp(insn.bytes, movRip, sizeof(movRip)) == 0);

  const uint8_t call[] = {0xE8, 0x00, 0x01, 0x00, 0x00};
  std::memcpy(code, call, sizeof(call));
  REQUIRE(mem::Decode(Addr(code), insn));
  CHECK_EQ(insn.length, uint8_t{5});
  CHECK(insn.relBranch);
  CHECK_EQ(insn.immOffset, uint8_t{1});
  CHECK_EQ(insn.immSize, uint8_t{4});
  CHECK_EQ(insn.target, Addr(code) + 5 + 0x100);
  CHECK_EQ(insn.text, std::format("call 0x{:X}", Addr(code) + 5 + 0x100));

  code[0] = 0xC3;
  REQUIRE(mem::Decode(Addr(code), insn));
  CHECK_EQ(insn.length, uint8_t{1});
  CHECK_EQ(insn.text, std::string("ret"));

  // cmp dword ptr [rip+disp], imm8: the displacement is followed by an immediate.
  const uint8_t cmpRip[] = {0x83, 0x3D, 0x00, 0x10, 0x00, 0x00, 0x05};
  std::memcpy(code, cmpRip, sizeof(cmpRip));
  REQUIRE(mem::Decode(Addr(code), insn));
  CHECK_EQ(insn.length, uint8_t{7});
  CHECK_EQ(insn.dispOffset, uint8_t{2});
  CHECK_EQ(insn.immOffset, uint8_t{6});
  CHECK_EQ(insn.immSize, uint8_t{1});
  CHECK_EQ(insn.target, Addr(code) + 7 + 0x1000);
  CHECK(insn.text.starts_with("cmp dword ptr [rip+0x1000], 0x5"));

  code[0] = 0x06;   // push es: invalid in 64-bit mode
  CHECK(!mem::Decode(Addr(code), insn));
  CHECK_EQ(insn.length, uint8_t{0});
  CHECK(!mem::Decode(0, insn));
}

CG_TEST(mem_disasm_text_printer) {
  struct Case {
    std::vector<uint8_t> bytes;
    const char* text;
  };
  const Case cases[] = {
      {{0x48, 0x89, 0x5C, 0x24, 0x08}, "mov [rsp+0x8], rbx"},
      {{0x0F, 0xB6, 0x41, 0x10}, "movzx eax, byte ptr [rcx+0x10]"},
      {{0x40, 0x88, 0xF0}, "mov al, sil"},
      {{0x88, 0xE0}, "mov al, ah"},
      {{0x66, 0x89, 0x08}, "mov [rax], cx"},
      {{0x41, 0xFF, 0xD0}, "call r8"},
      {{0x48, 0x83, 0xEC, 0x28}, "sub rsp, 0x28"},
      {{0x48, 0x83, 0xE4, 0xF0}, "and rsp, 0xFFFFFFFFFFFFFFF0"},
      {{0x8B, 0x44, 0x88, 0xF8}, "mov eax, [rax+rcx*4-0x8]"},
      {{0x4A, 0x8D, 0x04, 0x01}, "lea rax, [rcx+r8]"},
      {{0xC7, 0x41, 0x10, 0x01, 0x00, 0x00, 0x00}, "mov dword ptr [rcx+0x10], 0x1"},
      {{0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11}, "mov rax, 0x1122334455667788"},
      {{0x48, 0x63, 0xC1}, "movsxd rax, ecx"},
      {{0xF3, 0x0F, 0x10, 0x41, 0x04}, "movss xmm0, [rcx+0x4]"},
      {{0xF3, 0x0F, 0x59, 0xC1}, "mulss xmm0, xmm1"},
      {{0x0F, 0x57, 0xC0}, "xorps xmm0, xmm0"},
      {{0x0F, 0x2F, 0xC1}, "comiss xmm0, xmm1"},
      {{0xF3, 0x48, 0x0F, 0x2A, 0xC0}, "cvtsi2ss xmm0, rax"},
      {{0xF2, 0x0F, 0x2C, 0xC1}, "cvttsd2si eax, xmm1"},
      {{0x66, 0x48, 0x0F, 0x6E, 0xC0}, "movq xmm0, rax"},
      {{0x0F, 0x44, 0xC1}, "cmove eax, ecx"},
      {{0x0F, 0x94, 0xC0}, "sete al"},
      {{0x0F, 0xAF, 0xC1}, "imul eax, ecx"},
      {{0xFF, 0xC0}, "inc eax"},
      {{0x48, 0xF7, 0xD8}, "neg rax"},
      {{0x85, 0xC0}, "test eax, eax"},
      {{0x31, 0xC0}, "xor eax, eax"},
      {{0x41, 0x50}, "push r8"},
      {{0x5D}, "pop rbp"},
      {{0x90}, "nop"},
      {{0xCC}, "int3"},
      {{0x0F, 0x1F, 0x44, 0x00, 0x00}, "nop dword ptr [rax+rax]"},
      {{0x65, 0x48, 0x8B, 0x04, 0x25, 0x30, 0x00, 0x00, 0x00}, "mov rax, gs:[0x30]"},
      {{0xF0, 0x0F, 0xB1, 0x11}, "lock cmpxchg [rcx], edx"},
      {{0xF0, 0x0F, 0xC1, 0x41, 0xF8}, "lock xadd [rcx-0x8], eax"},
      {{0x48, 0x0F, 0xBD, 0xC7}, "bsr rax, rdi"},
      {{0x66, 0x0F, 0x6C, 0xC1}, "punpcklqdq xmm0, xmm1"},
      {{0x0F, 0xC6, 0xC1, 0x1B}, "shufps xmm0, xmm1, 0x1B"},
      {{0xD9, 0xEE}, "db 0xD9 0xEE"},
      {{0xF3, 0x48, 0xAB}, "rep stosq"},
      {{0x48, 0xC1, 0xE8, 0x03}, "shr rax, 0x3"},
      {{0x0F, 0xBF, 0x01}, "movsx eax, word ptr [rcx]"},
  };
  uint8_t code[32];
  for (const Case& c : cases) {
    std::memset(code, 0xCC, sizeof(code));
    std::memcpy(code, c.bytes.data(), c.bytes.size());
    mem::Insn insn;
    REQUIRE(mem::Decode(Addr(code), insn));
    CHECK_EQ(insn.length, static_cast<uint8_t>(c.bytes.size()));
    CHECK_EQ(insn.text, std::string(c.text));
  }

  // jcc rel8 / rel32 targets
  const uint8_t jcc[] = {0x74, 0x05, 0x0F, 0x85, 0x00, 0x02, 0x00, 0x00};
  std::memcpy(code, jcc, sizeof(jcc));
  mem::Insn insn;
  REQUIRE(mem::Decode(Addr(code), insn));
  CHECK(insn.relBranch);
  CHECK_EQ(insn.target, Addr(code) + 2 + 5);
  CHECK_EQ(insn.text, std::format("je 0x{:X}", Addr(code) + 7));
  REQUIRE(mem::Decode(Addr(code) + 2, insn));
  CHECK_EQ(insn.immOffset, uint8_t{2});
  CHECK_EQ(insn.target, Addr(code) + 2 + 6 + 0x200);
  CHECK(insn.text.starts_with("jne 0x"));
}

CG_TEST(mem_disasm_range_and_previous) {
  uint8_t code[64];
  std::memset(code, 0x90, sizeof(code));
  const uint8_t seq[] = {0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00,   // mov rax, [rip+0x10]
                         0x48, 0x89, 0x41, 0x10,                     // mov [rcx+0x10], rax
                         0xC3,                                       // ret
                         0x06};                                      // invalid
  constexpr size_t kAt = 24;
  std::memcpy(code + kAt, seq, sizeof(seq));
  const uintptr_t base = Addr(code);

  CHECK_EQ(mem::PreviousInstruction(base + kAt + 11), base + kAt + 7);
  CHECK_EQ(mem::PreviousInstruction(base + kAt + 7), base + kAt);
  CHECK_EQ(mem::PreviousInstruction(base + kAt + 12), base + kAt + 11);
  CHECK_EQ(mem::PreviousInstruction(0), uintptr_t{0});

  auto range = mem::DecodeRange(base + kAt, 5);
  REQUIRE(range.size() == 5);
  CHECK_EQ(range[0].length, uint8_t{7});
  CHECK(range[1].text.starts_with("mov [rcx+0x10], rax"));
  CHECK_EQ(range[2].text, std::string("ret"));
  CHECK_EQ(range[3].text, std::string("db 0x06"));
  CHECK_EQ(range[3].length, uint8_t{1});
  CHECK_EQ(range[4].text, std::string("nop"));
  CHECK(mem::DecodeRange(0, 4).empty());
  CHECK(mem::DecodeRange(base, 0).empty());
}

// ---- siggen ----------------------------------------------------------------------------------

CG_TEST(mem_siggen_roundtrip) {
  const mem::Module& main = mem::Module::Main();
  const uintptr_t fn = CodeAddress(&SigTargetFn);
  uint32_t (*volatile call)(uint32_t) = &SigTargetFn;
  CHECK(call(3) != 0);

  auto sig = mem::GenerateSignature(fn, main);
  REQUIRE(sig.has_value());
  auto pat = mem::Pattern::Parse(*sig);
  REQUIRE(pat.has_value());
  CHECK(pat->Size() <= 96);
  CHECK(pat->mask.back() != 0);
  const auto hits = mem::FindAllInModule(*pat, main, true, 4);
  REQUIRE(hits.size() == 1);
  CHECK_EQ(hits[0], fn);

  int local = 0;
  CHECK(!mem::GenerateSignature(Addr(&local), main).has_value());
  CHECK(!mem::GenerateSignature(fn, main, 1).has_value());
}
