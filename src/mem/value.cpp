#include "mem/value.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <system_error>
#include <type_traits>

#include <windows.h>

#include "mem/safe.h"

namespace cg::mem {
namespace {

constexpr size_t kMaxFormattedLen = 4096;   // ReadFormatted cap for String/WString/Bytes

std::string_view TrimWs(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
  return s;
}

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Signed types accept decimal within the signed range and hex as raw two's-complement bits of the
// width ("0xFF" for i8 is -1), so FormatValue(hex=true) output parses back to the same bytes.
bool ParseInteger(std::string_view text, size_t bytes, bool isSigned, std::vector<uint8_t>& out) {
  std::string_view s = TrimWs(text);
  bool neg = false;
  if (!s.empty() && (s.front() == '+' || s.front() == '-')) {
    neg = s.front() == '-';
    s.remove_prefix(1);
  }
  bool hex = false;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    hex = true;
    s.remove_prefix(2);
  }
  if (s.empty()) return false;
  uint64_t mag = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), mag, hex ? 16 : 10);
  if (ec != std::errc() || ptr != s.data() + s.size()) return false;

  const unsigned bits = static_cast<unsigned>(bytes * 8);
  const uint64_t umax = bits >= 64 ? ~0ull : (1ull << bits) - 1;
  uint64_t value = 0;
  if (isSigned) {
    const uint64_t smax = umax >> 1;
    if (neg) {
      if (mag > smax + 1) return false;
      value = (0 - mag) & umax;
    } else {
      if (mag > (hex ? umax : smax)) return false;
      value = mag;
    }
  } else {
    if (neg && mag != 0) return false;
    if (mag > umax) return false;
    value = mag;
  }
  out.resize(bytes);
  std::memcpy(out.data(), &value, bytes);   // x64 is little-endian
  return true;
}

template <class F> bool ParseFloat(std::string_view text, std::vector<uint8_t>& out) {
  std::string_view s = TrimWs(text);
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  if (s.empty() || s.front() == '+') return false;
  F v{};
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v, std::chars_format::general);
  if (ec != std::errc() || ptr != s.data() + s.size()) return false;
  out.resize(sizeof(F));
  std::memcpy(out.data(), &v, sizeof(F));
  return true;
}

bool ParseBytes(std::string_view text, std::vector<uint8_t>& out) {
  std::vector<uint8_t> bytes;
  size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\t' && text[j] != '\r' && text[j] != '\n') ++j;
    const std::string_view tok = text.substr(i, j - i);
    i = j;
    if (tok.size() % 2) return false;
    for (size_t k = 0; k < tok.size(); k += 2) {
      const int hi = HexDigit(tok[k]), lo = HexDigit(tok[k + 1]);
      if (hi < 0 || lo < 0) return false;
      bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
  }
  if (bytes.empty()) return false;
  out = std::move(bytes);
  return true;
}

bool ParseWString(std::string_view text, std::vector<uint8_t>& out) {
  if (text.empty() || text.size() > static_cast<size_t>(INT32_MAX)) return false;
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (n <= 0) return false;
  std::wstring w(static_cast<size_t>(n), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), w.data(), n) != n)
    return false;
  out.resize(w.size() * 2);
  for (size_t i = 0; i < w.size(); ++i) {
    out[2 * i] = static_cast<uint8_t>(w[i] & 0xFF);
    out[2 * i + 1] = static_cast<uint8_t>((w[i] >> 8) & 0xFF);
  }
  return true;
}

template <class T> T Load(const void* data) {
  T v;
  std::memcpy(&v, data, sizeof(T));
  return v;
}

template <class T> std::string FormatInt(const void* data, bool hex) {
  const T v = Load<T>(data);
  if (hex) return std::format("0x{:X}", static_cast<std::make_unsigned_t<T>>(v));
  return std::format("{}", static_cast<std::conditional_t<std::is_signed_v<T>, int64_t, uint64_t>>(v));
}

// "100", "1.5", "0.000123": six decimals with trailing zeros trimmed; values too small or too large
// for that to be meaningful use six significant digits instead ("1.23e-07").
std::string FormatFloat(double v) {
  if (std::isnan(v)) return "nan";
  if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
  if (v == 0) return "0";
  char buf[64];
  const double a = std::fabs(v);
  const bool fixed = a >= 1e-4 && a < 1e15;
  const auto res = fixed ? std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, 6)
                         : std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general, 6);
  if (res.ec != std::errc()) return std::format("{}", v);
  std::string s(buf, res.ptr);
  if (fixed && s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  if (s == "-0") s = "0";
  return s;
}

// Display-safe UTF-8: invalid sequences and control characters become '.'.
std::string SanitizeUtf8(const uint8_t* p, size_t n) {
  std::string out;
  out.reserve(n);
  size_t i = 0;
  while (i < n) {
    const uint8_t c = p[i];
    if (c < 0x80) {
      out += (c < 0x20 || c == 0x7F) ? '.' : static_cast<char>(c);
      ++i;
      continue;
    }
    size_t len = 0;
    uint8_t lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) len = 2;
    else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      if (c == 0xE0) lo = 0xA0;
      if (c == 0xED) hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      if (c == 0xF0) lo = 0x90;
      if (c == 0xF4) hi = 0x8F;
    }
    bool ok = len != 0 && i + len <= n;
    for (size_t k = 1; ok && k < len; ++k) {
      const uint8_t cc = p[i + k];
      ok = k == 1 ? (cc >= lo && cc <= hi) : (cc >= 0x80 && cc <= 0xBF);
    }
    if (!ok) {
      out += '.';
      ++i;
      continue;
    }
    out.append(reinterpret_cast<const char*>(p + i), len);
    i += len;
  }
  return out;
}

std::string FormatWide(const uint8_t* p, size_t units) {
  std::wstring w;
  w.reserve(units);
  for (size_t i = 0; i < units; ++i) {
    wchar_t c = static_cast<wchar_t>(p[2 * i] | (p[2 * i + 1] << 8));
    if (c == 0) break;
    if (c < 0x20 || c == 0x7F) c = L'.';
    w += c;
  }
  if (w.empty()) return {};
  // Without WC_ERR_INVALID_CHARS unpaired surrogates become U+FFFD instead of failing.
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::string FormatHexBytes(const uint8_t* p, size_t n) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string s;
  s.reserve(n * 3);
  for (size_t i = 0; i < n; ++i) {
    if (i) s += ' ';
    s += kHex[p[i] >> 4];
    s += kHex[p[i] & 0xF];
  }
  return s;
}

}  // namespace

const char* ValueTypeName(ValueType t) {
  switch (t) {
    case ValueType::I8: return "i8";
    case ValueType::I16: return "i16";
    case ValueType::I32: return "i32";
    case ValueType::I64: return "i64";
    case ValueType::U8: return "u8";
    case ValueType::U16: return "u16";
    case ValueType::U32: return "u32";
    case ValueType::U64: return "u64";
    case ValueType::F32: return "f32";
    case ValueType::F64: return "f64";
    case ValueType::String: return "string";
    case ValueType::WString: return "wstring";
    case ValueType::Bytes: return "bytes";
  }
  return "?";
}

std::optional<ValueType> ParseValueType(std::string_view s) {
  s = TrimWs(s);
  for (ValueType t : kAllValueTypes) {
    const std::string_view name = ValueTypeName(t);
    if (name.size() != s.size()) continue;
    bool eq = true;
    for (size_t i = 0; i < s.size() && eq; ++i) eq = LowerAscii(s[i]) == name[i];
    if (eq) return t;
  }
  return std::nullopt;
}

size_t ValueSize(ValueType t) {
  switch (t) {
    case ValueType::I8: case ValueType::U8: return 1;
    case ValueType::I16: case ValueType::U16: return 2;
    case ValueType::I32: case ValueType::U32: case ValueType::F32: return 4;
    case ValueType::I64: case ValueType::U64: case ValueType::F64: return 8;
    case ValueType::String: case ValueType::WString: case ValueType::Bytes: return 0;
  }
  return 0;
}

bool IsNumeric(ValueType t) { return ValueSize(t) != 0; }
bool IsFloat(ValueType t) { return t == ValueType::F32 || t == ValueType::F64; }

bool ParseValue(ValueType t, std::string_view text, std::vector<uint8_t>& out) {
  try {
    switch (t) {
      case ValueType::I8: return ParseInteger(text, 1, true, out);
      case ValueType::I16: return ParseInteger(text, 2, true, out);
      case ValueType::I32: return ParseInteger(text, 4, true, out);
      case ValueType::I64: return ParseInteger(text, 8, true, out);
      case ValueType::U8: return ParseInteger(text, 1, false, out);
      case ValueType::U16: return ParseInteger(text, 2, false, out);
      case ValueType::U32: return ParseInteger(text, 4, false, out);
      case ValueType::U64: return ParseInteger(text, 8, false, out);
      case ValueType::F32: return ParseFloat<float>(text, out);
      case ValueType::F64: return ParseFloat<double>(text, out);
      case ValueType::String:
        if (text.empty()) return false;
        out.assign(text.begin(), text.end());
        return true;
      case ValueType::WString: return ParseWString(text, out);
      case ValueType::Bytes: return ParseBytes(text, out);
    }
  } catch (...) {
  }
  return false;
}

std::string FormatValue(ValueType t, const void* data, size_t size, bool hex) {
  if (!data) return "??";
  const size_t need = ValueSize(t);
  if (need && size < need) return "??";
  const auto* p = static_cast<const uint8_t*>(data);
  switch (t) {
    case ValueType::I8: return FormatInt<int8_t>(p, hex);
    case ValueType::I16: return FormatInt<int16_t>(p, hex);
    case ValueType::I32: return FormatInt<int32_t>(p, hex);
    case ValueType::I64: return FormatInt<int64_t>(p, hex);
    case ValueType::U8: return FormatInt<uint8_t>(p, hex);
    case ValueType::U16: return FormatInt<uint16_t>(p, hex);
    case ValueType::U32: return FormatInt<uint32_t>(p, hex);
    case ValueType::U64: return FormatInt<uint64_t>(p, hex);
    case ValueType::F32: return FormatFloat(static_cast<double>(Load<float>(p)));
    case ValueType::F64: return FormatFloat(Load<double>(p));
    case ValueType::String: {
      const auto* nul = static_cast<const uint8_t*>(std::memchr(p, 0, size));
      return SanitizeUtf8(p, nul ? static_cast<size_t>(nul - p) : size);
    }
    case ValueType::WString: return FormatWide(p, size / 2);
    case ValueType::Bytes: return FormatHexBytes(p, size);
  }
  return "??";
}

std::string ReadFormatted(uintptr_t addr, ValueType t, size_t len, bool hex) {
  try {
    if (IsNumeric(t)) {
      uint8_t buf[8];
      const size_t n = ValueSize(t);
      if (!ReadRaw(addr, buf, n)) return "??";
      return FormatValue(t, buf, n, hex);
    }
    len = std::min(len, kMaxFormattedLen);
    switch (t) {
      case ValueType::String: {
        uint8_t first = 0;
        if (!ReadRaw(addr, &first, 1)) return "??";
        const std::string s = ReadCString(addr, len);
        return FormatValue(t, s.data(), s.size(), hex);
      }
      case ValueType::WString: {
        uint16_t first = 0;
        if (!ReadRaw(addr, &first, sizeof(first))) return "??";
        const std::wstring w = ReadWString(addr, len);
        return FormatValue(t, w.data(), w.size() * sizeof(wchar_t), hex);
      }
      case ValueType::Bytes: {
        if (len == 0) return {};
        std::vector<uint8_t> buf(len);
        if (ReadRaw(addr, buf.data(), len)) return FormatHexBytes(buf.data(), len);
        // Partially readable: readability is per page, so read page by page and show the bytes of
        // unreadable pages as "??".
        std::string s;
        s.reserve(len * 3);
        for (size_t off = 0; off < len;) {
          constexpr size_t kPage = 0x1000;
          const size_t n = std::min(kPage - static_cast<size_t>((addr + off) & (kPage - 1)), len - off);
          const bool ok = ReadRaw(addr + off, buf.data() + off, n);
          for (size_t i = off; i < off + n; ++i) {
            if (i) s += ' ';
            s += ok ? FormatHexBytes(&buf[i], 1) : std::string("??");
          }
          off += n;
        }
        return s;
      }
      default: break;
    }
  } catch (...) {
  }
  return "??";
}

}  // namespace cg::mem
