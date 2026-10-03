#include "mem/patch.h"

#include <algorithm>

#include "core/log.h"
#include "mem/safe.h"

namespace cg::mem {

namespace {

bool BytesAt(uintptr_t address, const std::vector<uint8_t>& expected) {
  std::vector<uint8_t> cur(expected.size());
  if (!ReadRaw(address, cur.data(), cur.size())) return false;
  return cur == expected;
}

// Writes `to` over `from` after verifying the site currently holds `from`. Already holding `to`
// counts as success (idempotent).
bool Swap(const Patches::Info& p, const std::vector<uint8_t>& from, const std::vector<uint8_t>& to, const char* what) {
  if (BytesAt(p.address, to)) return true;
  if (!BytesAt(p.address, from)) {
    log::Warn("mem", "patch '{}': bytes at {:#x} changed, refusing to {}", p.name, p.address, what);
    return false;
  }
  if (!WriteRaw(p.address, to.data(), to.size())) {
    log::Warn("mem", "patch '{}': write at {:#x} failed ({})", p.name, p.address, what);
    return false;
  }
  return true;
}

}  // namespace

Patches& Patches::Get() {
  static Patches instance;
  return instance;
}

bool Patches::Add(std::string name, uintptr_t address, std::vector<uint8_t> bytes) {
  try {
    if (bytes.empty() || !IsPlausiblePtr(address)) {
      log::Warn("mem", "patch '{}': invalid address {:#x} or empty bytes", name, address);
      return false;
    }
    std::lock_guard lock(mutex_);
    auto it = std::find_if(patches_.begin(), patches_.end(), [&](const Info& p) { return p.name == name; });
    if (it != patches_.end()) {
      if (it->applied && !Swap(*it, it->patched, it->original, "restore")) {
        log::Warn("mem", "patch '{}': could not restore previous registration", name);
        return false;
      }
      patches_.erase(it);
    }
    Info info;
    info.name = std::move(name);
    info.address = address;
    info.original.resize(bytes.size());
    if (!ReadRaw(address, info.original.data(), info.original.size())) {
      log::Warn("mem", "patch '{}': site {:#x} unreadable", info.name, address);
      return false;
    }
    info.patched = std::move(bytes);
    info.applied = false;
    log::Info("mem", "patch '{}' registered at {:#x} ({} bytes)", info.name, address, info.patched.size());
    patches_.push_back(std::move(info));
    return true;
  } catch (...) {
    return false;
  }
}

bool Patches::AddNop(std::string name, uintptr_t address, size_t count) {
  try {
    if (count == 0 || count > 4096) return false;
    return Add(std::move(name), address, std::vector<uint8_t>(count, 0x90));
  } catch (...) {
    return false;
  }
}

bool Patches::Apply(std::string_view name, bool on) {
  try {
    std::lock_guard lock(mutex_);
    for (Info& p : patches_) {
      if (p.name != name) continue;
      if (p.applied == on) return true;
      const bool ok = on ? Swap(p, p.original, p.patched, "apply") : Swap(p, p.patched, p.original, "restore");
      if (!ok) return false;
      p.applied = on;
      log::Info("mem", "patch '{}' {}", p.name, on ? "applied" : "restored");
      return true;
    }
    return false;
  } catch (...) {
    return false;
  }
}

bool Patches::IsApplied(std::string_view name) const {
  try {
    std::lock_guard lock(mutex_);
    for (const Info& p : patches_)
      if (p.name == name) return p.applied;
  } catch (...) {
  }
  return false;
}

bool Patches::Exists(std::string_view name) const {
  try {
    std::lock_guard lock(mutex_);
    for (const Info& p : patches_)
      if (p.name == name) return true;
  } catch (...) {
  }
  return false;
}

bool Patches::Remove(std::string_view name) {
  try {
    std::lock_guard lock(mutex_);
    for (auto it = patches_.begin(); it != patches_.end(); ++it) {
      if (it->name != name) continue;
      if (it->applied && !Swap(*it, it->patched, it->original, "restore")) return false;
      log::Info("mem", "patch '{}' removed", it->name);
      patches_.erase(it);
      return true;
    }
    return false;
  } catch (...) {
    return false;
  }
}

void Patches::RestoreAll() {
  try {
    std::lock_guard lock(mutex_);
    size_t restored = 0;
    for (auto it = patches_.rbegin(); it != patches_.rend(); ++it) {
      if (!it->applied) continue;
      if (Swap(*it, it->patched, it->original, "restore")) {
        it->applied = false;
        ++restored;
      }
    }
    if (restored) log::Info("mem", "restored {} patch(es)", restored);
  } catch (...) {
  }
}

std::vector<Patches::Info> Patches::List() const {
  try {
    std::lock_guard lock(mutex_);
    return patches_;
  } catch (...) {
    return {};
  }
}

}  // namespace cg::mem
