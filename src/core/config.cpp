#include "core/config.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <system_error>

#include <windows.h>

#include "core/log.h"
#include "core/paths.h"
#include "core/util.h"

namespace cg {
namespace {

using json = nlohmann::json;

constexpr uint64_t kMaxFileBytes = 16ull * 1024 * 1024;
constexpr size_t kMaxProfileName = 64;
constexpr size_t kMaxProfiles = 1000;
constexpr uint64_t kSaveRetryMs = 10'000;

// Serializes all file I/O (config file, tmp file, profiles) so concurrent saves never interleave.
std::mutex& IoMutex() {
  static std::mutex* m = new std::mutex();
  return *m;
}

std::atomic<uint64_t> g_nextSaveAttemptMs{0};

// Splits "a.b.c"; empty result means the key is invalid (empty key or empty segment).
std::vector<std::string> KeyPath(std::string_view key) {
  std::vector<std::string> parts;
  if (key.empty()) return parts;
  size_t start = 0;
  while (true) {
    const size_t dot = key.find('.', start);
    const std::string_view seg = key.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
    if (seg.empty()) return {};
    parts.emplace_back(seg);
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return parts;
}

const json* Find(const json& root, std::string_view key) {
  const auto parts = KeyPath(key);
  if (parts.empty()) return nullptr;
  const json* node = &root;
  for (const auto& p : parts) {
    if (!node->is_object()) return nullptr;
    const auto it = node->find(p);
    if (it == node->end()) return nullptr;
    node = &*it;
  }
  return node;
}

// Walks/creates objects along the key. A non-object in the way is replaced: the write wins.
json* FindOrCreate(json& root, std::string_view key) {
  const auto parts = KeyPath(key);
  if (parts.empty()) return nullptr;
  json* node = &root;
  for (const auto& p : parts) {
    if (!node->is_object()) *node = json::object();
    node = &(*node)[p];
  }
  return node;
}

// 1 and 1.0 compare equal in nlohmann, but int -> float is still a change worth persisting.
// Signed vs unsigned integers are not: the parser reads every non-negative integer as unsigned.
bool SameValue(const json& a, const json& b) {
  if (a.is_number_integer() && b.is_number_integer()) return a == b;
  return a.type() == b.type() && a == b;
}

std::string Dump(const json& j) { return j.dump(2, ' ', false, json::error_handler_t::replace) + "\n"; }

std::string PathUtf8(const std::filesystem::path& p) {
  try {
    return util::Narrow(p.wstring());
  } catch (...) {
    return "?";
  }
}

std::filesystem::path WithSuffix(const std::filesystem::path& p, const wchar_t* suffix) {
  std::filesystem::path out = p;
  out += suffix;
  return out;
}

enum class ReadResult { Ok, Missing, Failed, TooLarge };

ReadResult ReadWholeFile(const std::filesystem::path& file, std::string& out) {
  const HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? ReadResult::Missing : ReadResult::Failed;
  }
  ReadResult result = ReadResult::Ok;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h, &size) || size.QuadPart < 0) {
    result = ReadResult::Failed;
  } else if (static_cast<uint64_t>(size.QuadPart) > kMaxFileBytes) {
    result = ReadResult::TooLarge;
  } else {
    try {
      out.assign(static_cast<size_t>(size.QuadPart), '\0');
      size_t got = 0;
      while (got < out.size()) {
        DWORD n = 0;
        if (!ReadFile(h, out.data() + got, static_cast<DWORD>(out.size() - got), &n, nullptr)) {
          result = ReadResult::Failed;
          break;
        }
        if (n == 0) break;
        got += n;
      }
      out.resize(got);
    } catch (...) {
      result = ReadResult::Failed;
    }
  }
  CloseHandle(h);
  return result;
}

// Writes <file>.tmp, flushes it, then atomically replaces <file>. Caller holds IoMutex().
bool WriteFileAtomic(const std::filesystem::path& file, std::string_view data, DWORD& error) {
  error = 0;
  std::error_code ec;
  if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
  const std::filesystem::path tmp = WithSuffix(file, L".tmp");
  const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    error = GetLastError();
    return false;
  }
  bool ok = true;
  size_t done = 0;
  while (done < data.size()) {
    DWORD n = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 20));
    if (!WriteFile(h, data.data() + done, chunk, &n, nullptr) || n == 0) {
      error = GetLastError();
      ok = false;
      break;
    }
    done += n;
  }
  if (ok && !FlushFileBuffers(h)) {
    error = GetLastError();
    ok = false;
  }
  CloseHandle(h);
  if (ok && !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    error = GetLastError();
    ok = false;
  }
  if (!ok) DeleteFileW(tmp.c_str());
  return ok;
}

// Parses a settings document. Only a JSON object is a valid store.
bool ParseStore(std::string_view text, json& out) {
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF)
    text.remove_prefix(3);
  json parsed = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
  if (parsed.is_discarded() || !parsed.is_object()) return false;
  out = std::move(parsed);
  return true;
}

bool IsReservedDeviceName(const std::string& name) {
  static constexpr const char* kReserved[] = {"CON", "PRN", "AUX", "NUL"};
  for (const char* r : kReserved)
    if (util::IEquals(name, r)) return true;
  if (name.size() == 4 && (util::IEquals(name.substr(0, 3), "COM") || util::IEquals(name.substr(0, 3), "LPT")) &&
      name[3] >= '0' && name[3] <= '9')
    return true;
  return false;
}

// Keeps [A-Za-z0-9 _-], trims spaces (Windows strips trailing ones), caps length and steers
// clear of DOS device names. Empty result = invalid name.
std::string SanitizeProfileName(std::string_view name) {
  std::string out;
  for (char c : name) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' ||
                    c == '_' || c == '-';
    if (ok) out.push_back(c);
  }
  out = util::Trim(out);
  if (out.size() > kMaxProfileName) out = util::Trim(out.substr(0, kMaxProfileName));
  if (!out.empty() && IsReservedDeviceName(out)) out.insert(out.begin(), '_');
  return out;
}

std::filesystem::path ProfileDir() { return paths::Data(L"profiles"); }

std::filesystem::path ProfileFile(const std::string& sanitized) {
  return ProfileDir() / (util::Widen(sanitized) + L".json");
}

}  // namespace

Config& Config::Get() {
  // Leaked on purpose: SaveIfDirty may run from late shutdown paths after static destruction began.
  static Config* instance = new Config();
  return *instance;
}

bool Config::Load(const std::filesystem::path& file) {
  try {
    std::lock_guard io(IoMutex());
    {
      std::lock_guard lock(mutex_);
      file_ = file;
    }
    if (file.empty()) {
      std::lock_guard lock(mutex_);
      root_ = json::object();
      dirty_ = false;
      return true;
    }
    std::string text;
    const ReadResult rr = ReadWholeFile(file, text);
    if (rr == ReadResult::Missing) {
      std::lock_guard lock(mutex_);
      root_ = json::object();
      dirty_ = false;
      log::Info("config", "no settings file yet at '{}'; starting with defaults", PathUtf8(file));
      return true;
    }
    json parsed;
    if (rr == ReadResult::Ok && ParseStore(text, parsed)) {
      std::lock_guard lock(mutex_);
      root_ = std::move(parsed);
      dirty_ = false;
      log::Info("config", "loaded '{}'", PathUtf8(file));
      return true;
    }

    const std::filesystem::path backup = WithSuffix(file, L".bak");
    const bool backedUp = CopyFileW(file.c_str(), backup.c_str(), FALSE) != 0;
    const char* why = rr == ReadResult::TooLarge ? "too large" : rr == ReadResult::Failed ? "unreadable" : "not valid JSON";
    if (backedUp)
      log::Warn("config", "settings file '{}' is {}; backed up to '{}' and starting with defaults", PathUtf8(file), why,
                PathUtf8(backup));
    else
      log::Warn("config", "settings file '{}' is {} and could not be backed up (error {}); starting with defaults",
                PathUtf8(file), why, GetLastError());
    std::lock_guard lock(mutex_);
    root_ = json::object();
    dirty_ = false;
    return false;
  } catch (const std::exception& e) {
    log::Error("config", "Load failed: {}", e.what());
    return false;
  } catch (...) {
    return false;
  }
}

bool Config::Save() {
  try {
    std::lock_guard io(IoMutex());
    std::string text;
    std::filesystem::path target;
    {
      std::lock_guard lock(mutex_);
      if (file_.empty()) return false;
      text = Dump(root_);
      target = file_;
      dirty_ = false;
    }
    DWORD err = 0;
    if (WriteFileAtomic(target, text, err)) {
      g_nextSaveAttemptMs.store(0, std::memory_order_relaxed);
      return true;
    }
    {
      std::lock_guard lock(mutex_);
      dirty_ = true;
    }
    // Back off so a read-only folder does not cost a failed write (and a log line) every second.
    g_nextSaveAttemptMs.store(util::NowMs() + kSaveRetryMs, std::memory_order_relaxed);
    log::Warn("config", "could not save '{}' (error {}); will retry", PathUtf8(target), err);
    return false;
  } catch (const std::exception& e) {
    log::Error("config", "Save failed: {}", e.what());
  } catch (...) {
  }
  try {
    std::lock_guard lock(mutex_);
    dirty_ = true;
  } catch (...) {
  }
  return false;
}

void Config::SaveIfDirty() {
  try {
    {
      std::lock_guard lock(mutex_);
      if (!dirty_ || file_.empty()) return;
    }
    if (util::NowMs() < g_nextSaveAttemptMs.load(std::memory_order_relaxed)) return;
    Save();
  } catch (...) {
  }
}

bool Config::ReadBool(std::string_view key, bool def) const {
  try {
    std::lock_guard lock(mutex_);
    const json* n = Find(root_, key);
    return (n && n->is_boolean()) ? n->get<bool>() : def;
  } catch (...) {
    return def;
  }
}

int64_t Config::ReadInt(std::string_view key, int64_t def) const {
  try {
    std::lock_guard lock(mutex_);
    const json* n = Find(root_, key);
    if (!n) return def;
    if (n->is_number_integer() && !n->is_number_unsigned()) return n->get<int64_t>();
    if (n->is_number_unsigned()) {
      const uint64_t u = n->get<uint64_t>();
      return u <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ? static_cast<int64_t>(u) : def;
    }
    if (n->is_number_float()) {
      // Hand-edited files may hold "5.0"; accept integral values that fit, nothing else.
      const double d = n->get<double>();
      if (std::isfinite(d) && std::trunc(d) == d && d >= -9.2233720368547758e18 && d < 9.2233720368547758e18)
        return static_cast<int64_t>(d);
    }
    return def;
  } catch (...) {
    return def;
  }
}

double Config::ReadDouble(std::string_view key, double def) const {
  try {
    std::lock_guard lock(mutex_);
    const json* n = Find(root_, key);
    return (n && n->is_number()) ? n->get<double>() : def;
  } catch (...) {
    return def;
  }
}

std::string Config::ReadString(std::string_view key, std::string_view def) const {
  try {
    std::lock_guard lock(mutex_);
    const json* n = Find(root_, key);
    if (n && n->is_string()) return n->get_ref<const std::string&>();
    return std::string(def);
  } catch (...) {
    try {
      return std::string(def);
    } catch (...) {
      return {};
    }
  }
}

nlohmann::json Config::ReadJson(std::string_view key) const {
  try {
    std::lock_guard lock(mutex_);
    const json* n = Find(root_, key);
    return n ? *n : json();
  } catch (...) {
    return json();
  }
}

void Config::WriteBool(std::string_view key, bool v) { WriteJson(key, json(v)); }
void Config::WriteInt(std::string_view key, int64_t v) { WriteJson(key, json(v)); }
void Config::WriteString(std::string_view key, std::string_view v) {
  try {
    WriteJson(key, json(std::string(v)));
  } catch (...) {
  }
}

void Config::WriteDouble(std::string_view key, double v) {
  // JSON has no NaN/Inf; nlohmann would serialize them as null and silently lose the setting.
  if (!std::isfinite(v)) return;
  WriteJson(key, json(v));
}

void Config::WriteJson(std::string_view key, const nlohmann::json& v) {
  try {
    std::lock_guard lock(mutex_);
    json* slot = FindOrCreate(root_, key);
    if (!slot) return;
    if (SameValue(*slot, v)) return;
    *slot = v;
    dirty_ = true;
  } catch (...) {
  }
}

void Config::Erase(std::string_view key) {
  try {
    const auto parts = KeyPath(key);
    if (parts.empty()) return;
    std::lock_guard lock(mutex_);
    json* node = &root_;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
      if (!node->is_object()) return;
      const auto it = node->find(parts[i]);
      if (it == node->end()) return;
      node = &*it;
    }
    if (!node->is_object()) return;
    if (node->erase(parts.back()) > 0) dirty_ = true;
  } catch (...) {
  }
}

std::vector<std::string> Config::ListProfiles() const {
  std::vector<std::string> names;
  try {
    std::lock_guard io(IoMutex());
    std::error_code ec;
    std::filesystem::directory_iterator it(ProfileDir(), ec), end;
    for (; !ec && it != end && names.size() < kMaxProfiles; it.increment(ec)) {
      std::error_code fec;
      if (!it->is_regular_file(fec)) continue;
      const std::filesystem::path& p = it->path();
      if (!util::IEquals(util::Narrow(p.extension().wstring()), ".json")) continue;
      std::string stem = util::Narrow(p.stem().wstring());
      // Only names that survive sanitization can be loaded/deleted again by name.
      if (stem.empty() || SanitizeProfileName(stem) != stem) continue;
      names.push_back(std::move(stem));
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
      return util::ToLower(a) < util::ToLower(b);
    });
  } catch (...) {
  }
  return names;
}

bool Config::SaveProfile(const std::string& name) {
  try {
    const std::string clean = SanitizeProfileName(name);
    if (clean.empty()) {
      log::Warn("config", "profile name '{}' has no usable characters", name);
      return false;
    }
    std::string text;
    {
      std::lock_guard lock(mutex_);
      text = Dump(root_);
    }
    std::lock_guard io(IoMutex());
    const std::filesystem::path file = ProfileFile(clean);
    DWORD err = 0;
    if (!WriteFileAtomic(file, text, err)) {
      log::Warn("config", "could not save profile '{}' (error {})", clean, err);
      return false;
    }
    log::Info("config", "saved profile '{}'", clean);
    return true;
  } catch (const std::exception& e) {
    log::Error("config", "SaveProfile failed: {}", e.what());
  } catch (...) {
  }
  return false;
}

bool Config::LoadProfile(const std::string& name) {
  std::vector<Listener> toNotify;
  try {
    const std::string clean = SanitizeProfileName(name);
    if (clean.empty()) return false;
    std::string text;
    {
      std::lock_guard io(IoMutex());
      const ReadResult rr = ReadWholeFile(ProfileFile(clean), text);
      if (rr != ReadResult::Ok) {
        log::Warn("config", "profile '{}' could not be read", clean);
        return false;
      }
    }
    json parsed;
    if (!ParseStore(text, parsed)) {
      log::Warn("config", "profile '{}' is not a valid settings file", clean);
      return false;
    }
    {
      std::lock_guard lock(mutex_);
      root_ = std::move(parsed);
      dirty_ = true;   // the live config file now follows the loaded profile
      toNotify = listeners_;
    }
    log::Info("config", "loaded profile '{}'", clean);
  } catch (const std::exception& e) {
    log::Error("config", "LoadProfile failed: {}", e.what());
    return false;
  } catch (...) {
    return false;
  }
  for (Listener fn : toNotify) {
    try {
      fn();
    } catch (const std::exception& e) {
      log::Error("config", "reload listener threw: {}", e.what());
    } catch (...) {
      log::Error("config", "reload listener threw an unknown exception");
    }
  }
  return true;
}

bool Config::DeleteProfile(const std::string& name) {
  try {
    const std::string clean = SanitizeProfileName(name);
    if (clean.empty()) return false;
    std::lock_guard io(IoMutex());
    if (!DeleteFileW(ProfileFile(clean).c_str())) {
      log::Warn("config", "could not delete profile '{}' (error {})", clean, GetLastError());
      return false;
    }
    log::Info("config", "deleted profile '{}'", clean);
    return true;
  } catch (...) {
    return false;
  }
}

void Config::AddReloadListener(Listener fn) {
  if (!fn) return;
  try {
    std::lock_guard lock(mutex_);
    if (std::find(listeners_.begin(), listeners_.end(), fn) == listeners_.end()) listeners_.push_back(fn);
  } catch (...) {
  }
}

}  // namespace cg
