#pragma once
// Thread-safe logger: file sink (Consigliere/consigliere.log) + in-memory ring for the UI console.
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace cg::log {

enum class Level : uint8_t { Trace, Debug, Info, Warn, Error };

struct Entry {
  uint64_t timeMs;       // util::NowMs() at write time
  Level level;
  std::string channel;   // "core", "mem", "render", "bindings", "lua", ...
  std::string text;
};

void Init(const std::filesystem::path& file);   // truncates file; safe to call once
void Shutdown();                                 // flushes and closes file
void SetMinLevel(Level lvl);                     // default Info (Debug in debug builds)
Level MinLevel();

void Write(Level lvl, std::string_view channel, std::string_view text);

// UI access. Generation() increments on every Write/Clear; Copy() returns the ring (oldest first, max 4096).
uint64_t Generation();
std::vector<Entry> Copy();
void Clear();
const char* LevelName(Level lvl);

template <class... A> void Trace(std::string_view ch, std::format_string<A...> f, A&&... a) { Write(Level::Trace, ch, std::format(f, std::forward<A>(a)...)); }
template <class... A> void Debug(std::string_view ch, std::format_string<A...> f, A&&... a) { Write(Level::Debug, ch, std::format(f, std::forward<A>(a)...)); }
template <class... A> void Info(std::string_view ch, std::format_string<A...> f, A&&... a) { Write(Level::Info, ch, std::format(f, std::forward<A>(a)...)); }
template <class... A> void Warn(std::string_view ch, std::format_string<A...> f, A&&... a) { Write(Level::Warn, ch, std::format(f, std::forward<A>(a)...)); }
template <class... A> void Error(std::string_view ch, std::format_string<A...> f, A&&... a) { Write(Level::Error, ch, std::format(f, std::forward<A>(a)...)); }

}  // namespace cg::log
