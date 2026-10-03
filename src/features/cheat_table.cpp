// Data-driven cheats from bindings JSON (see cheat_table.h) turned into real Features.
#include "features/cheat_table.h"

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "core/log.h"
#include "core/util.h"
#include "features/feature.h"
#include "features/game_lua.h"
#include "game/bindings.h"
#include "mem/expr.h"
#include "mem/patch.h"
#include "mem/safe.h"
#include "mem/value.h"
#include "ui/widgets.h"

namespace cg::features {

namespace {

constexpr std::string_view kChannel = "cheats";
uint64_t g_syncedGeneration = ~0ull;

std::string Str(const nlohmann::json& j, const char* key, const std::string& def = {}) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : def;
}

std::optional<Category> ParseCategory(const std::string& s) {
  if (s.empty()) return Category::CheatTable;
  for (int i = 0; i < static_cast<int>(Category::Count); ++i) {
    const auto c = static_cast<Category>(i);
    if (util::IEquals(s, CategoryName(c))) return c;
  }
  if (util::IEquals(s, "CheatTable")) return Category::CheatTable;
  return std::nullopt;
}

// {"symbol": X [, "field": F]} | {"expr": E}  => "" if valid
std::string ValidateTarget(const nlohmann::json& t, const char* what) {
  if (!t.is_object()) return std::string(what) + " must be an object";
  const bool sym = t.contains("symbol") && t["symbol"].is_string() && !t["symbol"].get<std::string>().empty();
  const bool expr = t.contains("expr") && t["expr"].is_string() && !t["expr"].get<std::string>().empty();
  if (sym == expr) return std::string(what) + " needs exactly one of \"symbol\" or \"expr\"";
  if (t.contains("field") && (!sym || !t["field"].is_string())) return std::string(what) + ".field needs a \"symbol\" object";
  return {};
}

void TargetSymbols(const nlohmann::json& t, std::vector<std::string>& out) {
  if (!t.is_object()) return;
  if (auto s = Str(t, "symbol"); !s.empty()) out.push_back(s);
  if (auto f = Str(t, "field"); !f.empty()) out.push_back(f);
}

std::optional<uintptr_t> ResolveTarget(const nlohmann::json& t, uintptr_t* objectOut = nullptr) {
  const auto& b = game::Bindings::Get();
  if (const std::string sym = Str(t, "symbol"); !sym.empty()) {
    const auto base = b.Addr(sym);
    if (!base) return std::nullopt;
    if (objectOut) *objectOut = *base;
    const std::string field = Str(t, "field");
    return field.empty() ? base : b.Field(field, *base);
  }
  const auto r = mem::EvalAddress(Str(t, "expr"), [&b](std::string_view n) { return b.Addr(n); });
  if (!r.ok) return std::nullopt;
  return r.value;
}

// Encodes a numeric value for type t.
bool EncodeNumber(mem::ValueType t, double v, std::vector<uint8_t>& out) {
  return mem::ParseValue(t, mem::IsFloat(t) ? std::to_string(v) : std::to_string(static_cast<long long>(v)), out);
}

std::optional<double> DecodeNumber(mem::ValueType t, uintptr_t addr) {
  const size_t n = mem::ValueSize(t);
  uint8_t buf[8]{};
  if (n == 0 || n > sizeof(buf) || !mem::ReadRaw(addr, buf, n)) return std::nullopt;
  switch (t) {
    case mem::ValueType::I8: { int8_t v; std::memcpy(&v, buf, 1); return v; }
    case mem::ValueType::I16: { int16_t v; std::memcpy(&v, buf, 2); return v; }
    case mem::ValueType::I32: { int32_t v; std::memcpy(&v, buf, 4); return v; }
    case mem::ValueType::I64: { int64_t v; std::memcpy(&v, buf, 8); return static_cast<double>(v); }
    case mem::ValueType::U8: return buf[0];
    case mem::ValueType::U16: { uint16_t v; std::memcpy(&v, buf, 2); return v; }
    case mem::ValueType::U32: { uint32_t v; std::memcpy(&v, buf, 4); return v; }
    case mem::ValueType::U64: { uint64_t v; std::memcpy(&v, buf, 8); return static_cast<double>(v); }
    case mem::ValueType::F32: { float v; std::memcpy(&v, buf, 4); return v; }
    case mem::ValueType::F64: { double v; std::memcpy(&v, buf, 8); return v; }
    default: return std::nullopt;
  }
}

class CheatFeature : public Feature {
 public:
  CheatFeature(const nlohmann::json& def, Category cat, Kind kind)
      : Feature("table." + Str(def, "id"), Str(def, "name", Str(def, "id")), cat, kind, Str(def, "description")), def_(def) {
    type_ = util::ToLower(Str(def, "type"));
    valueType_ = mem::ParseValueType(Str(def, "value_type", "i32")).value_or(mem::ValueType::I32);
    if (const std::string hk = Str(def, "hotkey"); !hk.empty()) Key() = Hotkey::Parse(hk);
    if (type_ == "slider") {
      min_ = def.value("min", 0.0);
      max_ = def.value("max", 100.0);
      value_ = def.value("default", min_);
      format_ = Str(def, "format", mem::IsFloat(valueType_) ? "%.2f" : "%.0f");
      canFreeze_ = def.value("freeze", false);
    }
  }

  std::vector<std::string> Requires() const override {
    std::vector<std::string> r;
    if (type_ == "lua") r.push_back("vm");
    TargetSymbols(def_.value("target", nlohmann::json::object()), r);
    TargetSymbols(def_.value("site", nlohmann::json::object()), r);
    const std::string v = Str(def_, "value");
    if (util::StartsWith(v, "max:")) r.push_back(v.substr(4));
    if (const auto it = def_.find("requires"); it != def_.end() && it->is_array())
      for (const auto& s : *it)
        if (s.is_string()) r.push_back(s.get<std::string>());
    return r;
  }

  void OnEnable() override {
    if (type_ == "patch") {
      const std::string name = PatchName();
      const auto site = ResolveTarget(def_["site"]);
      bool ok = site.has_value();
      if (ok) {
        if (def_.contains("nop")) {
          ok = mem::Patches::Get().AddNop(name, *site, def_["nop"].get<size_t>());
        } else {
          std::vector<uint8_t> bytes;
          ok = mem::ParseValue(mem::ValueType::Bytes, Str(def_, "bytes"), bytes) && !bytes.empty() &&
               mem::Patches::Get().Add(name, *site, std::move(bytes));
        }
      }
      if (!ok || !mem::Patches::Get().Apply(name, true)) {
        log::Warn(kChannel, "patch '{}' could not be applied", Id());
        enabled_ = false;   // tells the registry the enable failed
      }
    } else if (type_ == "freeze" && !WriteConfigured()) {
      log::Warn(kChannel, "freeze '{}': target not writable", Id());
      enabled_ = false;
    }
  }
  void OnDisable() override {
    if (type_ == "patch") mem::Patches::Get().Apply(PatchName(), false);
  }
  void Tick(float) override {
    if (type_ == "freeze") WriteConfigured();
    else if (type_ == "slider" && frozen_) WriteSlider();
  }
  void Activate() override {
    if (type_ == "set") {
      if (!WriteConfigured()) log::Warn(kChannel, "set '{}': write failed", Id());
    } else if (type_ == "lua") {
      RunFeatureChunk(Str(def_, "code"), Name());
    }
  }

  bool HasSettings() const override { return type_ == "slider"; }
  void DrawSettings() override {
    if (type_ != "slider") return;
    float v = static_cast<float>(value_);
    if (ImGui::SliderFloat("##value", &v, static_cast<float>(min_), static_cast<float>(max_), format_.c_str())) {
      value_ = v;
      WriteSlider();
    }
    if (canFreeze_) {
      ImGui::SameLine();
      ImGui::Checkbox("Lock", &frozen_);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Read")) {
      if (const auto a = ResolveTarget(def_["target"]))
        if (const auto cur = DecodeNumber(valueType_, *a)) value_ = *cur;
    }
  }
  void SaveExtra(nlohmann::json& j) const override {
    if (type_ == "slider") j = {{"value", value_}, {"frozen", frozen_}};
  }
  void LoadExtra(const nlohmann::json& j) override {
    if (type_ != "slider" || !j.is_object()) return;
    value_ = j.value("value", value_);
    frozen_ = canFreeze_ && j.value("frozen", false);
  }

 private:
  std::string PatchName() const { return Id(); }

  bool WriteBytes(uintptr_t addr, const std::vector<uint8_t>& b) { return !b.empty() && mem::WriteRaw(addr, b.data(), b.size()); }

  bool WriteConfigured() {
    uintptr_t object = 0;
    const auto addr = ResolveTarget(def_["target"], &object);
    if (!addr) return false;
    const std::string v = Str(def_, "value", def_.contains("value") ? def_["value"].dump() : "0");
    std::vector<uint8_t> bytes;
    if (util::StartsWith(v, "max:")) {
      const auto src = game::Bindings::Get().Field(v.substr(4), object);
      const size_t n = mem::ValueSize(valueType_);
      if (!src || n == 0) return false;
      bytes.resize(n);
      if (!mem::ReadRaw(*src, bytes.data(), n)) return false;
    } else if (!mem::ParseValue(valueType_, v, bytes)) {
      return false;
    }
    return WriteBytes(*addr, bytes);
  }

  bool WriteSlider() {
    const auto addr = ResolveTarget(def_["target"]);
    std::vector<uint8_t> bytes;
    if (!addr || !EncodeNumber(valueType_, value_, bytes)) return false;
    if (valueType_ == mem::ValueType::F32) return mem::Write<float>(*addr, static_cast<float>(value_));
    return WriteBytes(*addr, bytes);
  }

  nlohmann::json def_;
  std::string type_;
  mem::ValueType valueType_ = mem::ValueType::I32;
  double min_ = 0, max_ = 100, value_ = 0;
  std::string format_;
  bool canFreeze_ = false, frozen_ = false;
};

}  // namespace

namespace detail {
uint64_t CheatTableSyncedGeneration() { return g_syncedGeneration; }
}  // namespace detail

std::string ValidateCheat(const nlohmann::json& c) {
  if (!c.is_object()) return "cheat must be a JSON object";
  const std::string id = Str(c, "id");
  if (id.empty()) return "missing \"id\"";
  for (char ch : id)
    if (static_cast<unsigned char>(ch) < 0x21 || ch == '.') return "\"id\" must not contain spaces, dots or control characters";
  if (c.contains("name") && !c["name"].is_string()) return "\"name\" must be a string";
  if (!ParseCategory(Str(c, "category"))) return "unknown category '" + Str(c, "category") + "'";
  if (c.contains("hotkey") && (!c["hotkey"].is_string() || !Hotkey::Parse(c["hotkey"].get<std::string>()).Valid()))
    return "\"hotkey\" is not a valid key";
  if (c.contains("requires") && !c["requires"].is_array()) return "\"requires\" must be an array of symbol names";
  const std::string type = util::ToLower(Str(c, "type"));
  if (type == "patch") {
    if (!c.contains("site")) return "patch needs \"site\"";
    if (auto e = ValidateTarget(c["site"], "site"); !e.empty()) return e;
    if (c.contains("nop")) {
      if (!c["nop"].is_number_unsigned() || c["nop"].get<size_t>() == 0 || c["nop"].get<size_t>() > 64) return "\"nop\" must be 1..64";
      return {};
    }
    std::vector<uint8_t> b;
    if (!mem::ParseValue(mem::ValueType::Bytes, Str(c, "bytes"), b) || b.empty()) return "patch needs \"bytes\" (hex) or \"nop\"";
    return {};
  }
  if (type == "lua") return Str(c, "code").empty() ? "lua cheat needs \"code\"" : "";
  if (type != "freeze" && type != "set" && type != "slider") return "unknown type '" + type + "' (patch|freeze|set|slider|lua)";
  if (!c.contains("target")) return type + " needs \"target\"";
  if (auto e = ValidateTarget(c["target"], "target"); !e.empty()) return e;
  const auto vt = mem::ParseValueType(Str(c, "value_type", "i32"));
  if (!vt) return "unknown value_type '" + Str(c, "value_type") + "'";
  if (type == "slider") {
    if (!mem::IsNumeric(*vt)) return "slider needs a numeric value_type";
    if (!c.value("min", nlohmann::json(0)).is_number() || !c.value("max", nlohmann::json(0)).is_number()) return "slider min/max must be numbers";
    if (c.value("min", 0.0) >= c.value("max", 100.0)) return "slider min must be below max";
    return {};
  }
  if (!c.contains("value")) return type + " needs \"value\"";
  const std::string v = c["value"].is_string() ? c["value"].get<std::string>() : c["value"].dump();
  if (util::StartsWith(v, "max:")) {
    if (type != "freeze" || v.size() <= 4) return "\"max:<FieldSymbol>\" is only valid for freeze";
    if (Str(c["target"], "symbol").empty()) return "\"max:\" needs a symbol target (the object)";
    return {};
  }
  std::vector<uint8_t> bytes;
  if (!mem::ParseValue(*vt, v, bytes)) return "value '" + v + "' is not a valid " + mem::ValueTypeName(*vt);
  return {};
}

void SyncCheatTableFeatures(Registry& r) {
  const auto& bindings = game::Bindings::Get();
  g_syncedGeneration = bindings.Generation();
  std::vector<std::string> stale;
  for (const auto& f : r.All())
    if (util::StartsWith(f->Id(), "table.")) stale.push_back(f->Id());
  for (const auto& id : stale) r.Remove(id);

  const nlohmann::json cheats = bindings.Cheats();
  if (!cheats.is_array()) return;
  size_t added = 0;
  for (const auto& c : cheats) {
    const std::string err = ValidateCheat(c);
    if (!err.empty()) {
      log::Warn(kChannel, "cheat '{}' skipped: {}", c.is_object() ? Str(c, "id") : std::string("?"), err);
      continue;
    }
    const std::string type = util::ToLower(Str(c, "type"));
    const Kind kind = (type == "patch" || type == "freeze") ? Kind::Toggle : type == "slider" ? Kind::Panel : Kind::Action;
    try {
      if (r.Add(std::make_unique<CheatFeature>(c, *ParseCategory(Str(c, "category")), kind))) ++added;
    } catch (const std::exception& e) {
      log::Warn(kChannel, "cheat '{}' failed: {}", Str(c, "id"), e.what());
    }
  }
  log::Info(kChannel, "cheat table: {} feature(s) from bindings generation {}", added, g_syncedGeneration);
}

}  // namespace cg::features
