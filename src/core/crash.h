#pragma once
// Crash diagnostics. Installs a vectored *continue* handler + unhandled-exception filter that write
// Data("crash")/crash-<time>.txt (exception, module+offset of faulting RIP, registers, stack walk
// with module+offset) and a minidump. Chains to any previous filter. Never swallows exceptions.
namespace cg::crash {

void Install();
void Uninstall();

}  // namespace cg::crash
