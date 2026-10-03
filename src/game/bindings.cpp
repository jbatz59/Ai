#include "game/bindings.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <span>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <windows.h>

#include "core/log.h"
#include "core/util.h"
#include "mem/module.h"
#include "mem/pattern.h"
#include "mem/rtti.h"
#include "mem/safe.h"

namespace cg::game {
namespace {

using json = nlohmann::json;
constexpr std::string_view kCh = "bindings";
constexpr const wchar_t* kOverridesFile = L"zz_user_overrides.json";

// ---------------------------------------------------------------------------------------------
// Definitions
// ---------------------------------------------------------------------------------------------
enum class Op : uint8_t { Module, Pattern, RttiVtable, RttiInstance, String, Xref, FunctionStart, Symbol, Export, Add, Rip, Call, Deref, VFunc };

const char* OpName(Op op) {
  switch (op) {
    case Op::Module: return "module";
    case Op::Pattern: return "pattern";
    case Op::RttiVtable: return "rtti_vtable";
    case Op::RttiInstance: return "rtti_instance";
    case Op::String: return "string";
    case Op::Xref: return "xref";
    case Op::FunctionStart: return "function_start";
    case Op::Symbol: return "symbol";
    case Op::Export: return "export";
    case Op::Add: return "add";
    case Op::Rip: return "rip";
    case Op::Call: return "call";
    case Op::Deref: return "deref";
    case Op::VFunc: return "vfunc";
  }
  return "?";
}

bool IsDynamicOp(Op op) { return op == Op::Deref || op == Op::VFunc; }
// Ops that only read memory around the cursor and may therefore run on every Addr() call.
bool IsTailOp(Op op) { return op == Op::Deref || op == Op::VFunc || op == Op::Add || op == Op::Rip || op == Op::Call; }

struct Step {
  Op op = Op::Add;
  std::string text;      // module / class / string / symbol / export dll / section
  std::string text2;     // export function name
  bool wide = false;     // string
  bool unique = false;   // pattern: fail unless it matches exactly once (ambiguous = unsafe)
  int64_t n = 0;         // add amount, xref index, pattern index, vfunc index, rip disp offset
  int64_t n2 = 0;        // rip instruction length
  mem::Pattern pattern;  // pattern
};
using Chain = std::vector<Step>;

struct Def {
  std::string name;
  SymbolKind kind = SymbolKind::Address;
  std::vector<Chain> chains;   // [0] = "steps", then "fallbacks"
  std::vector<int64_t> fieldChain;
  int64_t value = 0;
  std::string type, expectClass, notes, sourceFile;
  bool verified = false;
  bool pinned = false;
  std::string loadError;       // non-empty => definition invalid
};

std::optional<int64_t> JsonInt(const json& j) {
  if (j.is_number_integer()) return j.get<int64_t>();
  if (j.is_number_unsigned()) return static_cast<int64_t>(j.get<uint64_t>());
  if (j.is_string()) {
    std::string s = util::Trim(j.get<std::string>());
    if (!s.empty() && s[0] == '+') s.erase(0, 1);
    return util::ParseInt(s, false);
  }
  return std::nullopt;
}

std::optional<SymbolKind> ParseKind(std::string_view s) {
  static constexpr std::pair<std::string_view, SymbolKind> kKinds[] = {
      {"address", SymbolKind::Address}, {"pointer", SymbolKind::Pointer}, {"function", SymbolKind::Function},
      {"offset", SymbolKind::Offset},   {"field", SymbolKind::Field},     {"constant", SymbolKind::Constant}};
  for (const auto& [n, k] : kKinds)
    if (util::IEquals(n, s)) return k;
  return std::nullopt;
}

bool JsonTruthy(const json& j) {
  if (j.is_boolean()) return j.get<bool>();
  if (j.is_number()) return j.get<double>() != 0;
  return !j.is_null();
}

// Parses one step object. Returns false with `err` set on malformed input; `skip` = no-op step.
bool ParseStep(const json& j, Step& s, bool& skip, std::string& err) {
  skip = false;
  if (!j.is_object()) { err = "step is not an object"; return false; }
  static constexpr std::pair<std::string_view, Op> kOps[] = {
      {"module", Op::Module}, {"pattern", Op::Pattern}, {"rtti_vtable", Op::RttiVtable}, {"rtti_instance", Op::RttiInstance},
      {"string", Op::String}, {"xref", Op::Xref}, {"function_start", Op::FunctionStart}, {"symbol", Op::Symbol},
      {"export", Op::Export}, {"add", Op::Add}, {"sub", Op::Add}, {"rip", Op::Rip}, {"call", Op::Call},
      {"deref", Op::Deref}, {"vfunc", Op::VFunc}};
  const json* arg = nullptr;
  std::string_view key;
  for (const auto& [k, op] : kOps) {
    auto it = j.find(std::string(k));
    if (it == j.end()) continue;
    if (arg) { err = "step has more than one operation ('" + std::string(key) + "' and '" + std::string(k) + "')"; return false; }
    arg = &*it;
    key = k;
    s.op = op;
  }
  if (!arg) { err = "unknown step " + j.dump(); return false; }
  const std::string keyStr(key);
  auto needString = [&](std::string& out, bool allowEmpty) {
    if (!arg->is_string()) { err = "'" + keyStr + "' must be a string"; return false; }
    out = arg->get<std::string>();
    if (!allowEmpty && out.empty()) { err = "'" + keyStr + "' must not be empty"; return false; }
    return true;
  };
  auto needInt = [&](const json& v, const char* what, int64_t& out) {
    auto n = JsonInt(v);
    if (!n) { err = std::string("'") + what + "' must be an integer (number or \"0x..\" string)"; return false; }
    out = *n;
    return true;
  };
  switch (s.op) {
    case Op::Module:
      if (arg->is_null()) return true;
      return needString(s.text, true);
    case Op::Pattern: {
      if (!needString(s.text, false)) return false;
      auto p = mem::Pattern::Parse(s.text);
      if (!p || p->Size() == 0) { err = "invalid pattern '" + s.text + "'"; return false; }
      s.pattern = std::move(*p);
      if (auto it = j.find("index"); it != j.end()) {
        if (!needInt(*it, "index", s.n)) return false;
        if (s.n < 0 || s.n > 100000) { err = "'index' out of range"; return false; }
      }
      if (auto it = j.find("section"); it != j.end()) {
        if (!it->is_string()) { err = "'section' must be a string"; return false; }
        s.text2 = it->get<std::string>();
      }
      if (auto it = j.find("unique"); it != j.end()) {
        if (!it->is_boolean()) { err = "'unique' must be true or false"; return false; }
        s.unique = it->get<bool>();
        if (s.unique && s.n != 0) { err = "'unique' cannot be combined with 'index'"; return false; }
      }
      return true;
    }
    case Op::RttiVtable:
    case Op::RttiInstance:
    case Op::Symbol:
      return needString(s.text, false);
    case Op::String:
      if (!needString(s.text, false)) return false;
      if (auto it = j.find("wide"); it != j.end()) s.wide = JsonTruthy(*it);
      return true;
    case Op::Xref:
      if (arg->is_boolean()) { s.n = 0; return true; }
      if (!needInt(*arg, "xref", s.n)) return false;
      if (s.n < 0 || s.n > 4096) { err = "'xref' index out of range"; return false; }
      return true;
    case Op::Export: {
      std::string v;
      if (!needString(v, false)) return false;
      const size_t bang = v.find('!');
      if (bang == std::string::npos || bang + 1 >= v.size()) { err = "'export' must be \"dll!Function\""; return false; }
      s.text = util::Trim(v.substr(0, bang));
      s.text2 = util::Trim(v.substr(bang + 1));
      return true;
    }
    case Op::Add:
      if (!needInt(*arg, keyStr.c_str(), s.n)) return false;
      if (key == "sub") s.n = -s.n;
      return true;
    case Op::Rip:
      if (!arg->is_array() || arg->size() != 2) { err = "'rip' must be [dispOffset, instrLen]"; return false; }
      if (!needInt((*arg)[0], "rip[0]", s.n) || !needInt((*arg)[1], "rip[1]", s.n2)) return false;
      if (s.n < 0 || s.n2 < s.n + 4 || s.n2 > 15) { err = "'rip' values out of range"; return false; }
      return true;
    case Op::VFunc:
      if (!needInt(*arg, "vfunc", s.n)) return false;
      if (s.n < 0 || s.n > 4096) { err = "'vfunc' index out of range"; return false; }
      return true;
    case Op::FunctionStart:
    case Op::Call:
    case Op::Deref:
      skip = !JsonTruthy(*arg);
      return true;
  }
  err = "unhandled step";
  return false;
}

bool ParseChain(const json& arr, Chain& out, std::string& err) {
  if (!arr.is_array()) { err = "steps must be an array"; return false; }
  size_t i = 0;
  for (const json& js : arr) {
    ++i;
    Step s;
    bool skip = false;
    std::string e;
    if (!ParseStep(js, s, skip, e)) { err = "step " + std::to_string(i) + ": " + e; return false; }
    if (!skip) out.push_back(std::move(s));
  }
  if (out.empty()) { err = "empty step list"; return false; }
  return true;
}

// Builds a Def from JSON; never throws (caller catches nlohmann errors anyway). loadError is set
// when the definition is unusable.
Def ParseDef(const std::string& name, const json& j, const std::string& file) {
  Def d;
  d.name = name;
  d.sourceFile = file;
  try {
    if (!j.is_object()) { d.loadError = "definition is not an object"; return d; }
    if (auto it = j.find("type"); it != j.end() && it->is_string()) d.type = it->get<std::string>();
    if (auto it = j.find("expect_class"); it != j.end() && it->is_string()) d.expectClass = it->get<std::string>();
    if (auto it = j.find("notes"); it != j.end() && it->is_string()) d.notes = it->get<std::string>();
    if (auto it = j.find("verified"); it != j.end()) d.verified = JsonTruthy(*it);
    if (auto it = j.find("pinned"); it != j.end()) d.pinned = JsonTruthy(*it);
    auto kit = j.find("kind");
    if (kit == j.end() || !kit->is_string()) { d.loadError = "missing \"kind\""; return d; }
    auto kind = ParseKind(kit->get<std::string>());
    if (!kind) { d.loadError = "unknown kind '" + kit->get<std::string>() + "'"; return d; }
    d.kind = *kind;
    switch (d.kind) {
      case SymbolKind::Offset:
      case SymbolKind::Constant: {
        auto it = j.find("value");
        if (it == j.end()) { d.loadError = "missing \"value\""; return d; }
        auto v = JsonInt(*it);
        if (!v) { d.loadError = "\"value\" is not an integer"; return d; }
        d.value = *v;
        return d;
      }
      case SymbolKind::Field: {
        auto it = j.find("chain");
        if (it == j.end() || !it->is_array() || it->empty()) { d.loadError = "missing or empty \"chain\""; return d; }
        size_t i = 0;
        for (const json& o : *it) {
          ++i;
          auto v = JsonInt(o);
          if (!v) { d.loadError = "chain[" + std::to_string(i - 1) + "] is not an integer"; return d; }
          d.fieldChain.push_back(*v);
        }
        return d;
      }
      case SymbolKind::Address:
      case SymbolKind::Pointer:
      case SymbolKind::Function: break;
    }
    auto sit = j.find("steps");
    if (sit == j.end()) { d.loadError = "missing \"steps\""; return d; }
    std::vector<const json*> lists{&*sit};
    if (auto fit = j.find("fallbacks"); fit != j.end()) {
      if (!fit->is_array()) { d.loadError = "\"fallbacks\" must be an array of step arrays"; return d; }
      for (const json& f : *fit) lists.push_back(&f);
    }
    for (size_t li = 0; li < lists.size(); ++li) {
      Chain c;
      std::string err;
      const std::string where = li == 0 ? std::string("steps") : "fallback " + std::to_string(li);
      if (!ParseChain(*lists[li], c, err)) { d.loadError = where + ": " + err; return d; }
      bool dyn = false;
      for (size_t si = 0; si < c.size(); ++si) {
        if (IsDynamicOp(c[si].op)) {
          if (d.kind != SymbolKind::Pointer) {
            d.loadError = where + ": step " + std::to_string(si + 1) + " ('" + OpName(c[si].op) + "') is only allowed in kind \"pointer\"";
            return d;
          }
          dyn = true;
        } else if (dyn && !IsTailOp(c[si].op)) {
          d.loadError = where + ": step " + std::to_string(si + 1) + " ('" + OpName(c[si].op) +
                        "') cannot follow deref/vfunc (only add/sub/rip/call/deref/vfunc may)";
          return d;
        }
      }
      d.chains.push_back(std::move(c));
    }
  } catch (const std::exception& e) {
    d.loadError = std::string("invalid definition: ") + e.what();
  }
  return d;
}

// ---------------------------------------------------------------------------------------------
// Resolved table
// ---------------------------------------------------------------------------------------------
struct Entry {
  SymbolInfo info;
  std::vector<Step> tail;              // pointer: dynamic part
  std::vector<int64_t> chain;          // field
  int64_t value = 0;                   // offset / constant
  mutable std::atomic<uintptr_t> validObj{0};   // expect_class cache: last validated object...
  mutable std::atomic<uintptr_t> validVt{0};    // ...and its vtable at validation time
};

struct SvHash {
  using is_transparent = void;
  size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};
using Table = std::unordered_map<std::string, Entry, SvHash, std::equal_to<>>;
using DefMap = std::unordered_map<std::string, Def, SvHash, std::equal_to<>>;

std::optional<uintptr_t> EvalTail(uintptr_t cursor, std::span<const Step> tail) {
  for (const Step& s : tail) {
    switch (s.op) {
      case Op::Deref: {
        uintptr_t v = 0;
        if (!mem::ReadRaw(cursor, &v, sizeof(v)) || !mem::IsPlausiblePtr(v)) return std::nullopt;
        cursor = v;
        break;
      }
      case Op::VFunc: {
        uintptr_t vt = 0, fn = 0;
        if (!mem::ReadRaw(cursor, &vt, sizeof(vt)) || !mem::IsPlausiblePtr(vt)) return std::nullopt;
        if (!mem::ReadRaw(vt + static_cast<uintptr_t>(s.n) * 8, &fn, sizeof(fn)) || !mem::IsPlausiblePtr(fn)) return std::nullopt;
        cursor = fn;
        break;
      }
      case Op::Add: cursor += static_cast<uintptr_t>(s.n); break;
      case Op::Rip: {
        auto r = mem::ResolveRip(cursor, static_cast<int>(s.n), static_cast<int>(s.n2));
        if (!r) return std::nullopt;
        cursor = *r;
        break;
      }
      case Op::Call: {
        auto r = mem::ResolveCall(cursor);
        if (!r) return std::nullopt;
        cursor = *r;
        break;
      }
      default: return std::nullopt;
    }
  }
  return cursor;
}

struct Result {
  bool done = false;
  bool ok = false;
  uintptr_t value = 0;
  int64_t intValue = 0;
  bool dynamic = false;
  std::vector<Step> tail;
  std::string error;
  double ms = 0;
};

// Shared across resolves: rtti_instance object cache (class -> object).
struct InstanceCache {
  std::mutex mu;
  std::unordered_map<std::string, uintptr_t> objects;
};

class Resolver {
 public:
  Resolver(const DefMap& defs, std::unordered_map<std::string, Result>& memo, InstanceCache& cache,
           std::atomic<float>* progress, size_t total)
      : defs_(defs), memo_(memo), cache_(cache), progress_(progress), total_(total) {}

  const Result& Resolve(const std::string& name) {
    if (auto it = memo_.find(name); it != memo_.end() && it->second.done) return it->second;
    if (std::find(stack_.begin(), stack_.end(), name) != stack_.end()) {
      cycle_.clear();
      for (auto it = std::find(stack_.begin(), stack_.end(), name); it != stack_.end(); ++it) cycle_ += *it + " -> ";
      cycle_ += name;
      static const Result kCycle;   // placeholder; caller reports cycle_
      return kCycle;
    }
    auto dit = defs_.find(name);
    Result r;
    if (dit == defs_.end()) {
      r.error = "unknown symbol '" + name + "'";
    } else {
      stack_.push_back(name);
      const auto t0 = std::chrono::steady_clock::now();
      try {
        r = ResolveDef(dit->second);
      } catch (const std::exception& e) {
        r = Result{};
        r.error = std::string("internal error: ") + e.what();
      }
      r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      stack_.pop_back();
      ++completed_;
      if (progress_ && total_) progress_->store(std::min(1.0f, static_cast<float>(completed_) / static_cast<float>(total_)));
    }
    r.done = true;
    auto& slot = memo_[name];
    slot = std::move(r);
    return slot;
  }

 private:
  Result ResolveDef(const Def& d) {
    Result r;
    if (!d.loadError.empty()) { r.error = d.loadError; return r; }
    switch (d.kind) {
      case SymbolKind::Offset:
      case SymbolKind::Constant:
        r.ok = true;
        r.intValue = d.value;
        r.value = static_cast<uintptr_t>(d.value);
        return r;
      case SymbolKind::Field:
        r.ok = true;
        return r;
      default: break;
    }
    std::string errors;
    for (size_t ci = 0; ci < d.chains.size(); ++ci) {
      std::string err;
      Result cr;
      if (RunChain(d, d.chains[ci], cr, err)) {
        if (!d.expectClass.empty() && !cr.dynamic && !mem::rtti::IsA(cr.value, d.expectClass)) {
          const std::string actual = mem::rtti::ClassNameOf(cr.value);
          err = "expect_class '" + d.expectClass + "' failed (object at " + util::Hex(cr.value) + " is '" +
                (actual.empty() ? std::string("not polymorphic") : actual) + "')";
        } else {
          cr.ok = true;
          return cr;
        }
      }
      if (!errors.empty()) errors += "; ";
      errors += (ci == 0 ? std::string("steps") : "fallback " + std::to_string(ci)) + ": " + err;
    }
    r.error = errors;
    return r;
  }

  bool RunChain(const Def& d, const Chain& chain, Result& out, std::string& err) {
    uintptr_t cursor = 0;
    std::optional<mem::Module> modStore;
    auto mod = [&]() -> const mem::Module& { return modStore ? *modStore : mem::Module::Main(); };
    for (size_t i = 0; i < chain.size(); ++i) {
      const Step& s = chain[i];
      const std::string at = "step " + std::to_string(i + 1) + " (" + OpName(s.op) + "): ";
      if (IsDynamicOp(s.op)) {   // static prefix ends here; the rest is evaluated on every Addr()
        out.value = cursor;
        out.dynamic = true;
        out.tail.assign(chain.begin() + static_cast<ptrdiff_t>(i), chain.end());
        if (!mem::IsPlausiblePtr(cursor)) { err = at + "static prefix " + util::Hex(cursor) + " is not a plausible address"; return false; }
        return true;
      }
      switch (s.op) {
        case Op::Module: {
          if (s.text.empty() || util::IEquals(s.text, mem::Module::Main().name)) {
            modStore.reset();
          } else {
            auto m = mem::Module::Find(s.text);
            if (!m) { err = at + "module '" + s.text + "' is not loaded"; return false; }
            modStore = std::move(*m);
          }
          cursor = mod().base;
          if (!cursor) { err = at + "module base unavailable"; return false; }
          break;
        }
        case Op::Pattern: {
          const mem::Module& m = mod();
          const size_t need = static_cast<size_t>(s.n) + 1;   // matches required for the index
          const size_t want = s.unique ? 2 : need;             // unique: look for a second one too
          std::vector<uintptr_t> hits;
          std::string where;
          if (s.text2.empty()) {
            hits = mem::FindAllInModule(s.pattern, m, true, want);
            where = "executable sections";
          } else if (util::IEquals(s.text2, "any")) {
            hits = mem::FindAllInModule(s.pattern, m, false, want);
            where = "any section";
          } else {
            const mem::Section* sec = m.FindSection(s.text2);
            if (!sec) { err = at + "section '" + s.text2 + "' not found in " + m.name; return false; }
            hits = mem::FindAll(s.pattern, sec->start, sec->size, want);
            where = s.text2;
          }
          if (hits.size() < need) {
            err = at + (hits.empty() ? std::string("no match") : "only " + std::to_string(hits.size()) + " match(es), index " + std::to_string(s.n) + " requested") +
                  " for '" + s.text + "' in " + where + " of " + m.name;
            return false;
          }
          if (s.unique && hits.size() > 1) {
            err = at + "'" + s.text + "' matches more than once in " + where + " of " + m.name + " (ambiguous; 'unique' requested)";
            return false;
          }
          cursor = hits[static_cast<size_t>(s.n)];
          break;
        }
        case Op::RttiVtable: {
          auto vt = mem::rtti::PrimaryVTable(mod(), s.text);
          if (!vt) { err = at + "RTTI class '" + s.text + "' (or its primary vtable) not found in " + mod().name; return false; }
          cursor = *vt;
          break;
        }
        case Op::RttiInstance: {
          auto vt = mem::rtti::PrimaryVTable(mod(), s.text);
          if (!vt) { err = at + "RTTI class '" + s.text + "' not found in " + mod().name; return false; }
          uintptr_t obj = 0;
          {
            std::lock_guard lk(cache_.mu);
            if (auto it = cache_.objects.find(s.text); it != cache_.objects.end()) obj = it->second;
          }
          if (obj && mem::ReadOr<uintptr_t>(obj, 0) != *vt) obj = 0;
          if (!obj) {
            auto found = mem::rtti::FindInstances(*vt, 1);
            if (found.empty()) { err = at + "no live instance of '" + s.text + "' found"; return false; }
            obj = found.front();
            std::lock_guard lk(cache_.mu);
            cache_.objects[s.text] = obj;
          }
          cursor = obj;
          break;
        }
        case Op::String: {
          auto a = mem::FindString(mod(), s.text, s.wide);
          if (!a) { err = at + std::string(s.wide ? "wide " : "") + "string \"" + s.text + "\" not found in " + mod().name; return false; }
          cursor = *a;
          break;
        }
        case Op::Xref: {
          auto refs = mem::FindXrefs(mod(), cursor, static_cast<size_t>(s.n) + 1);
          if (refs.size() <= static_cast<size_t>(s.n)) {
            err = at + "found " + std::to_string(refs.size()) + " xref(s) to " + util::Hex(cursor) + ", index " + std::to_string(s.n) + " requested";
            return false;
          }
          cursor = refs[static_cast<size_t>(s.n)];
          break;
        }
        case Op::FunctionStart: {
          auto f = mem::FunctionStart(cursor);
          if (!f) { err = at + "no .pdata function contains " + util::Hex(cursor); return false; }
          cursor = *f;
          break;
        }
        case Op::Symbol: {
          const Result& dep = Resolve(s.text);
          if (!dep.done) { err = at + "dependency cycle: " + cycle_; return false; }
          if (!dep.ok) { err = at + "dependency '" + s.text + "' failed"; return false; }
          auto dit = defs_.find(s.text);
          if (dit != defs_.end() && dit->second.kind == SymbolKind::Field) { err = at + "'" + s.text + "' is a field and has no value"; return false; }
          if (dep.dynamic) {
            if (d.kind != SymbolKind::Pointer) { err = at + "'" + s.text + "' is dynamic; only kind \"pointer\" may depend on it"; return false; }
            for (size_t k = i + 1; k < chain.size(); ++k)
              if (!IsTailOp(chain[k].op)) { err = at + "step " + std::to_string(k + 1) + " ('" + OpName(chain[k].op) + "') cannot follow a dynamic symbol"; return false; }
            out.value = dep.value;
            out.dynamic = true;
            out.tail = dep.tail;
            out.tail.insert(out.tail.end(), chain.begin() + static_cast<ptrdiff_t>(i) + 1, chain.end());
            return true;
          }
          cursor = dep.value;
          break;
        }
        case Op::Export: {
          std::string dll = s.text;
          if (dll.empty()) dll = mem::Module::Main().name;
          auto a = mem::FindExport(dll, s.text2);
          if (!a) {
            err = at + (mem::Module::Find(dll) ? "export '" + s.text2 + "' not found in " + dll : "module '" + dll + "' is not loaded");
            return false;
          }
          cursor = *a;
          break;
        }
        case Op::Add: cursor += static_cast<uintptr_t>(s.n); break;
        case Op::Rip: {
          auto a = mem::ResolveRip(cursor, static_cast<int>(s.n), static_cast<int>(s.n2));
          if (!a) { err = at + "cannot read rel32 at " + util::Hex(cursor + static_cast<uintptr_t>(s.n)); return false; }
          cursor = *a;
          break;
        }
        case Op::Call: {
          auto a = mem::ResolveCall(cursor);
          if (!a) { err = at + "no E8/E9 rel32 at " + util::Hex(cursor); return false; }
          cursor = *a;
          break;
        }
        case Op::Deref:
        case Op::VFunc: break;   // handled above
      }
    }
    if (!mem::IsPlausiblePtr(cursor)) { err = "result " + util::Hex(cursor) + " is not a plausible address"; return false; }
    out.value = cursor;
    return true;
  }

  const DefMap& defs_;
  std::unordered_map<std::string, Result>& memo_;
  InstanceCache& cache_;
  std::atomic<float>* progress_;
  size_t total_;
  size_t completed_ = 0;
  std::vector<std::string> stack_;
  std::string cycle_;
};

void FillEntry(Entry& e, const Def& d, const Result& r) {
  e.info.name = d.name;
  e.info.kind = d.kind;
  e.info.verified = d.verified;
  e.info.type = d.type;
  e.info.expectClass = d.expectClass;
  e.info.notes = d.notes;
  e.info.sourceFile = d.sourceFile;
  e.info.resolveMs = r.ms;
  e.info.dynamic = r.ok && r.dynamic;
  e.info.staticValue = r.ok ? r.value : 0;
  e.info.error = r.ok ? std::string() : r.error;
  e.info.status = !r.ok ? SymbolStatus::Failed : (d.pinned ? SymbolStatus::Overridden : SymbolStatus::Resolved);
  e.tail = r.ok ? r.tail : std::vector<Step>{};
  e.chain = d.fieldChain;
  e.value = r.intValue;
  e.validObj.store(0);
  e.validVt.store(0);
}

Result ResultFromEntry(const Entry& e) {
  Result r;
  r.done = true;
  r.ok = e.info.status == SymbolStatus::Resolved || e.info.status == SymbolStatus::Overridden;
  r.value = e.info.staticValue;
  r.intValue = e.value;
  r.dynamic = e.info.dynamic;
  r.tail = e.tail;
  r.error = e.info.error;
  r.ms = e.info.resolveMs;
  return r;
}

json HexJson(uint64_t v) { return util::Hex(v); }

}  // namespace

// ---------------------------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------------------------
struct Bindings::Impl {
  mutable std::shared_mutex mu;   // guards table, cheats, gameBuild, loadErrors, defs, overrides, dir
  Table table;
  json cheats = json::array();
  std::string gameBuild;
  std::vector<std::string> loadErrors;
  DefMap defs;          // merged definitions (incl. overrides)
  DefMap baseDefs;      // merged definitions without the overrides file
  json overrides = json::object();   // contents of zz_user_overrides.json ("symbols" object)
  std::filesystem::path dir;

  std::mutex opMu;      // serialises writers (load / pin / define)
  std::atomic<bool> resolving{false};
  std::atomic<float> progress{0};
  std::atomic<uint64_t> generation{0};
  InstanceCache instances;

  bool Load(const std::filesystem::path& d);
  bool PersistOverrides(const std::filesystem::path& d, const json& symbols);
  void ResolveOne(const std::string& name, const Def* def);
};

bool Bindings::Impl::PersistOverrides(const std::filesystem::path& d, const json& symbols) {
  if (d.empty()) {
    log::Warn(kCh, "no bindings directory loaded yet; override not persisted");
    return false;
  }
  try {
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    json doc = json::object();
    doc["schema"] = 1;
    doc["symbols"] = symbols;
    const std::string text = doc.dump(2);
    const std::filesystem::path file = d / kOverridesFile;
    std::filesystem::path tmp = file;
    tmp += L".tmp";
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (!f) { log::Error(kCh, "cannot write {}", util::Narrow(tmp.wstring())); return false; }
      f.write(text.data(), static_cast<std::streamsize>(text.size()));
      f.flush();
      if (!f) { log::Error(kCh, "write failed: {}", util::Narrow(tmp.wstring())); return false; }
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      log::Error(kCh, "MoveFileExW failed ({}) for {}", static_cast<unsigned long>(GetLastError()), util::Narrow(file.wstring()));
      std::filesystem::remove(tmp, ec);
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    log::Error(kCh, "persisting overrides failed: {}", e.what());
    return false;
  }
}

bool Bindings::Impl::Load(const std::filesystem::path& d) {
  std::vector<std::filesystem::path> files;
  std::vector<std::string> errors;
  {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code ec2;
      if (!it->is_regular_file(ec2)) continue;
      if (util::ToLower(util::Narrow(it->path().extension().wstring())) != ".json") continue;
      files.push_back(it->path());
    }
    if (ec) errors.push_back(util::Narrow(d.wstring()) + ": " + ec.message());
  }
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
    return util::ToLower(util::Narrow(a.filename().wstring())) < util::ToLower(util::Narrow(b.filename().wstring()));
  });

  DefMap merged, base;
  json cheats = json::array();
  json overrides = json::object();
  std::string build;
  size_t parsed = 0;
  for (const auto& path : files) {
    const std::string fname = util::Narrow(path.filename().wstring());
    const bool isOverrides = util::IEquals(fname, util::Narrow(kOverridesFile));
    try {
      std::ifstream f(path, std::ios::binary);
      if (!f) { errors.push_back(fname + ": cannot open"); continue; }
      json doc = json::parse(f, nullptr, true, true);
      if (!doc.is_object()) { errors.push_back(fname + ": top level is not an object"); continue; }
      if (auto it = doc.find("schema"); it != doc.end() && !(it->is_number_integer() && it->get<int64_t>() == 1))
        errors.push_back(fname + ": unsupported schema " + it->dump() + " (expected 1); loading anyway");
      ++parsed;
      if (auto it = doc.find("game_build"); it != doc.end() && it->is_string() && !it->get<std::string>().empty()) build = it->get<std::string>();
      if (auto it = doc.find("symbols"); it != doc.end()) {
        if (!it->is_object()) {
          errors.push_back(fname + ": \"symbols\" is not an object");
        } else {
          for (const auto& [name, def] : it->items()) {
            Def parsedDef = ParseDef(name, def, fname);
            if (!parsedDef.loadError.empty()) errors.push_back(fname + ": " + name + ": " + parsedDef.loadError);
            if (isOverrides) overrides[name] = def;
            else base.insert_or_assign(name, parsedDef);
            merged.insert_or_assign(name, std::move(parsedDef));
          }
        }
      }
      if (auto it = doc.find("cheats"); it != doc.end()) {
        if (!it->is_array()) {
          errors.push_back(fname + ": \"cheats\" is not an array");
        } else {
          for (const json& c : *it) {
            bool replaced = false;
            if (c.is_object()) {
              if (auto id = c.find("id"); id != c.end() && !id->is_null()) {
                for (json& existing : cheats) {
                  if (existing.is_object() && existing.contains("id") && existing["id"] == *id) {
                    existing = c;
                    replaced = true;
                    break;
                  }
                }
              }
            }
            if (!replaced) cheats.push_back(c);
          }
        }
      }
    } catch (const json::exception& e) {
      errors.push_back(fname + ": " + e.what());
    } catch (const std::exception& e) {
      errors.push_back(fname + ": " + e.what());
    }
  }
  for (const auto& e : errors) log::Warn(kCh, "{}", e);

  // Resolve every symbol (sorted for deterministic logs / progress).
  std::vector<std::string> names;
  names.reserve(merged.size());
  for (const auto& [n, _] : merged) names.push_back(n);
  std::sort(names.begin(), names.end());
  progress.store(0);
  std::unordered_map<std::string, Result> memo;
  Resolver resolver(merged, memo, instances, &progress, names.size());
  Table table;
  size_t ok = 0, failed = 0;
  for (const auto& n : names) {
    const Result& r = resolver.Resolve(n);
    auto [it, _] = table.try_emplace(n);
    FillEntry(it->second, merged.at(n), r);
    if (r.ok) ++ok;
    else {
      ++failed;
      log::Warn(kCh, "{}: {}", n, r.error);
    }
  }
  progress.store(1);

  {
    std::unique_lock lk(mu);
    table.swap(this->table);
    this->cheats = std::move(cheats);
    this->gameBuild = std::move(build);
    this->loadErrors = std::move(errors);
    this->defs = std::move(merged);
    this->baseDefs = std::move(base);
    this->overrides = std::move(overrides);
    this->dir = d;
  }
  generation.fetch_add(1);
  log::Info(kCh, "loaded {} file(s) from {}: {} symbol(s) resolved, {} failed", parsed, util::Narrow(d.wstring()), ok, failed);
  return parsed > 0;
}

// Resolves a single symbol against the live table (dependencies are taken from it, not re-scanned)
// and inserts the result. `def` == nullptr removes the symbol. Caller holds opMu.
void Bindings::Impl::ResolveOne(const std::string& name, const Def* def) {
  if (!def) {
    std::unique_lock lk(mu);
    defs.erase(name);
    if (auto it = table.find(name); it != table.end()) table.erase(it);
    return;
  }
  DefMap defsCopy;
  std::unordered_map<std::string, Result> memo;
  {
    std::shared_lock lk(mu);
    defsCopy = defs;
    for (const auto& [n, e] : table)
      if (n != name) memo.emplace(n, ResultFromEntry(e));
  }
  defsCopy.insert_or_assign(name, *def);
  Resolver resolver(defsCopy, memo, instances, nullptr, 0);
  Result r;
  try {
    r = resolver.Resolve(name);
  } catch (const std::exception& e) {
    r.done = true;
    r.error = std::string("internal error: ") + e.what();
  }
  std::unique_lock lk(mu);
  defs.insert_or_assign(name, *def);
  auto [it, _] = table.try_emplace(name);
  FillEntry(it->second, *def, r);
}

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------
const char* SymbolKindName(SymbolKind k) {
  switch (k) {
    case SymbolKind::Address: return "address";
    case SymbolKind::Pointer: return "pointer";
    case SymbolKind::Function: return "function";
    case SymbolKind::Offset: return "offset";
    case SymbolKind::Field: return "field";
    case SymbolKind::Constant: return "constant";
  }
  return "?";
}

const char* SymbolStatusName(SymbolStatus s) {
  switch (s) {
    case SymbolStatus::Pending: return "pending";
    case SymbolStatus::Resolved: return "resolved";
    case SymbolStatus::Failed: return "failed";
    case SymbolStatus::Overridden: return "overridden";
  }
  return "?";
}

Bindings::Bindings() : impl_(std::make_unique<Impl>()) {}

Bindings& Bindings::Get() {
  static Bindings* instance = new Bindings();   // leaked on purpose: used during unload
  return *instance;
}

bool Bindings::LoadDirectory(const std::filesystem::path& dir) {
  try {
    std::lock_guard op(impl_->opMu);
    impl_->resolving.store(true);
    bool ok = false;
    try {
      ok = impl_->Load(dir);
    } catch (const std::exception& e) {
      log::Error(kCh, "load failed: {}", e.what());
    }
    impl_->resolving.store(false);
    return ok;
  } catch (...) {
    impl_->resolving.store(false);
    return false;
  }
}

bool Bindings::Reload() {
  std::filesystem::path d;
  try {
    std::shared_lock lk(impl_->mu);
    d = impl_->dir;
  } catch (...) {
    return false;
  }
  if (d.empty()) {
    log::Warn(kCh, "Reload: no directory loaded yet");
    return false;
  }
  return LoadDirectory(d);
}

bool Bindings::Resolving() const { return impl_->resolving.load(); }
float Bindings::Progress() const { return impl_->resolving.load() ? impl_->progress.load() : 1.0f; }
uint64_t Bindings::Generation() const { return impl_->generation.load(); }

bool Bindings::Has(std::string_view name) const {
  try {
    std::shared_lock lk(impl_->mu);
    auto it = impl_->table.find(name);
    return it != impl_->table.end() &&
           (it->second.info.status == SymbolStatus::Resolved || it->second.info.status == SymbolStatus::Overridden);
  } catch (...) {
    return false;
  }
}

std::optional<uintptr_t> Bindings::Addr(std::string_view name) const {
  try {
    std::shared_lock lk(impl_->mu);
    auto it = impl_->table.find(name);
    if (it == impl_->table.end()) return std::nullopt;
    const Entry& e = it->second;
    const SymbolInfo& i = e.info;
    if (i.status != SymbolStatus::Resolved && i.status != SymbolStatus::Overridden) return std::nullopt;
    if (i.kind != SymbolKind::Address && i.kind != SymbolKind::Pointer && i.kind != SymbolKind::Function) return std::nullopt;
    if (!i.dynamic) return i.staticValue;
    auto v = EvalTail(i.staticValue, e.tail);
    if (!v) return std::nullopt;
    if (!i.expectClass.empty()) {
      uintptr_t vt = 0;
      if (!mem::ReadRaw(*v, &vt, sizeof(vt))) return std::nullopt;
      if (e.validObj.load(std::memory_order_relaxed) != *v || e.validVt.load(std::memory_order_relaxed) != vt) {
        if (!mem::rtti::IsA(*v, i.expectClass)) return std::nullopt;
        e.validObj.store(*v, std::memory_order_relaxed);
        e.validVt.store(vt, std::memory_order_relaxed);
      }
    }
    return v;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<int64_t> Bindings::Offset(std::string_view name) const {
  try {
    std::shared_lock lk(impl_->mu);
    auto it = impl_->table.find(name);
    if (it == impl_->table.end()) return std::nullopt;
    const SymbolInfo& i = it->second.info;
    if (i.status != SymbolStatus::Resolved && i.status != SymbolStatus::Overridden) return std::nullopt;
    if (i.kind != SymbolKind::Offset && i.kind != SymbolKind::Constant) return std::nullopt;
    return it->second.value;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<uintptr_t> Bindings::Field(std::string_view name, uintptr_t object) const {
  try {
    std::shared_lock lk(impl_->mu);
    auto it = impl_->table.find(name);
    if (it == impl_->table.end()) return std::nullopt;
    const SymbolInfo& i = it->second.info;
    if (i.status != SymbolStatus::Resolved && i.status != SymbolStatus::Overridden) return std::nullopt;
    if (i.kind != SymbolKind::Field || !mem::IsPlausiblePtr(object)) return std::nullopt;
    return mem::Deref(object, std::span<const int64_t>(it->second.chain));
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<SymbolInfo> Bindings::Info(std::string_view name) const {
  try {
    std::shared_lock lk(impl_->mu);
    auto it = impl_->table.find(name);
    if (it == impl_->table.end()) return std::nullopt;
    return it->second.info;
  } catch (...) {
    return std::nullopt;
  }
}

std::vector<SymbolInfo> Bindings::List() const {
  std::vector<SymbolInfo> out;
  try {
    std::shared_lock lk(impl_->mu);
    out.reserve(impl_->table.size());
    for (const auto& [_, e] : impl_->table) out.push_back(e.info);
  } catch (...) {
    return out;
  }
  std::sort(out.begin(), out.end(), [](const SymbolInfo& a, const SymbolInfo& b) { return a.name < b.name; });
  return out;
}

nlohmann::json Bindings::Cheats() const {
  try {
    std::shared_lock lk(impl_->mu);
    return impl_->cheats;
  } catch (...) {
    return json::array();
  }
}

std::string Bindings::GameBuild() const {
  try {
    std::shared_lock lk(impl_->mu);
    return impl_->gameBuild;
  } catch (...) {
    return {};
  }
}

std::vector<std::string> Bindings::LoadErrors() const {
  try {
    std::shared_lock lk(impl_->mu);
    return impl_->loadErrors;
  } catch (...) {
    return {};
  }
}

void Bindings::Pin(const std::string& name, SymbolKind kind, uintptr_t value) {
  try {
    std::lock_guard op(impl_->opMu);
    json def = json::object();
    def["kind"] = SymbolKindName(kind);
    def["pinned"] = true;
    def["verified"] = true;
    switch (kind) {
      case SymbolKind::Offset:
      case SymbolKind::Constant:
        def["value"] = HexJson(value);
        break;
      case SymbolKind::Field:
        def["chain"] = json::array({HexJson(value)});
        break;
      default: {
        json steps = json::array();
        const mem::Module& main = mem::Module::Main();
        if (main.Valid() && main.Contains(value)) {
          steps.push_back(json::object({{"module", ""}}));
          steps.push_back(json::object({{"add", HexJson(main.Rva(value))}}));
        } else if (auto m = mem::Module::Containing(value)) {
          steps.push_back(json::object({{"module", m->name}}));
          steps.push_back(json::object({{"add", HexJson(m->Rva(value))}}));
        } else {
          steps.push_back(json::object({{"add", HexJson(value)}}));
        }
        def["steps"] = std::move(steps);
        def["notes"] = "pinned at " + mem::Describe(value);
        break;
      }
    }
    std::filesystem::path d;
    json symbols;
    {
      std::unique_lock lk(impl_->mu);
      // Keep the descriptive fields of the original definition (type / expect_class) for the UI.
      if (auto it = impl_->defs.find(name); it != impl_->defs.end()) {
        if (!it->second.type.empty()) def["type"] = it->second.type;
      }
      impl_->overrides[name] = def;
      symbols = impl_->overrides;
      d = impl_->dir;
    }
    impl_->PersistOverrides(d, symbols);
    Def parsed = ParseDef(name, def, util::Narrow(kOverridesFile));
    impl_->ResolveOne(name, &parsed);
    impl_->generation.fetch_add(1);
    log::Info(kCh, "pinned {} ({}) = {}", name, SymbolKindName(kind), util::Hex(value));
  } catch (const std::exception& e) {
    log::Error(kCh, "Pin({}) failed: {}", name, e.what());
  }
}

void Bindings::Unpin(const std::string& name) {
  try {
    std::lock_guard op(impl_->opMu);
    std::filesystem::path d;
    json symbols;
    std::optional<Def> base;
    {
      std::unique_lock lk(impl_->mu);
      if (!impl_->overrides.contains(name)) return;
      impl_->overrides.erase(name);
      symbols = impl_->overrides;
      d = impl_->dir;
      if (auto it = impl_->baseDefs.find(name); it != impl_->baseDefs.end()) base = it->second;
    }
    impl_->PersistOverrides(d, symbols);
    impl_->ResolveOne(name, base ? &*base : nullptr);
    impl_->generation.fetch_add(1);
    log::Info(kCh, "unpinned {}", name);
  } catch (const std::exception& e) {
    log::Error(kCh, "Unpin({}) failed: {}", name, e.what());
  }
}

bool Bindings::Define(const std::string& name, const nlohmann::json& definition, std::string* error) {
  try {
    if (name.empty()) {
      if (error) *error = "empty symbol name";
      return false;
    }
    Def parsed = ParseDef(name, definition, util::Narrow(kOverridesFile));
    if (!parsed.loadError.empty()) {
      if (error) *error = parsed.loadError;
      return false;
    }
    std::lock_guard op(impl_->opMu);
    std::filesystem::path d;
    json symbols;
    {
      std::unique_lock lk(impl_->mu);
      impl_->overrides[name] = definition;
      symbols = impl_->overrides;
      d = impl_->dir;
    }
    const bool persisted = impl_->PersistOverrides(d, symbols);
    impl_->ResolveOne(name, &parsed);
    impl_->generation.fetch_add(1);
    auto info = Info(name);
    const bool ok = info && (info->status == SymbolStatus::Resolved || info->status == SymbolStatus::Overridden);
    if (!ok) {
      if (error) *error = info ? info->error : "resolution failed";
      log::Warn(kCh, "defined {} but it failed to resolve: {}", name, info ? info->error : std::string("?"));
      return false;
    }
    if (!persisted && error) *error = "resolved, but the overrides file could not be written";
    log::Info(kCh, "defined {} ({})", name, SymbolKindName(parsed.kind));
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    log::Error(kCh, "Define({}) failed: {}", name, e.what());
    return false;
  }
}

}  // namespace cg::game
