#include "mem/hook.h"

#include <MinHook.h>

#include "core/log.h"

namespace cg::mem {

namespace {
const char* StatusText(MH_STATUS s) {
  const char* t = MH_StatusToString(s);
  return t ? t : "unknown";
}
}  // namespace

Hooks& Hooks::Get() {
  static Hooks instance;
  return instance;
}

bool Hooks::Init() {
  try {
    std::lock_guard lock(mutex_);
    if (initialized_) return true;
    const MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
      log::Warn("mem", "MH_Initialize failed: {}", StatusText(s));
      return false;
    }
    initialized_ = true;
    log::Info("mem", "MinHook initialized");
    return true;
  } catch (...) {
    return false;
  }
}

void Hooks::Shutdown() {
  try {
    std::lock_guard lock(mutex_);
    if (!initialized_) {
      hooks_.clear();
      return;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    for (auto it = hooks_.rbegin(); it != hooks_.rend(); ++it) {
      const MH_STATUS s = MH_RemoveHook(reinterpret_cast<void*>(it->target));
      if (s != MH_OK) log::Warn("mem", "hook '{}': MH_RemoveHook failed: {}", it->name, StatusText(s));
    }
    const size_t n = hooks_.size();
    hooks_.clear();
    MH_Uninitialize();
    initialized_ = false;
    log::Info("mem", "hooks shut down ({} removed)", n);
  } catch (...) {
  }
}

bool Hooks::Install(std::string name, void* target, void* detour, void** original, bool enable) {
  try {
    std::lock_guard lock(mutex_);
    if (!initialized_) {
      log::Warn("mem", "hook '{}': MinHook not initialized", name);
      return false;
    }
    if (!target || !detour) {
      log::Warn("mem", "hook '{}': null target or detour", name);
      return false;
    }
    for (const Info& h : hooks_) {
      if (h.name == name) {
        log::Warn("mem", "hook '{}': already installed", name);
        return false;
      }
      if (h.target == reinterpret_cast<uintptr_t>(target)) {
        log::Warn("mem", "hook '{}': target {:#x} already hooked by '{}'", name, h.target, h.name);
        return false;
      }
    }
    hooks_.reserve(hooks_.size() + 1);   // reserve first so push_back below cannot throw
    void* trampoline = nullptr;
    MH_STATUS s = MH_CreateHook(target, detour, &trampoline);
    if (s != MH_OK) {
      log::Warn("mem", "hook '{}': MH_CreateHook({:#x}) failed: {}", name, reinterpret_cast<uintptr_t>(target), StatusText(s));
      return false;
    }
    if (original) *original = trampoline;
    if (enable) {
      s = MH_EnableHook(target);
      if (s != MH_OK) {
        log::Warn("mem", "hook '{}': MH_EnableHook failed: {}", name, StatusText(s));
        MH_RemoveHook(target);
        if (original) *original = nullptr;
        return false;
      }
    }
    hooks_.push_back(Info{name, reinterpret_cast<uintptr_t>(target), reinterpret_cast<uintptr_t>(detour), enable});
    log::Info("mem", "hook '{}' installed at {:#x}{}", name, reinterpret_cast<uintptr_t>(target), enable ? "" : " (disabled)");
    return true;
  } catch (...) {
    return false;
  }
}

bool Hooks::SetEnabled(std::string_view name, bool enabled) {
  try {
    std::lock_guard lock(mutex_);
    for (Info& h : hooks_) {
      if (h.name != name) continue;
      if (h.enabled == enabled) return true;
      void* t = reinterpret_cast<void*>(h.target);
      const MH_STATUS s = enabled ? MH_EnableHook(t) : MH_DisableHook(t);
      if (s != MH_OK) {
        log::Warn("mem", "hook '{}': {} failed: {}", h.name, enabled ? "enable" : "disable", StatusText(s));
        return false;
      }
      h.enabled = enabled;
      return true;
    }
    return false;
  } catch (...) {
    return false;
  }
}

bool Hooks::Remove(std::string_view name) {
  try {
    std::lock_guard lock(mutex_);
    for (auto it = hooks_.begin(); it != hooks_.end(); ++it) {
      if (it->name != name) continue;
      void* t = reinterpret_cast<void*>(it->target);
      if (it->enabled) {
        const MH_STATUS s = MH_DisableHook(t);
        if (s != MH_OK && s != MH_ERROR_DISABLED) log::Warn("mem", "hook '{}': MH_DisableHook failed: {}", it->name, StatusText(s));
      }
      const MH_STATUS s = MH_RemoveHook(t);
      if (s != MH_OK) {
        log::Warn("mem", "hook '{}': MH_RemoveHook failed: {}", it->name, StatusText(s));
        it->enabled = false;
        return false;
      }
      log::Info("mem", "hook '{}' removed", it->name);
      hooks_.erase(it);
      return true;
    }
    return false;
  } catch (...) {
    return false;
  }
}

bool Hooks::Exists(std::string_view name) const {
  try {
    std::lock_guard lock(mutex_);
    for (const Info& h : hooks_)
      if (h.name == name) return true;
  } catch (...) {
  }
  return false;
}

std::vector<Hooks::Info> Hooks::List() const {
  try {
    std::lock_guard lock(mutex_);
    return hooks_;
  } catch (...) {
    return {};
  }
}

}  // namespace cg::mem
