#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <windows.h>

#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/hotkeys.h"
#include "core/log.h"
#include "core/mp_guard.h"
#include "core/tasks.h"
#include "core/util.h"
#include "testing.h"

namespace {

using namespace cg;

std::filesystem::path MakeTempDir(const char* tag) {
  wchar_t buf[MAX_PATH + 1];
  const DWORD n = GetTempPathW(MAX_PATH + 1, buf);
  std::filesystem::path base = n ? std::filesystem::path(std::wstring(buf, n)) : std::filesystem::current_path();
  base /= L"cg_test_" + util::Widen(tag) + L"_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
          std::to_wstring(GetTickCount64());
  std::error_code ec;
  std::filesystem::create_directories(base, ec);
  return base;
}

std::string ReadText(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void WriteText(const std::filesystem::path& p, const std::string& s) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << s;
}

bool WaitFor(const std::function<bool()>& cond, int timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return cond();
}

}  // namespace

// --- util --------------------------------------------------------------------------------------

CG_TEST(core_util_parse_uint) {
  CHECK_EQ(util::ParseUInt("26").value_or(0), 26u);
  CHECK_EQ(util::ParseUInt("0x1A").value_or(0), 26u);
  CHECK_EQ(util::ParseUInt("0X1a").value_or(0), 26u);
  CHECK_EQ(util::ParseUInt("1A", true).value_or(0), 26u);
  CHECK_EQ(util::ParseUInt("0x1A", true).value_or(0), 26u);
  CHECK_EQ(util::ParseUInt("  42 \t").value_or(0), 42u);
  CHECK(!util::ParseUInt("1A").has_value());
  CHECK(!util::ParseUInt("1Ah", true).has_value());
  CHECK(!util::ParseUInt("26 x").has_value());
  CHECK(!util::ParseUInt("").has_value());
  CHECK(!util::ParseUInt("0x").has_value());
  CHECK(!util::ParseUInt("-1").has_value());
  CHECK_EQ(util::ParseUInt("18446744073709551615").value_or(0), std::numeric_limits<uint64_t>::max());
  CHECK(!util::ParseUInt("18446744073709551616").has_value());
  CHECK_EQ(util::ParseUInt("0xFFFFFFFFFFFFFFFF").value_or(0), std::numeric_limits<uint64_t>::max());
  CHECK(!util::ParseUInt("0x10000000000000000").has_value());
  CHECK_EQ(util::ParseUInt("7ff6`12345678", true).value_or(0), 0x7ff612345678ull);
  CHECK(!util::ParseUInt("12``34").has_value());
  CHECK(!util::ParseUInt("`1234").has_value());
}

CG_TEST(core_util_parse_int) {
  CHECK_EQ(util::ParseInt("-5").value_or(0), -5);
  CHECK_EQ(util::ParseInt("-0x10").value_or(0), -16);
  CHECK_EQ(util::ParseInt("-ff", true).value_or(0), -255);
  CHECK_EQ(util::ParseInt("9223372036854775807").value_or(0), std::numeric_limits<int64_t>::max());
  CHECK_EQ(util::ParseInt("-9223372036854775808").value_or(0), std::numeric_limits<int64_t>::min());
  CHECK(!util::ParseInt("9223372036854775808").has_value());
  CHECK(!util::ParseInt("-9223372036854775809").has_value());
  CHECK(!util::ParseInt("--5").has_value());
  CHECK(!util::ParseInt("-").has_value());
  CHECK(!util::ParseInt("5-").has_value());
  CHECK(!util::ParseInt("+5").has_value());
}

CG_TEST(core_util_parse_double) {
  CHECK_EQ(util::ParseDouble("1.5").value_or(0), 1.5);
  CHECK_EQ(util::ParseDouble(" -2e3 ").value_or(0), -2000.0);
  CHECK_EQ(util::ParseDouble("+0.25").value_or(0), 0.25);
  CHECK_EQ(util::ParseDouble("0x10").value_or(0), 16.0);
  CHECK(!util::ParseDouble("abc").has_value());
  CHECK(!util::ParseDouble("1.5x").has_value());
  CHECK(!util::ParseDouble("").has_value());
  CHECK(!util::ParseDouble("nan").has_value());
  CHECK(!util::ParseDouble("inf").has_value());
  CHECK(!util::ParseDouble("1e999").has_value());
}

CG_TEST(core_util_hex_and_sizes) {
  CHECK_EQ(util::Hex(0x1f), std::string("0x1F"));
  CHECK_EQ(util::Hex(0x1f, 4), std::string("0x001F"));
  CHECK_EQ(util::Hex(255, 0, false), std::string("FF"));
  CHECK_EQ(util::Hex(0), std::string("0x0"));
  CHECK_EQ(util::Hex(0xFFFFFFFFFFFFFFFFull), std::string("0xFFFFFFFFFFFFFFFF"));
  const uint8_t bytes[] = {0x48, 0x8B, 0x05};
  CHECK_EQ(util::HexBytes(bytes, 3), std::string("48 8B 05"));
  CHECK_EQ(util::HexBytes(bytes, 3, '\0'), std::string("488B05"));
  CHECK_EQ(util::HexBytes(bytes, 0), std::string());
  CHECK_EQ(util::FormatBytesSize(512), std::string("512 B"));
  CHECK_EQ(util::FormatBytesSize(1536), std::string("1.5 KB"));
  CHECK_EQ(util::FormatBytesSize(12897485), std::string("12.3 MB"));
  CHECK_EQ(util::FormatBytesSize(1048575), std::string("1.0 MB"));
}

CG_TEST(core_util_strings) {
  const auto parts = util::Split("a,,b,", ',');
  REQUIRE(parts.size() == 2);
  CHECK_EQ(parts[0], std::string("a"));
  CHECK_EQ(parts[1], std::string("b"));
  const auto all = util::Split("a,,b,", ',', false);
  REQUIRE(all.size() == 4);
  CHECK_EQ(all[1], std::string());
  CHECK_EQ(all[3], std::string());
  CHECK(util::Split("", ',').empty());
  CHECK_EQ(util::Trim("  x y \r\n"), std::string("x y"));
  CHECK_EQ(util::ToLower("MiXeD 123"), std::string("mixed 123"));
  CHECK(util::IEquals("Insert", "INSERT"));
  CHECK(!util::IEquals("Insert", "Inser"));
  CHECK(util::IContains("MafiaDefinitiveEdition.exe", "definitive"));
  CHECK(!util::IContains("abc", "abcd"));
  CHECK(util::StartsWith("features.player", "features."));
  CHECK(!util::StartsWith("feat", "features"));
  const std::string utf8 = "Lost Heaven \xC5\xBDi\xC5\xBEkov \xE2\x9C\x93";
  const std::wstring wide = util::Widen(utf8);
  CHECK_EQ(wide.size(), size_t(20));
  CHECK_EQ(util::Narrow(wide), utf8);
  CHECK(util::Widen("").empty());
  CHECK(util::Narrow(L"").empty());
}

CG_TEST(core_util_fuzzy_ordering) {
  CHECK(util::FuzzyScore("xyz", "God Mode") < 0);
  CHECK(util::FuzzyScore("godmodex", "God Mode") < 0);
  CHECK(util::FuzzyScore("", "anything") >= 0);
  CHECK(util::FuzzyScore("god", "God Mode") >= 0);
  CHECK(util::FuzzyScore("GOD", "god mode") >= 0);
  // Consecutive word-start match beats scattered letters.
  CHECK(util::FuzzyScore("god", "God Mode") > util::FuzzyScore("god", "Ground Defense"));
  // Word-start initials beat mid-word letters.
  CHECK(util::FuzzyScore("sv", "Spawn Vehicle") > util::FuzzyScore("sv", "Save"));
  // camelCase boundaries count as word starts.
  CHECK(util::FuzzyScore("gm", "godMode") > util::FuzzyScore("gm", "gamma"));
  // Earlier match beats a later one; a shorter text beats a longer one with the same match.
  CHECK(util::FuzzyScore("spawn", "Spawn Vehicle") > util::FuzzyScore("spawn", "Vehicle Spawner"));
  CHECK(util::FuzzyScore("tele", "Teleport") > util::FuzzyScore("tele", "Teleport to waypoint marker"));
  // Separators '.', '_', '-' start words.
  CHECK(util::FuzzyScore("pg", "player.god") > util::FuzzyScore("pg", "plague"));
  CHECK(util::FuzzyScore("ig", "inf_gun") > util::FuzzyScore("ig", "icing"));
}

// --- hotkeys -------------------------------------------------------------------------------------

CG_TEST(core_hotkey_to_string_and_parse) {
  CHECK_EQ((Hotkey{VK_F5, true, true, false}.ToString()), std::string("Ctrl+Shift+F5"));
  CHECK_EQ((Hotkey{VK_INSERT}.ToString()), std::string("Insert"));
  CHECK_EQ(Hotkey{}.ToString(), std::string("Unbound"));
  CHECK((Hotkey::Parse("Ctrl+Shift+F5") == Hotkey{VK_F5, true, true, false}));
  CHECK((Hotkey::Parse("  shift+ctrl+f5 ") == Hotkey{VK_F5, true, true, false}));
  CHECK((Hotkey::Parse("Alt+-") == Hotkey{VK_OEM_MINUS, false, false, true}));
  CHECK((Hotkey::Parse("Ctrl+Num +") == Hotkey{VK_ADD, true, false, false}));
  CHECK((Hotkey::Parse("Mouse4") == Hotkey{VK_XBUTTON1}));
  CHECK((Hotkey::Parse("0x07") == Hotkey{0x07}));
  CHECK(!Hotkey::Parse("Unbound").Valid());
  CHECK(!Hotkey::Parse("").Valid());
  CHECK(!Hotkey::Parse("Ctrl+").Valid());
  CHECK(!Hotkey::Parse("Ctrl+Banana").Valid());
  CHECK(!Hotkey::Parse("Ctrl+Banana").ctrl);
  CHECK(!Hotkey::Parse("0x100").Valid());
  CHECK_EQ(std::string(hotkeys::VkName(VK_NUMPAD7)), std::string("Num 7"));
  CHECK_EQ(std::string(hotkeys::VkName(VK_XBUTTON2)), std::string("Mouse5"));
  CHECK_EQ(std::string(hotkeys::VkName(VK_F24)), std::string("F24"));
  CHECK_EQ(std::string(hotkeys::VkName(VK_NEXT)), std::string("PageDown"));
  CHECK_EQ(std::string(hotkeys::VkName(VK_OEM_3)), std::string("`"));
}

CG_TEST(core_hotkey_round_trip_all_keys) {
  int failures = 0;
  for (int vk = 1; vk <= 254; ++vk) {
    for (int mods = 0; mods < 8; ++mods) {
      const Hotkey hk{static_cast<uint16_t>(vk), (mods & 1) != 0, (mods & 2) != 0, (mods & 4) != 0};
      const Hotkey back = Hotkey::Parse(hk.ToString());
      if (!(back == hk)) {
        if (++failures <= 5) CHECK_EQ(hk.ToString(), back.ToString());
      }
    }
  }
  CHECK_EQ(failures, 0);
}

CG_TEST(core_hotkey_capture_state) {
  CHECK(!hotkeys::Capturing());
  hotkeys::BeginCapture();
  CHECK(hotkeys::Capturing());
  CHECK(!hotkeys::Pressed(Hotkey{VK_F5}));
  Hotkey hk{VK_F6};
  CHECK(!hotkeys::PollCapture(hk));   // no key went down
  CHECK((hk == Hotkey{VK_F6}));
  hotkeys::CancelCapture();
  CHECK(!hotkeys::Capturing());
  CHECK(!hotkeys::Pressed(Hotkey{}));
  CHECK(!hotkeys::IsDown(0));
  CHECK(!hotkeys::IsDown(300));
}

// --- config --------------------------------------------------------------------------------------

CG_TEST(core_config_dotted_keys_and_defaults) {
  const auto dir = MakeTempDir("config_keys");
  Config& cfg = Config::Get();
  REQUIRE(cfg.Load(dir / L"config.json"));   // missing file => empty store

  CHECK_EQ(cfg.ReadInt("a.b", 7), int64_t(7));
  cfg.WriteInt("a.b.c", 5);
  CHECK_EQ(cfg.ReadInt("a.b.c", 0), int64_t(5));
  const auto sub = cfg.ReadJson("a.b");
  CHECK(sub.is_object());
  CHECK(sub.contains("c"));
  CHECK(cfg.ReadJson("a.nope").is_null());

  // Type mismatches return the default.
  CHECK_EQ(cfg.ReadBool("a.b.c", true), true);
  CHECK_EQ(cfg.ReadString("a.b.c", "x"), std::string("x"));
  CHECK_EQ(cfg.ReadInt("a.b", 3), int64_t(3));
  CHECK_EQ(cfg.ReadDouble("a.b.c", 0.0), 5.0);   // integers are valid doubles

  cfg.WriteDouble("ui.scale", 1.25);
  CHECK_EQ(cfg.ReadFloat("ui.scale", 1.0f), 1.25f);
  CHECK_EQ(cfg.ReadInt("ui.scale", 9), int64_t(9));   // non-integral: not an int
  cfg.WriteBool("features.player.god.enabled", true);
  CHECK(cfg.ReadBool("features.player.god.enabled", false));
  cfg.WriteString("ui.theme", "Noir");
  CHECK_EQ(cfg.ReadString("ui.theme", ""), std::string("Noir"));

  // Writing through a scalar replaces it with an object.
  cfg.WriteInt("x", 1);
  cfg.WriteInt("x.y", 2);
  CHECK_EQ(cfg.ReadInt("x.y", 0), int64_t(2));

  // Invalid keys never match and never write.
  CHECK_EQ(cfg.ReadInt("", 9), int64_t(9));
  cfg.WriteInt("bad..key", 1);
  CHECK_EQ(cfg.ReadInt("bad..key", 4), int64_t(4));
  CHECK(cfg.ReadJson("bad").is_null());

  cfg.Erase("a.b.c");
  CHECK_EQ(cfg.ReadInt("a.b.c", -1), int64_t(-1));
  cfg.Erase("does.not.exist");

  cfg.Load(std::filesystem::path());   // detach the singleton from the temp folder
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

CG_TEST(core_config_save_load_round_trip) {
  const auto dir = MakeTempDir("config_save");
  const auto file = dir / L"config.json";
  Config& cfg = Config::Get();
  REQUIRE(cfg.Load(file));
  cfg.WriteInt("limits.min", std::numeric_limits<int64_t>::min());
  cfg.WriteInt("limits.max", std::numeric_limits<int64_t>::max());
  cfg.WriteDouble("ui.scale", 1.5);
  cfg.WriteString("ui.name", "Tommy \xE2\x9C\x93");
  cfg.WriteBool("hud.visible", false);
  cfg.WriteJson("list", nlohmann::json::array({1, 2, 3}));
  REQUIRE(cfg.Save());
  CHECK(std::filesystem::exists(file));
  CHECK(!std::filesystem::exists(dir / L"config.json.tmp"));

  const std::string text = ReadText(file);
  CHECK(text.find("\n  \"hud\": {") != std::string::npos);   // pretty-printed, indent 2
  const auto parsed = nlohmann::json::parse(text, nullptr, false);
  CHECK(parsed.is_object());

  cfg.WriteInt("limits.min", 0);   // diverge in memory, then reload from disk
  REQUIRE(cfg.Load(file));
  CHECK_EQ(cfg.ReadInt("limits.min", 0), std::numeric_limits<int64_t>::min());
  CHECK_EQ(cfg.ReadInt("limits.max", 0), std::numeric_limits<int64_t>::max());
  CHECK_EQ(cfg.ReadDouble("ui.scale", 0), 1.5);
  CHECK_EQ(cfg.ReadString("ui.name", ""), std::string("Tommy \xE2\x9C\x93"));
  CHECK_EQ(cfg.ReadBool("hud.visible", true), false);
  CHECK_EQ(cfg.ReadJson("list").size(), size_t(3));

  // SaveIfDirty persists pending writes.
  cfg.WriteInt("later", 42);
  cfg.SaveIfDirty();
  CHECK(ReadText(file).find("\"later\": 42") != std::string::npos);

  cfg.Load(std::filesystem::path());
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

CG_TEST(core_config_corrupt_file_is_backed_up) {
  const auto dir = MakeTempDir("config_corrupt");
  const auto file = dir / L"config.json";
  const std::string garbage = "{ \"ui\": { \"scale\": 1.5, ";
  WriteText(file, garbage);
  Config& cfg = Config::Get();
  CHECK(!cfg.Load(file));
  CHECK_EQ(ReadText(dir / L"config.json.bak"), garbage);
  CHECK(cfg.ReadJson("ui").is_null());
  CHECK_EQ(cfg.ReadDouble("ui.scale", 2.0), 2.0);

  // A JSON document that is not an object is not a valid store either.
  WriteText(file, "[1, 2, 3]");
  CHECK(!cfg.Load(file));

  // Comments and a UTF-8 BOM (Notepad) are tolerated.
  WriteText(file, "\xEF\xBB\xBF// hand edited\n{ \"a\": { \"b\": 3 } }");
  CHECK(cfg.Load(file));
  CHECK_EQ(cfg.ReadInt("a.b", 0), int64_t(3));

  cfg.Load(std::filesystem::path());
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// --- tasks ---------------------------------------------------------------------------------------

CG_TEST(core_tasks_render_queue_is_fifo) {
  tasks::DrainRender();   // start from an empty queue
  std::vector<int> order;
  for (int i = 0; i < 100; ++i) tasks::PostRender([&order, i] { order.push_back(i); });
  tasks::PostRender([] { throw std::runtime_error("expected test exception"); });
  tasks::PostRender([&order] {
    order.push_back(100);
    tasks::PostRender([&order] { order.push_back(101); });   // runs on the next drain
  });
  tasks::DrainRender();
  REQUIRE(order.size() == 101);
  bool fifo = true;
  for (int i = 0; i <= 100; ++i) fifo = fifo && order[static_cast<size_t>(i)] == i;
  CHECK(fifo);
  tasks::DrainRender();
  CHECK_EQ(order.size(), size_t(102));
  CHECK_EQ(order.back(), 101);
  tasks::PostRender(tasks::Fn());   // empty functions are ignored
  tasks::DrainRender();
}

CG_TEST(core_tasks_game_queue_and_flag) {
  tasks::DrainGame();
  int ran = 0;
  tasks::PostGame([&ran] { ++ran; });
  tasks::PostGame([&ran] { ran *= 10; });
  tasks::DrainGame();
  CHECK_EQ(ran, 10);
  const bool before = tasks::GameThreadHookActive();
  tasks::SetGameThreadHookActive(true);
  CHECK(tasks::GameThreadHookActive());
  tasks::SetGameThreadHookActive(false);
  CHECK(!tasks::GameThreadHookActive());
  tasks::SetGameThreadHookActive(before);
}

CG_TEST(core_tasks_run_async_executes) {
  std::atomic<int> count{0};
  std::atomic<bool> offThread{true};
  const auto mainId = std::this_thread::get_id();
  for (int i = 0; i < 32; ++i)
    tasks::RunAsync([&count, &offThread, mainId] {
      if (std::this_thread::get_id() == mainId) offThread = false;
      count.fetch_add(1);
    });
  tasks::RunAsync([] { throw std::runtime_error("expected async test exception"); });
  std::atomic<bool> afterThrow{false};
  tasks::RunAsync([&afterThrow] { afterThrow = true; });
  CHECK(WaitFor([&] { return count.load() == 32 && afterThrow.load(); }, 10000));
  CHECK(offThread.load());
}

// --- log -----------------------------------------------------------------------------------------

CG_TEST(core_log_copy_and_generation) {
  const log::Level oldLevel = log::MinLevel();
  log::SetMinLevel(log::Level::Trace);

  const uint64_t g0 = log::Generation();
  log::Info("test", "hello {}", 42);
  CHECK(log::Generation() > g0);
  auto entries = log::Copy();
  REQUIRE(!entries.empty());
  CHECK_EQ(entries.back().text, std::string("hello 42"));
  CHECK_EQ(entries.back().channel, std::string("test"));
  CHECK(entries.back().level == log::Level::Info);
  CHECK(entries.back().timeMs <= util::NowMs());

  log::SetMinLevel(log::Level::Warn);
  CHECK(log::MinLevel() == log::Level::Warn);
  const uint64_t g1 = log::Generation();
  log::Debug("test", "filtered");
  CHECK_EQ(log::Generation(), g1);
  log::SetMinLevel(log::Level::Trace);

  log::Clear();
  CHECK(log::Copy().empty());
  CHECK(log::Generation() > g1);

  for (int i = 0; i < 5000; ++i) log::Trace("test", "line {}", i);
  entries = log::Copy();
  CHECK_EQ(entries.size(), size_t(4096));
  if (!entries.empty()) {
    CHECK_EQ(entries.front().text, std::string("line 904"));
    CHECK_EQ(entries.back().text, std::string("line 4999"));
  }

  log::Warn("test", "{}", std::string(20000, 'x'));
  entries = log::Copy();
  CHECK(!entries.empty() && entries.back().text.size() <= 8192 + 3);

  CHECK_EQ(std::string(log::LevelName(log::Level::Error)), std::string("ERROR"));
  CHECK_EQ(std::string(log::LevelName(log::Level::Warn)), std::string("WARN"));
  log::Clear();
  log::SetMinLevel(oldLevel);
}

CG_TEST(core_log_file_sink) {
  const auto dir = MakeTempDir("log");
  const auto file = dir / L"consigliere.log";
  const log::Level oldLevel = log::MinLevel();
  log::SetMinLevel(log::Level::Info);
  log::Info("test", "before init");
  log::Init(file);
  log::Error("core", "boom {}", 7);   // Warn/Error flush immediately
  const std::string text = ReadText(file);
  CHECK(text.find("] [INFO] [test] before init") != std::string::npos);
  const size_t pos = text.find("] [ERROR] [core] boom 7");
  CHECK(pos != std::string::npos);
  // "[HH:MM:SS.mmm] " prefix.
  const size_t lineStart = text.rfind('\n', pos) == std::string::npos ? 0 : text.rfind('\n', pos) + 1;
  CHECK_EQ(pos - lineStart, size_t(13));
  CHECK_EQ(text[lineStart], '[');
  CHECK_EQ(text[lineStart + 3], ':');
  CHECK_EQ(text[lineStart + 9], '.');
  log::Shutdown();
  log::SetMinLevel(oldLevel);
  log::Clear();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// --- mp_guard ------------------------------------------------------------------------------------

CG_TEST(core_mp_guard_clean_process) {
  mp_guard::Refresh();
  CHECK(!mp_guard::Blocked());
  CHECK(mp_guard::Reason().empty());
}
