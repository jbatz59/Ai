#pragma once
// Named function hooks on top of MinHook. All hooks are tracked for the UI and removed on unload.
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace cg::mem {

class Hooks {
 public:
  static Hooks& Get();

  bool Init();        // MH_Initialize (idempotent)
  void Shutdown();    // disables + removes every hook, MH_Uninitialize

  // Creates and (optionally) enables a detour. `original` receives the trampoline.
  bool Install(std::string name, void* target, void* detour, void** original, bool enable = true);
  template <class F> bool Install(std::string name, uintptr_t target, F* detour, F** original, bool enable = true) {
    return Install(std::move(name), reinterpret_cast<void*>(target), reinterpret_cast<void*>(detour),
                   reinterpret_cast<void**>(original), enable);
  }
  bool SetEnabled(std::string_view name, bool enabled);
  bool Remove(std::string_view name);
  bool Exists(std::string_view name) const;

  struct Info {
    std::string name;
    uintptr_t target;
    uintptr_t detour;
    bool enabled;
  };
  std::vector<Info> List() const;

 private:
  Hooks() = default;
  mutable std::mutex mutex_;
  std::vector<Info> hooks_;
  bool initialized_ = false;
};

}  // namespace cg::mem
