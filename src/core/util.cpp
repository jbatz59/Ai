#include "core/util.h"

#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <format>
#include <limits>

#include <windows.h>

namespace cg::util {
namespace {

constexpr char ToLowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr bool IsUpperAscii(char c) { return c >= 'A' && c <= 'Z'; }
constexpr bool IsLowerAscii(char c) { return c >= 'a' && c <= 'z'; }
constexpr bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

int DigitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Digit separators are accepted between two digits so values copied from WinDbg ("7ff6`12345678")
// or written C++-style ("1'000'000") parse.
bool IsDigitSeparator(char c) { return c == '`' || c == '\''; }

// Parses an unsigned magnitude (no sign, no surrounding whitespace). "0x" forces hex.
std::optional<uint64_t> ParseMagnitude(std::string_view s, bool defaultHex) {
  bool hex = defaultHex;
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    hex = true;
    s.remove_prefix(2);
  }
  if (s.empty()) return std::nullopt;
  const uint64_t base = hex ? 16 : 10;
  uint64_t value = 0;
  bool prevWasDigit = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (IsDigitSeparator(c)) {
      const bool nextIsDigit = i + 1 < s.size() && DigitValue(s[i + 1]) >= 0 &&
                               static_cast<uint64_t>(DigitValue(s[i + 1])) < base;
      if (!prevWasDigit || !nextIsDigit) return std::nullopt;
      prevWasDigit = false;
      continue;
    }
    const int d = DigitValue(c);
    if (d < 0 || static_cast<uint64_t>(d) >= base) return std::nullopt;
    if (value > (std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(d)) / base) return std::nullopt;
    value = value * base + static_cast<uint64_t>(d);
    prevWasDigit = true;
  }
  return value;
}

std::string_view TrimView(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && IsSpace(s[b])) ++b;
  while (e > b && IsSpace(s[e - 1])) --e;
  return s.substr(b, e - b);
}

bool IsWordSeparator(char c) {
  return c == ' ' || c == '.' || c == '_' || c == '-' || c == '/' || c == '\\' || c == ':' || c == '\t';
}

namespace fuzzy {
constexpr int kMatch = 16;
constexpr int kWordStart = 24;
constexpr int kFirstChar = 8;
constexpr int kConsecutive = 16;
constexpr int kExactCase = 1;
constexpr int kGapStart = 3;          // first skipped char between two matches
constexpr int kGapExtend = 1;         // each further skipped char
constexpr int kMaxGapPenalty = 12;    // a far-away match is not worse than a moderately far one
constexpr size_t kGapWindow = 10;     // gaps >= this length are saturated at kMaxGapPenalty
constexpr size_t kMaxLeadingPenalty = 12;
constexpr size_t kMaxLengthPenalty = 16;
constexpr int kNone = std::numeric_limits<int>::min() / 2;
static_assert(kGapStart + kGapExtend * (static_cast<int>(kGapWindow) - 1) >= kMaxGapPenalty);

int GapPenalty(size_t gap) {
  return std::min(kGapStart + kGapExtend * static_cast<int>(gap - 1), kMaxGapPenalty);
}
}  // namespace fuzzy

}  // namespace

std::wstring Widen(std::string_view utf8) {
  if (utf8.empty() || utf8.size() > static_cast<size_t>(INT_MAX)) return {};
  try {
    const int len = static_cast<int>(utf8.size());
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), len, nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), len, out.data(), needed);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written));
    return out;
  } catch (...) {
    return {};
  }
}

std::string Narrow(std::wstring_view wide) {
  if (wide.empty() || wide.size() > static_cast<size_t>(INT_MAX)) return {};
  try {
    const int len = static_cast<int>(wide.size());
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), len, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, wide.data(), len, out.data(), needed, nullptr, nullptr);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written));
    return out;
  } catch (...) {
    return {};
  }
}

std::string ToLower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = ToLowerAscii(c);
  return out;
}

std::string Trim(std::string_view s) { return std::string(TrimView(s)); }

bool IEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) return false;
  return true;
}

bool IContains(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  if (needle.size() > haystack.size()) return false;
  const size_t last = haystack.size() - needle.size();
  for (size_t i = 0; i <= last; ++i) {
    size_t j = 0;
    while (j < needle.size() && ToLowerAscii(haystack[i + j]) == ToLowerAscii(needle[j])) ++j;
    if (j == needle.size()) return true;
  }
  return false;
}

bool StartsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

std::vector<std::string> Split(std::string_view s, char sep, bool skipEmpty) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t pos = s.find(sep, start);
    const std::string_view part = s.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start);
    if (!skipEmpty || !part.empty()) out.emplace_back(part);
    if (pos == std::string_view::npos) break;
    start = pos + 1;
  }
  return out;
}

std::optional<uint64_t> ParseUInt(std::string_view s, bool defaultHex) {
  return ParseMagnitude(TrimView(s), defaultHex);
}

std::optional<int64_t> ParseInt(std::string_view s, bool defaultHex) {
  s = TrimView(s);
  bool negative = false;
  if (!s.empty() && s[0] == '-') {
    negative = true;
    s.remove_prefix(1);
  }
  const auto mag = ParseMagnitude(s, defaultHex);
  if (!mag) return std::nullopt;
  constexpr uint64_t kMaxPos = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  if (negative) {
    if (*mag > kMaxPos + 1) return std::nullopt;
    if (*mag == kMaxPos + 1) return std::numeric_limits<int64_t>::min();
    return -static_cast<int64_t>(*mag);
  }
  if (*mag > kMaxPos) return std::nullopt;
  return static_cast<int64_t>(*mag);
}

std::optional<double> ParseDouble(std::string_view s) {
  s = TrimView(s);
  if (s.empty()) return std::nullopt;
  std::string_view body = s;
  bool negative = false;
  if (body[0] == '+' || body[0] == '-') {
    negative = body[0] == '-';
    body.remove_prefix(1);
  }
  if (body.size() >= 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X')) {
    const auto mag = ParseMagnitude(body, true);
    if (!mag) return std::nullopt;
    const double v = static_cast<double>(*mag);
    return negative ? -v : v;
  }
  if (body.empty() || body[0] == '+' || body[0] == '-') return std::nullopt;
  double v = 0;
  const char* first = body.data();
  const char* last = body.data() + body.size();
  const auto res = std::from_chars(first, last, v, std::chars_format::general);
  if (res.ec != std::errc() || res.ptr != last) return std::nullopt;
  if (!std::isfinite(v)) return std::nullopt;
  return negative ? -v : v;
}

std::string Hex(uint64_t v, int minDigits, bool prefix) {
  minDigits = std::clamp(minDigits, 0, 64);
  std::string digits = std::format("{:X}", v);
  if (static_cast<int>(digits.size()) < minDigits) digits.insert(0, static_cast<size_t>(minDigits) - digits.size(), '0');
  return prefix ? "0x" + digits : digits;
}

std::string HexBytes(const void* data, size_t n, char sep) {
  if (!data || n == 0) return {};
  static constexpr char kDigits[] = "0123456789ABCDEF";
  const auto* p = static_cast<const uint8_t*>(data);
  std::string out;
  out.reserve(n * (sep ? 3 : 2));
  for (size_t i = 0; i < n; ++i) {
    if (i && sep) out.push_back(sep);
    out.push_back(kDigits[p[i] >> 4]);
    out.push_back(kDigits[p[i] & 0xF]);
  }
  return out;
}

std::string FormatBytesSize(uint64_t bytes) {
  static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
  if (bytes < 1024) return std::format("{} B", bytes);
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  // Promote before 1023.95 so rounding never prints "1024.0 KB".
  while (value >= 1023.95 && unit + 1 < std::size(kUnits)) {
    value /= 1024.0;
    ++unit;
  }
  return std::format("{:.1f} {}", value, kUnits[unit]);
}

int FuzzyScore(std::string_view pattern, std::string_view text) {
  using namespace fuzzy;

  if (pattern.empty()) return 0;
  if (pattern.size() > text.size()) return -1;

  // Cheap subsequence rejection before the alignment.
  {
    size_t pi = 0;
    for (size_t ti = 0; ti < text.size() && pi < pattern.size(); ++ti)
      if (ToLowerAscii(text[ti]) == ToLowerAscii(pattern[pi])) ++pi;
    if (pi != pattern.size()) return -1;
  }

  try {
    const size_t m = text.size();
    std::vector<int> bonus(m);
    for (size_t j = 0; j < m; ++j) {
      int b = 0;
      if (j == 0) b = kWordStart + kFirstChar;
      else if (IsWordSeparator(text[j - 1])) b = kWordStart;
      else if (IsLowerAscii(text[j - 1]) && IsUpperAscii(text[j])) b = kWordStart;
      bonus[j] = b;
    }

    // prev[j]: best score of the previous pattern prefix with its last char matched at text[j].
    // O(pattern * text * kGapWindow): exact for short gaps, saturated penalty for long ones.
    std::vector<int> prev(m, kNone), cur(m, kNone);
    for (size_t j = 0; j < m; ++j) {
      if (ToLowerAscii(text[j]) != ToLowerAscii(pattern[0])) continue;
      const int lead = static_cast<int>(std::min(j, kMaxLeadingPenalty));
      prev[j] = kMatch + bonus[j] + (text[j] == pattern[0] ? kExactCase : 0) - lead;
    }
    for (size_t i = 1; i < pattern.size(); ++i) {
      const char pc = ToLowerAscii(pattern[i]);
      int farBest = kNone;   // max(prev[k]) over k <= j - 1 - kGapWindow
      for (size_t j = 0; j < m; ++j) {
        if (j >= kGapWindow + 1) farBest = std::max(farBest, prev[j - 1 - kGapWindow]);
        int score = kNone;
        if (j > 0 && ToLowerAscii(text[j]) == pc) {
          const int gain = kMatch + bonus[j] + (text[j] == pattern[i] ? kExactCase : 0);
          if (prev[j - 1] != kNone) score = prev[j - 1] + gain + kConsecutive;
          const size_t nearStart = j >= kGapWindow ? j - kGapWindow : 0;
          for (size_t k = nearStart; k + 1 < j; ++k)
            if (prev[k] != kNone) score = std::max(score, prev[k] + gain - GapPenalty(j - k - 1));
          if (farBest != kNone) score = std::max(score, farBest + gain - kMaxGapPenalty);
        }
        cur[j] = score;
      }
      prev.swap(cur);
    }
    int best = kNone;
    for (int v : prev) best = std::max(best, v);
    if (best == kNone) return -1;
    const int lengthPenalty =
        static_cast<int>(std::min(text.size() - pattern.size(), kMaxLengthPenalty * 4) / 4);
    return std::max(0, best - lengthPenalty);
  } catch (...) {
    return -1;
  }
}

uint64_t NowMs() { return GetTickCount64(); }

}  // namespace cg::util
