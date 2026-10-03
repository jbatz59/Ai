#pragma once
// Consigliere's own Lua 5.4 runtime for user scripts in Consigliere/scripts/*.lua.
// Each script gets its own environment table (globals isolated) on one shared lua_State that is
// only ever touched from the render thread. API surface (documented in docs/SCRIPTING.md):
//   cg.*      log, notify, version, time, frame, menu_open, on(event, fn), unload_script
//   mem.*     read/write typed values, deref, pattern scan, module info, rtti, eval
//   game.*    facade (player/vehicle/world/camera) + game.lua(code, cb) bridge
//   bind.*    bindings addr/field/offset/has/define
//   ui.*      immediate-mode widgets usable inside ui callbacks (text, button, checkbox, slider...)
//   draw.*    overlay primitives (line, rect, text, circle, world_text, w2s)
//   key.*     pressed/down by name ("F5", "Ctrl+Shift+K")
//   feature.* feature.toggle{...} / feature.action{...} create real menu entries
// Events: "tick"(dt), "draw"() (overlay), "menu"() (inside the Scripts page card), "unload"().
// Removed from stdlib for safety: os.execute, os.remove, os.rename, os.exit, io.popen, loadlib.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cg::script {

struct ScriptInfo {
  std::string name;                 // file stem
  std::filesystem::path path;
  bool loaded = false;
  bool enabled = true;              // persisted in Config "scripts.<name>.enabled"
  std::string error;                // last load/runtime error
  double lastTickMs = 0;            // profiling: time spent in its tick handler last frame
};

class Engine {
 public:
  static Engine& Get();

  bool Init();                      // render thread; creates the VM, loads enabled scripts
  void Shutdown();                  // fires "unload", removes script features, closes VM

  void Tick(float dt);              // fires "tick"
  void DrawOverlay();               // fires "draw" (background draw list)
  void DrawMenu();                  // fires "menu" for each script inside its own card

  void ReloadAll();
  bool Reload(const std::string& name);
  void SetEnabled(const std::string& name, bool enabled);
  std::vector<ScriptInfo> List() const;

  // REPL: runs in a shared "console" environment. Returns false + error text on failure;
  // `output` collects print() output and the returned values.
  bool RunConsole(std::string_view code, std::string& output);

 private:
  Engine();
  struct Impl;
  Impl* impl_;
};

}  // namespace cg::script
