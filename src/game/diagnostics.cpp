#include "game/diagnostics.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cg_version.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/tasks.h"
#include "core/util.h"
#include "game/bindings.h"
#include "game/script_vm.h"
#include "mem/module.h"
#include "mem/pattern.h"
#include "mem/rtti.h"
#include "mem/safe.h"

namespace cg::game::diagnostics {
namespace {

std::atomic<bool> g_running{false};

std::string Rva(const mem::Module& m, uintptr_t a) { return m.Contains(a) ? util::Hex(a - m.base) : util::Hex(a); }

std::string Bytes(uintptr_t at, size_t n) {
  std::vector<uint8_t> buf(n);
  return mem::ReadRaw(at, buf.data(), n) ? util::HexBytes(buf.data(), n) : std::string("<unreadable>");
}

// Every pattern string that appears in a symbol definition (steps and fallbacks), per symbol.
std::map<std::string, std::vector<std::string>> PatternsFromBindings() {
  std::map<std::string, std::vector<std::string>> out;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(paths::Data(L"bindings"), ec)) {
    if (entry.path().extension() != L".json") continue;
    try {
      std::ifstream f(entry.path(), std::ios::binary);
      const auto j = nlohmann::json::parse(f, nullptr, false);
      if (!j.is_object() || !j.contains("symbols") || !j["symbols"].is_object()) continue;
      for (const auto& [name, def] : j["symbols"].items()) {
        std::vector<const nlohmann::json*> chains;
        if (def.contains("steps")) chains.push_back(&def["steps"]);
        if (def.contains("fallbacks") && def["fallbacks"].is_array())
          for (const auto& fb : def["fallbacks"]) chains.push_back(&fb);
        for (const auto* chain : chains) {
          if (!chain->is_array()) continue;
          for (const auto& step : *chain)
            if (step.is_object() && step.contains("pattern") && step["pattern"].is_string())
              out[name].push_back(step["pattern"].get<std::string>());
        }
      }
    } catch (...) {
    }
  }
  return out;
}

std::vector<std::string> Tokens(const std::string& ida) { return util::Split(ida, ' '); }

std::string Join(const std::vector<std::string>& t, size_t n) {
  std::string s;
  for (size_t i = 0; i < n && i < t.size(); ++i) s += (i ? " " : "") + t[i];
  return s;
}

void DumpMatches(std::ostringstream& o, const mem::Module& m, const std::vector<uintptr_t>& hits, size_t maxShown) {
  for (size_t i = 0; i < hits.size() && i < maxShown; ++i) {
    const uintptr_t a = hits[i];
    const auto fn = mem::FunctionStart(a);
    o << "      @" << Rva(m, a) << (fn ? " (fn " + Rva(m, *fn) + ")" : std::string()) << "\n";
    o << "        -16: " << Bytes(a - 16, 16) << "\n";
    o << "        +0 : " << Bytes(a, 48) << "\n";
  }
}

std::string Build() {
  std::ostringstream o;
  const mem::Module& m = mem::Module::Main();
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
  std::error_code ec;
  o << "Chroma " << CG_VERSION << " diagnostics\n";
  o << "host " << m.name << " | image size " << util::Hex(m.size) << " | PE timestamp " << util::Hex(nt->FileHeader.TimeDateStamp)
    << " | checksum " << util::Hex(nt->OptionalHeader.CheckSum) << " | file size "
    << std::filesystem::file_size(paths::GameExe(), ec) << "\n";
  for (const auto& sec : m.sections) o << "  section " << sec.name << " rva " << util::Hex(sec.start - m.base) << " size " << util::Hex(sec.size) << "\n";
  o << "script VM: " << vm::StatusText() << "\n\n";

  o << "== Bindings ==\n";
  for (const auto& s : Bindings::Get().List()) {
    o << "  " << s.name << " [" << SymbolKindName(s.kind) << "] " << SymbolStatusName(s.status);
    if (!s.error.empty()) o << " - " << s.error;
    if (s.status == SymbolStatus::Resolved || s.status == SymbolStatus::Overridden) o << " = " << Rva(m, s.staticValue);
    o << "\n";
  }

  o << "\n== Pattern near-misses (shorter prefixes of each shipped pattern) ==\n";
  for (const auto& [name, patterns] : PatternsFromBindings()) {
    for (const auto& ida : patterns) {
      o << "  " << name << ": " << ida << "\n";
      const auto tokens = Tokens(ida);
      std::set<size_t> lengths{tokens.size()};
      for (size_t n : {24, 20, 16, 12, 10, 8, 6})
        if (n < tokens.size()) lengths.insert(n);
      for (auto it = lengths.rbegin(); it != lengths.rend(); ++it) {
        const std::string prefix = Join(tokens, *it);
        const auto pat = mem::Pattern::Parse(prefix);
        if (!pat) continue;
        const auto hits = mem::FindAllInModule(*pat, m, true, 6);
        o << "    first " << *it << " bytes: " << (hits.size() >= 6 ? "6+" : std::to_string(hits.size())) << " match(es)\n";
        if (!hits.empty() && hits.size() <= 3) DumpMatches(o, m, hits, 3);
        if (hits.size() == 1) break;   // a unique prefix is the most useful near-miss; shorter adds noise
      }
    }
  }

  o << "\n== Script machine offset accesses (mov reg,[reg+0D0h]) ==\n";
  for (const char* ida : {"48 8B 89 D0 00 00 00", "48 8B 8B D0 00 00 00", "48 8B 81 D0 00 00 00", "48 8B 8F D0 00 00 00"}) {
    const auto pat = mem::Pattern::Parse(ida);
    const auto hits = mem::FindAllInModule(*pat, m, true, 12);
    o << "  " << ida << ": " << (hits.size() >= 12 ? "12+" : std::to_string(hits.size())) << " shown\n";
    DumpMatches(o, m, hits, 12);
  }

  o << "\n== String anchors and code references ==\n";
  for (const char* text : {"luaL_loadbuffer", "lua_pcall", "loadstring", "HavokScript", "Havok Script", "hks", "C_ScriptMachine",
                           "C_ScriptGameMachine", "ScriptMachine", "game_structure_console", "carpls", "GetActivePlayer",
                           "SetDemigod", "InventorySetUnlimitedAmmo", "attempt to call", "PANIC: unprotected error", "=stdin",
                           "_LOADED", "__index"}) {
    const auto at = mem::FindString(m, text);
    if (!at) {
      o << "  \"" << text << "\": not found\n";
      continue;
    }
    const auto refs = mem::FindXrefs(m, *at, 6);
    o << "  \"" << text << "\" @" << Rva(m, *at) << " refs " << refs.size() << "\n";
    for (uintptr_t r : refs) {
      const auto fn = mem::FunctionStart(r);
      o << "    ref @" << Rva(m, r) << (fn ? " fn " + Rva(m, *fn) : std::string()) << " : " << Bytes(r, 24) << "\n";
    }
  }

  o << "\n== RTTI ==\n";
  const auto& classes = mem::rtti::Index(m);
  o << "  " << classes.size() << " classes with MSVC type descriptors\n";
  size_t shown = 0;
  for (const auto& c : classes) {
    if (!util::IContains(c.name, "script") && !util::IContains(c.name, "lua") && !util::IContains(c.name, "hks")) continue;
    o << "  " << c.name;
    if (!c.vtables.empty()) o << " vtable " << Rva(m, c.vtables.front().address);
    o << "\n";
    if (++shown >= 60) break;
  }
  return o.str();
}

}  // namespace

bool Running() { return g_running.load(); }

void WriteReportAsync(std::function<void(bool ok, std::wstring path)> done) {
  if (g_running.exchange(true)) return;
  tasks::RunAsync([done] {
    const std::wstring path = paths::Data(L"diagnostics.txt").wstring();
    bool ok = false;
    try {
      const std::string text = Build();
      std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
      f << text;
      ok = static_cast<bool>(f);
    } catch (const std::exception& e) {
      log::Error("diag", "diagnostics report failed: {}", e.what());
    } catch (...) {
      log::Error("diag", "diagnostics report failed");
    }
    if (ok) log::Info("diag", "diagnostics report written: {}", util::Narrow(path));
    g_running.store(false);
    if (done) tasks::PostRender([done, ok, path] { done(ok, path); });
  });
}

void AutoReportIfVmUnbound() {
  if (vm::MissingBindings().empty()) return;
  // Once per game build: the marker holds the PE timestamp of the exe we already reported on.
  const mem::Module& m = mem::Module::Main();
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
  const std::string stamp = util::Hex(nt->FileHeader.TimeDateStamp);
  const auto marker = paths::Data(L"diagnostics.build");
  std::string previous;
  {
    std::ifstream in(marker);
    std::getline(in, previous);
  }
  if (previous == stamp && std::filesystem::exists(paths::Data(L"diagnostics.txt"))) return;
  std::ofstream(marker, std::ios::trunc) << stamp;
  log::Info("diag", "script VM bindings unresolved on this build; writing Chroma\\diagnostics.txt for troubleshooting");
  WriteReportAsync();
}

}  // namespace cg::game::diagnostics
