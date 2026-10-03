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
      if (it->farJump) {
        SetFarEnabled(*it, false);   // trampoline stays allocated: a thread may still be inside it
        continue;
      }
      const MH_STATUS s = MH_RemoveHook(reinterpret_cast<void*>(it->target));
      if (s != MH_OK) log::Warn("mem", "hook '{}': MH_RemoveHook failed: {}", it->name, StatusText(s));
    }
    far_.clear();
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
    far_.reserve(far_.size() + 1);
    void* trampoline = nullptr;
    MH_STATUS s = MH_CreateHook(target, detour, &trampoline);
    if (s == MH_ERROR_MEMORY_ALLOC) {
      // No free memory within ±1 GB of the target: big games reserve the space around their image.
      const auto at = reinterpret_cast<uintptr_t>(target);
      farhook::Hook fh;
      std::string err;
      if (!farhook::Create(at, detour, fh, err)) {
        log::Warn("mem", "hook '{}': no free memory near {:#x} and the far-jump fallback refused: {}", name, at, err);
        return false;
      }
      if (original) *original = fh.trampoline;   // before enabling: the detour may run right away
      if (enable && !farhook::SetEnabled(fh, true, err)) {
        log::Warn("mem", "hook '{}': far-jump enable failed: {}", name, err);
        if (original) *original = nullptr;
        return false;
      }
      far_.push_back(fh);
      hooks_.push_back(Info{name, at, reinterpret_cast<uintptr_t>(detour), enable, true});
      log::Info("mem", "hook '{}' installed at {:#x} via far jump (no free memory within 1 GB; {} bytes moved){}", name, at,
                fh.len, enable ? "" : " (disabled)");
      return true;
    }
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
      if (h.farJump) return SetFarEnabled(h, enabled);
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
      if (it->farJump) {
        if (!SetFarEnabled(*it, false)) return false;
        for (auto f = far_.begin(); f != far_.end(); ++f) {
          if (f->target != it->target) continue;
          far_.erase(f);   // the trampoline itself is never freed
          break;
        }
        log::Info("mem", "hook '{}' removed", it->name);
        hooks_.erase(it);
        return true;
      }
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

bool Hooks::SetFarEnabled(Info& h, bool enabled) {
  for (farhook::Hook& f : far_) {
    if (f.target != h.target) continue;
    std::string err;
    if (!farhook::SetEnabled(f, enabled, err)) {
      log::Warn("mem", "hook '{}': far-jump {} failed: {}", h.name, enabled ? "enable" : "disable", err);
      return false;
    }
    h.enabled = enabled;
    return true;
  }
  return false;
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
