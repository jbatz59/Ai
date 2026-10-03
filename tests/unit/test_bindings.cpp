// Unit tests for the bindings pattern step, especially the "unique" option (regression: a pattern
// that matched exactly once was wrongly rejected because the resolver required two matches).
#include <cstdint>
#include <optional>
#include <string>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "game/bindings.h"
#include "mem/module.h"
#include "mem/siggen.h"
#include "testing.h"

#ifdef _MSC_VER
#define CG_TEST_NOINLINE __declspec(noinline)
#else
#define CG_TEST_NOINLINE __attribute__((noinline))
#endif

namespace {

// A function with a distinctive body so siggen can build a signature that matches it exactly once.
CG_TEST_NOINLINE uint64_t BindTargetFn(uint64_t x) {
  volatile uint64_t a = x * 0x9E3779B97F4A7C15ull;
  a ^= (a >> 29);
  a += 0x1234567089ABCDEFull;
  a ^= (a << 17);
  return a;
}

template <class F> uintptr_t CodeAddr(F p) { return reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(p)); }

}  // namespace

using cg::game::Bindings;
using cg::game::SymbolStatus;
using json = nlohmann::json;

// The regression itself: a unique pattern with exactly one match must resolve, not fail.
CG_TEST(bindings_unique_pattern_resolves_single_match) {
  const cg::mem::Module& main = cg::mem::Module::Main();
  uint64_t (*volatile call)(uint64_t) = &BindTargetFn;
  CHECK(call(7) != 0);   // keep the function (and its body) in the image

  auto sig = cg::mem::GenerateSignature(CodeAddr(&BindTargetFn), main);
  REQUIRE(sig.has_value());

  json def = {{"kind", "function"}, {"steps", json::array({{{"pattern", *sig}, {"unique", true}}})}};
  std::string err;
  const bool ok = Bindings::Get().Define("Test.UniqueOne", def, &err);
  CHECK(ok);   // must NOT be rejected for "only 1 match"
  if (!ok) cg::test::Fail(__FILE__, __LINE__, "unique pattern with one match failed: " + err);

  auto addr = Bindings::Get().Addr("Test.UniqueOne");
  REQUIRE(addr.has_value());
  CHECK_EQ(*addr, CodeAddr(&BindTargetFn));
}

// A unique pattern that matches more than once must be rejected as ambiguous.
CG_TEST(bindings_unique_pattern_rejects_multiple_matches) {
  // "?? ??" matches almost everywhere, so "unique" must refuse it.
  json def = {{"kind", "address"}, {"steps", json::array({{{"pattern", "?? ??"}, {"unique", true}}})}};
  std::string err;
  const bool ok = Bindings::Get().Define("Test.UniqueMany", def, &err);
  CHECK(!ok);
  CHECK(err.find("more than once") != std::string::npos || err.find("ambiguous") != std::string::npos);

  auto info = Bindings::Get().Info("Test.UniqueMany");
  REQUIRE(info.has_value());
  CHECK(info->status == SymbolStatus::Failed);
}

// Without "unique", the same ambiguous pattern resolves to its first match (index 0).
CG_TEST(bindings_pattern_without_unique_takes_first_match) {
  json def = {{"kind", "address"}, {"steps", json::array({{{"pattern", "?? ??"}}})}};
  std::string err;
  const bool ok = Bindings::Get().Define("Test.FirstMatch", def, &err);
  CHECK(ok);
  if (!ok) cg::test::Fail(__FILE__, __LINE__, "non-unique pattern failed: " + err);
}

// "unique" and "index" together make no sense and must be a load-time error.
CG_TEST(bindings_unique_with_index_is_rejected) {
  json def = {{"kind", "address"}, {"steps", json::array({{{"pattern", "90"}, {"unique", true}, {"index", 1}}})}};
  std::string err;
  const bool ok = Bindings::Get().Define("Test.UniqueIndex", def, &err);
  CHECK(!ok);
  CHECK(!err.empty());
}
