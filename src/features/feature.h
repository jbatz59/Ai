#pragma once
// Feature model. Every menu entry is a Feature: a toggle (god mode), an action (repair car) or a
// panel (teleport locations). Features declare the bindings they need; the UI greys them out
// with the exact missing symbol instead of crashing or silently doing nothing.
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "core/hotkeys.h"

namespace cg::features {

enum class Category : uint8_t { Player, Vehicle, Weapons, World, Teleport, Camera, Visuals, CheatTable, Scripts, Count };
const char* CategoryName(Category c);
const char* CategoryIcon(Category c);   // CG_ICON_* glyph

enum class Kind : uint8_t {
  Toggle,   // on/off; Tick() runs while enabled
  Action,   // one-shot Activate()
  Panel     // custom UI only (DrawSettings is the whole body)
};

class Feature {
 public:
  Feature(std::string id, std::string name, Category category, Kind kind, std::string description);
  virtual ~Feature() = default;
  Feature(const Feature&) = delete;
  Feature& operator=(const Feature&) = delete;

  const std::string& Id() const { return id_; }            // stable, used as config key: "player.god"
  const std::string& Name() const { return name_; }
  const std::string& Description() const { return description_; }
  Category GetCategory() const { return category_; }
  Kind GetKind() const { return kind_; }

  // Binding names this feature needs. Default: none.
  virtual std::vector<std::string> Requires() const { return {}; }
  // True if every required binding resolved and multiplayer guard is not blocking.
  bool Available() const;
  std::string UnavailableReason() const;   // "" when available

  bool Enabled() const { return enabled_; }
  void SetEnabled(bool on);   // no-op if unavailable and on==true; calls OnEnable/OnDisable; persists; toasts
  void Trigger();             // Toggle: flip; Action: Activate(); Panel: nothing

  Hotkey& Key() { return hotkey_; }
  const Hotkey& Key() const { return hotkey_; }

  // --- overridables -------------------------------------------------------------------------
  virtual void OnEnable() {}
  virtual void OnDisable() {}
  virtual void Activate() {}             // Action
  virtual void Tick(float dt) {}         // every frame while enabled (Toggle) / always (Panel)
  virtual bool HasSettings() const { return false; }
  virtual void DrawSettings() {}         // ImGui, drawn under the feature row (or as the panel body)
  virtual void DrawOverlay() {}          // background draw list, every frame while enabled/always for panels
  virtual void SaveExtra(nlohmann::json& j) const {}
  virtual void LoadExtra(const nlohmann::json& j) {}
  // Favorites are shown on the Home page and in the HUD.
  bool favorite = false;

 protected:
  bool enabled_ = false;

 private:
  std::string id_, name_, description_;
  Category category_;
  Kind kind_;
  Hotkey hotkey_;
};

class Registry {
 public:
  static Registry& Get();

  Feature* Add(std::unique_ptr<Feature> f);   // ids must be unique; returns the stored pointer
  bool Remove(const std::string& id);         // used by scripts and cheat-table reloads
  Feature* Find(const std::string& id) const;
  const std::vector<std::unique_ptr<Feature>>& All() const { return features_; }
  std::vector<Feature*> InCategory(Category c) const;

  void Tick(float dt);        // hotkeys + Tick for enabled toggles and panels (render thread)
  void DrawOverlays();
  void DisableAll();          // panic key / unload
  void Save() const;          // into Config ("features.<id>")
  void Load();                // from Config; re-enables features that were on
  void RegisterBuiltins();    // calls every Register*Features() below exactly once

 private:
  Registry() = default;
  std::vector<std::unique_ptr<Feature>> features_;
};

// Implemented in the respective features/*.cpp files.
void RegisterPlayerFeatures(Registry& r);
void RegisterVehicleFeatures(Registry& r);
void RegisterWeaponFeatures(Registry& r);
void RegisterWorldFeatures(Registry& r);
void RegisterTeleportFeatures(Registry& r);
void RegisterCameraFeatures(Registry& r);
void RegisterVisualFeatures(Registry& r);
// Data-driven cheats from Bindings::Cheats(); re-run after every bindings reload.
void SyncCheatTableFeatures(Registry& r);

}  // namespace cg::features
