#pragma once
// JSON-backed settings store. Keys are dotted paths: "ui.scale", "features.player.god.enabled".
// All methods are thread-safe. Writes mark the store dirty; SaveIfDirty() (called by the overlay
// about once per second) persists to disk, so callers never need to save explicitly.
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace cg {

class Config {
 public:
  static Config& Get();

  bool Load(const std::filesystem::path& file);   // missing file => empty store, returns true
  bool Save();                                    // writes atomically (tmp file + rename)
  void SaveIfDirty();

  bool ReadBool(std::string_view key, bool def) const;
  int64_t ReadInt(std::string_view key, int64_t def) const;
  double ReadDouble(std::string_view key, double def) const;
  float ReadFloat(std::string_view key, float def) const { return static_cast<float>(ReadDouble(key, def)); }
  std::string ReadString(std::string_view key, std::string_view def) const;
  nlohmann::json ReadJson(std::string_view key) const;   // null if missing

  void WriteBool(std::string_view key, bool v);
  void WriteInt(std::string_view key, int64_t v);
  void WriteDouble(std::string_view key, double v);
  void WriteString(std::string_view key, std::string_view v);
  void WriteJson(std::string_view key, const nlohmann::json& v);
  void Erase(std::string_view key);

  // Profiles are full snapshots stored in Data("profiles")/<name>.json.
  std::vector<std::string> ListProfiles() const;
  bool SaveProfile(const std::string& name);
  bool LoadProfile(const std::string& name);    // replaces the live store, then fires listeners
  bool DeleteProfile(const std::string& name);

  // Fired (on the caller's thread) after LoadProfile so features can re-read their state.
  using Listener = void (*)();
  void AddReloadListener(Listener fn);

 private:
  Config() = default;
  mutable std::mutex mutex_;
  nlohmann::json root_ = nlohmann::json::object();
  std::filesystem::path file_;
  bool dirty_ = false;
  std::vector<Listener> listeners_;
};

}  // namespace cg
