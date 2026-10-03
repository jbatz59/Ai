#pragma once
// Build fingerprint + binding diagnostics for remote troubleshooting. Writes Chroma/diagnostics.txt:
// exe fingerprint, every binding's status/error, near-miss matches for each shipped pattern
// (shorter prefixes, with surrounding bytes), script-engine string anchors with their code
// references, and RTTI classes related to scripting. Nothing leaves the PC unless the user sends it.
#include <functional>
#include <string>

namespace cg::game::diagnostics {

// Runs on a worker thread; `done` is delivered on the render thread.
void WriteReportAsync(std::function<void(bool ok, std::wstring path)> done = {});
bool Running();

// Called after the first bindings pass: writes the report once per game build when the script-VM
// bindings did not resolve, so the file is already there when the user asks for help.
void AutoReportIfVmUnbound();

}  // namespace cg::game::diagnostics
