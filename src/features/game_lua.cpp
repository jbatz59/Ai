#include "features/game_lua.h"

#include <atomic>
#include <charconv>
#include <cmath>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include "core/log.h"
#include "ui/notify.h"

namespace cg::features {
namespace {

constexpr std::string_view kChannel = "features";
constexpr size_t kMaxToastError = 400;
constexpr size_t kMaxChunkName = 48;

// Lua 5.1 subset only (HKS): no goto, no integer division, no bit operators, no table.unpack reliance.
// Kept free of `--` comments so the source can be inspected/minified safely.
constexpr std::string_view kPrelude = R"lua(local G = _G
local pcall, type, tostring, select, error, rawset = pcall, type, tostring, select, error, rawset
local CG = {}
CG.__v = 1

local function pack(...)
  return { n = select("#", ...), ... }
end

local function unpackn(t, i, n)
  if i > n then return end
  return t[i], unpackn(t, i + 1, n)
end

local function str(e)
  if type(e) == "string" then return e end
  local ok, s = pcall(tostring, e)
  if ok and type(s) == "string" then return s end
  return "(unprintable error)"
end

local function index(obj, key)
  local ok, v = pcall(function() return obj[key] end)
  if ok then return v end
  return nil
end

function CG.get(...)
  local cur = G
  for i = 1, select("#", ...) do
    if cur == nil then return nil end
    cur = index(cur, (select(i, ...)))
  end
  return cur
end

function CG.has(obj, name)
  return obj ~= nil and index(obj, name) ~= nil
end

function CG.try(f, ...)
  local r = pack(pcall(f, ...))
  if not r[1] then return false, str(r[2]) end
  return unpackn(r, 1, r.n)
end

function CG.call(obj, method, ...)
  local name = str(method)
  if obj == nil then return false, "cannot call " .. name .. ": object is nil" end
  local fn = index(obj, method)
  if fn == nil then return false, name .. " not available on this build" end
  local r = pack(pcall(fn, obj, ...))
  if not r[1] then return false, name .. ": " .. str(r[2]) end
  return unpackn(r, 1, r.n)
end

function CG.must(ok, ...)
  if not ok then
    local e = ...
    if e == nil then e = "operation failed" end
    error(str(e), 0)
  end
  return ...
end

function CG.need(v, err)
  if v == nil then
    if err == nil then err = "required value is missing" end
    error(str(err), 0)
  end
  return v
end

function CG.player()
  local g = CG.get("game", "game")
  if g == nil then return nil, "game.game is not available" end
  local ok, p = CG.call(g, "GetActivePlayer")
  if not ok then return nil, p end
  if p == nil then return nil, "no active player (still loading?)" end
  return p
end

function CG.vehicle()
  local p, e = CG.player()
  if p == nil then return nil, e end
  local ok, v = CG.call(p, "GetOwner")
  if not ok then return nil, v end
  if v == nil then return nil, "the player is not in a vehicle" end
  return v
end

function CG.target()
  local p, e = CG.player()
  if p == nil then return nil, e end
  local ok, v = CG.call(p, "GetOwner")
  if ok and v ~= nil then return v, true end
  return p, false
end

function CG.vec(x, y, z)
  local M = CG.get("Math")
  if M == nil then return nil, "Math is not available" end
  local ok, v = CG.call(M, "newVector", x, y, z)
  if not ok then return nil, v end
  if v == nil then return nil, "Math:newVector returned nil" end
  return v
end

rawset(G, "CG", CG)
)lua";

// vm::Generation() + 1 the prelude was queued for; 0 = not queued (or its run failed).
std::atomic<uint64_t> g_preludeFor{0};

struct Job {
  std::string code;
  std::string what;
  std::string chunkName;
  game::vm::Callback done;
  bool toast = true;
};

std::string ChunkName(std::string_view what) {
  std::string name = "=";
  for (char c : what) {
    if (name.size() > kMaxChunkName) break;
    const auto u = static_cast<unsigned char>(c);
    name.push_back(u >= 0x20 && u < 0x7F ? c : '?');
  }
  if (name.size() == 1) name += "feature";
  return name;
}

void Report(const Job& job, const std::string& error) {
  try {
    const std::string& what = job.what.empty() ? std::string("Game script") : job.what;
    if (!job.toast) {
      log::Debug(kChannel, "{} failed: {}", what, error);
      return;
    }
    log::Warn(kChannel, "{} failed: {}", what, error);
    std::string text = error.empty() ? std::string("unknown error") : error;
    if (text.size() > kMaxToastError) {
      text.resize(kMaxToastError);
      text += "...";
    }
    ui::notify::Push(ui::notify::Kind::Error, what + " failed", std::move(text), 6.0f);
  } catch (...) {
  }
}

void Attempt(std::shared_ptr<const Job> job, bool retried) {
  EnsurePrelude();
  std::string chunk = GuardedChunk(job->code);
  std::string name = job->chunkName;
  game::vm::Run(
      std::move(chunk),
      [job, retried](const game::vm::Result& r) {
        try {
          if (!r.ok && !retried && r.error.find(kCgPreludeMissingMarker) != std::string::npos) {
            // The VM was reset between the prelude and the chunk: install again and retry once.
            g_preludeFor.store(0);
            Attempt(job, true);
            return;
          }
          if (!r.ok) Report(*job, r.error);
          if (job->done) job->done(r);
        } catch (const std::exception& e) {
          log::Error(kChannel, "{}: completion handler threw: {}", job->what, e.what());
        } catch (...) {
          log::Error(kChannel, "{}: completion handler threw", job->what);
        }
      },
      std::move(name));
}

}  // namespace

std::string LuaQuote(std::string_view s) {
  static constexpr char kDigits[] = "0123456789";
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '\\' || c == '"' || c == '\'') {
      out.push_back('\\');
      out.push_back(c);
    } else if (u >= 0x20 && u < 0x7F) {
      out.push_back(c);
    } else {
      // Always three digits: "\0" followed by a literal digit would otherwise merge into one escape.
      out.push_back('\\');
      out.push_back(kDigits[u / 100]);
      out.push_back(kDigits[(u / 10) % 10]);
      out.push_back(kDigits[u % 10]);
    }
  }
  out.push_back('"');
  return out;
}

std::string LuaNumber(double v) {
  if (!std::isfinite(v)) return "nil";
  char buf[64];
  const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
  if (ec != std::errc()) return "nil";
  std::string s(buf, end);
  // "a-" .. "-1" would start a comment; a parenthesised negative is safe in every expression slot.
  if (std::signbit(v)) return "(" + s + ")";
  return s;
}

std::string_view CgPrelude() { return kPrelude; }

std::string GuardedChunk(std::string_view code) {
  std::string out = std::format(
      "do local c = rawget(_G, \"CG\") if type(c) ~= \"table\" or c.__v ~= {} then error(\"{}\", 0) end end ",
      kCgPreludeVersion, kCgPreludeMissingMarker);
  out.append(code);
  return out;
}

void EnsurePrelude() {
  if (!game::vm::Ready()) return;   // a chunk queued now fails with "not ready" anyway
  const uint64_t gen = game::vm::Generation();
  if (g_preludeFor.load() == gen + 1) return;
  g_preludeFor.store(gen + 1);
  try {
    game::vm::Run(
        std::string(kPrelude),
        [gen](const game::vm::Result& r) {
          if (r.ok) return;
          uint64_t expected = gen + 1;
          g_preludeFor.compare_exchange_strong(expected, 0);
          try {
            log::Error(kChannel, "installing the CG prelude into the game VM failed: {}", r.error);
          } catch (...) {
          }
        },
        "=consigliere_prelude");
  } catch (...) {
    g_preludeFor.store(0);
  }
}

void RunFeatureChunk(std::string code, std::string what, game::vm::Callback done, bool toastOnError) {
  try {
    auto job = std::make_shared<Job>();
    job->chunkName = ChunkName(what);
    job->code = std::move(code);
    job->what = std::move(what);
    job->done = std::move(done);
    job->toast = toastOnError;
    Attempt(std::move(job), false);
  } catch (const std::exception& e) {
    log::Error(kChannel, "RunFeatureChunk: {}", e.what());
  } catch (...) {
    log::Error(kChannel, "RunFeatureChunk failed");
  }
}

}  // namespace cg::features
