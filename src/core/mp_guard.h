#pragma once
// Consigliere is single-player only. If a multiplayer client (MafiaHub/MafiaMP or any module we
// flag) is present in the process, every gameplay feature, patch and script memory write is refused.
#include <string>

namespace cg::mp_guard {

// Checks loaded modules and the command line. Re-run periodically (cheap) — modules can load late.
void Refresh();
bool Blocked();
const std::string& Reason();   // empty if not blocked

}  // namespace cg::mp_guard
