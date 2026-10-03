#pragma once
// Named byte patches with original-byte backup. Applying verifies the site still holds the bytes
// captured at registration (prevents double-patching / patching shifted code after a game update).
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace cg::mem {

class Patches {
 public:
  static Patches& Get();

  // Registers (does not apply). Captures original bytes now. Re-registering a name replaces it
  // (restoring the old one first if applied). Fails if the site is unreadable.
  bool Add(std::string name, uintptr_t address, std::vector<uint8_t> bytes);
  bool AddNop(std::string name, uintptr_t address, size_t count);

  bool Apply(std::string_view name, bool on);   // on=false restores the original bytes
  bool IsApplied(std::string_view name) const;
  bool Exists(std::string_view name) const;
  bool Remove(std::string_view name);           // restores then forgets
  void RestoreAll();                            // unload path

  struct Info {
    std::string name;
    uintptr_t address;
    std::vector<uint8_t> original, patched;
    bool applied;
  };
  std::vector<Info> List() const;

 private:
  Patches() = default;
  mutable std::mutex mutex_;
  std::vector<Info> patches_;
};

}  // namespace cg::mem
