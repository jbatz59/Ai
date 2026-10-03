#pragma once
// Minimal test framework (no dependencies). Tests self-register:
//   CG_TEST(pattern_parses_wildcards) { CHECK(x); CHECK_EQ(a, b); }
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace cg::test {

struct Case {
  const char* name;
  void (*fn)();
};
std::vector<Case>& Registry();
struct Registrar {
  Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};
void Fail(const char* file, int line, const std::string& msg);   // records failure, continues

template <class T> std::string Show(const T& v) {
  if constexpr (requires(std::ostream& o) { o << v; }) { std::ostringstream s; s << v; return s.str(); }
  else return "<unprintable>";
}

}  // namespace cg::test

#define CG_TEST(name)                                                    \
  static void cg_test_##name();                                          \
  static ::cg::test::Registrar cg_test_reg_##name(#name, cg_test_##name); \
  static void cg_test_##name()

#define CHECK(cond) \
  do { if (!(cond)) ::cg::test::Fail(__FILE__, __LINE__, "CHECK(" #cond ")"); } while (0)

#define CHECK_EQ(a, b)                                                                                     \
  do {                                                                                                     \
    auto&& cg_a_ = (a); auto&& cg_b_ = (b);                                                                \
    if (!(cg_a_ == cg_b_))                                                                                 \
      ::cg::test::Fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b "): " + ::cg::test::Show(cg_a_) + " != " + ::cg::test::Show(cg_b_)); \
  } while (0)

#define REQUIRE(cond) \
  do { if (!(cond)) { ::cg::test::Fail(__FILE__, __LINE__, "REQUIRE(" #cond ")"); return; } } while (0)
