#include "script/engine.h"

// User Lua scripting is not part of this release: the engine is a no-op so the rest of the menu
// (overlay, features, tools) is unaffected. The embedded Lua 5.4 library stays vendored for a
// future release.
namespace cg::script {

struct Engine::Impl {};

Engine::Engine() : impl_(nullptr) {}
Engine& Engine::Get() {
  static Engine e;
  return e;
}

bool Engine::Init() { return false; }
void Engine::Shutdown() {}
void Engine::Tick(float) {}
void Engine::DrawOverlay() {}
void Engine::DrawMenu() {}
void Engine::ReloadAll() {}
bool Engine::Reload(const std::string&) { return false; }
void Engine::SetEnabled(const std::string&, bool) {}
std::vector<ScriptInfo> Engine::List() const { return {}; }

bool Engine::RunConsole(std::string_view, std::string& output) {
  output = "User scripting is not included in this build.";
  return false;
}

}  // namespace cg::script
