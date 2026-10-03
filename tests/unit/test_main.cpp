#include <cstdio>
#include <cstring>

#include "testing.h"

namespace cg::test {
std::vector<Case>& Registry() {
  static std::vector<Case> r;
  return r;
}
static int g_failures = 0;
void Fail(const char* file, int line, const std::string& msg) {
  ++g_failures;
  std::printf("    FAIL %s:%d: %s\n", file, line, msg.c_str());
}
}  // namespace cg::test

// Usage: cg_tests.exe [substring-filter]
int main(int argc, char** argv) {
  using namespace cg::test;
  int ran = 0, failedCases = 0;
  for (const Case& c : Registry()) {
    if (argc > 1 && !std::strstr(c.name, argv[1])) continue;
    const int before = g_failures;
    std::printf("[ RUN  ] %s\n", c.name);
    std::fflush(stdout);
    c.fn();
    ++ran;
    const bool ok = g_failures == before;
    if (!ok) ++failedCases;
    std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", c.name);
  }
  std::printf("\n%d test(s), %d failed\n", ran, failedCases);
  return failedCases == 0 ? 0 : 1;
}
