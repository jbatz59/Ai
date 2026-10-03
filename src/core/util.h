#pragma once
// Small, dependency-free helpers shared by every module.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cg::util {

std::wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view wide);

std::string ToLower(std::string_view s);
std::string Trim(std::string_view s);
bool IEquals(std::string_view a, std::string_view b);
bool IContains(std::string_view haystack, std::string_view needle);
bool StartsWith(std::string_view s, std::string_view prefix);
std::vector<std::string> Split(std::string_view s, char sep, bool skipEmpty = true);

// "0x1A", "1Ah" are not accepted; "0x1A", "1A" (hex=true) and "26" (hex=false) are.
std::optional<uint64_t> ParseUInt(std::string_view s, bool defaultHex = false);
std::optional<int64_t> ParseInt(std::string_view s, bool defaultHex = false);
std::optional<double> ParseDouble(std::string_view s);

std::string Hex(uint64_t v, int minDigits = 0, bool prefix = true);   // Hex(0x1f) == "0x1F"
std::string HexBytes(const void* data, size_t n, char sep = ' ');     // "48 8B 05"
std::string FormatBytesSize(uint64_t bytes);                          // "12.3 MB"

// Fuzzy subsequence match for the command palette. Returns <0 for no match,
// otherwise a score where larger is better (consecutive + word-start bonuses).
int FuzzyScore(std::string_view pattern, std::string_view text);

uint64_t NowMs();   // monotonic milliseconds (GetTickCount64)

}  // namespace cg::util
