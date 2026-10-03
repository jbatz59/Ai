#include "mem/expr.h"

#include <format>

#include <windows.h>

#include "mem/basic_internal.h"
#include "mem/safe.h"

namespace cg::mem {
namespace {

constexpr size_t kMaxExprLen = 4096;
constexpr int kMaxDepth = 64;

bool IsWordChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':' ||
         c == '$' || c == '@';
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool AllHex(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s)
    if (HexDigit(c) < 0) return false;
  return true;
}

uint64_t HexValue(std::string_view s) {   // caller checked AllHex and length <= 16
  uint64_t v = 0;
  for (char c : s) v = (v << 4) | static_cast<uint64_t>(HexDigit(c));
  return v;
}

// Grammar (all arithmetic wraps modulo 2^64):
//   expr    := term (('+' | '-') term)*
//   term    := unary ('*' unary)*
//   unary   := ('-' | '+') unary | primary
//   primary := '(' expr ')' | '[' expr ']' | '#' decimal | '"' module '"' | word
class Parser {
 public:
  Parser(std::string_view s, const SymbolResolver& resolver) : s_(s), resolver_(resolver) {}

  bool Run(uintptr_t& out) {
    SkipWs();
    if (AtEnd()) return Fail("empty expression", 0);
    if (!Expr(out, 0)) return false;
    SkipWs();
    if (!AtEnd()) return Fail(std::format("unexpected '{}'", s_[pos_]), pos_);
    return true;
  }

  const std::string& Error() const { return error_; }

 private:
  bool AtEnd() const { return pos_ >= s_.size(); }
  void SkipWs() {
    while (!AtEnd() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) ++pos_;
  }
  bool Fail(std::string msg, size_t at) {
    if (error_.empty()) error_ = std::format("{} at column {}", msg, at + 1);
    return false;
  }
  bool Expect(char c) {
    SkipWs();
    if (AtEnd()) return Fail(std::format("expected '{}' but the expression ended", c), pos_);
    if (s_[pos_] != c) return Fail(std::format("expected '{}' but found '{}'", c, s_[pos_]), pos_);
    ++pos_;
    return true;
  }

  bool Expr(uintptr_t& v, int depth) {
    if (depth > kMaxDepth) return Fail("expression nested too deeply", pos_);
    if (!Term(v, depth)) return false;
    for (;;) {
      SkipWs();
      if (AtEnd() || (s_[pos_] != '+' && s_[pos_] != '-')) return true;
      const char op = s_[pos_++];
      uintptr_t rhs = 0;
      if (!Term(rhs, depth)) return false;
      v = op == '+' ? v + rhs : v - rhs;
    }
  }

  bool Term(uintptr_t& v, int depth) {
    if (!Unary(v, depth)) return false;
    for (;;) {
      SkipWs();
      if (AtEnd() || s_[pos_] != '*') return true;
      ++pos_;
      uintptr_t rhs = 0;
      if (!Unary(rhs, depth)) return false;
      v *= rhs;
    }
  }

  bool Unary(uintptr_t& v, int depth) {
    SkipWs();
    if (!AtEnd() && (s_[pos_] == '-' || s_[pos_] == '+')) {
      if (depth > kMaxDepth) return Fail("expression nested too deeply", pos_);
      const bool neg = s_[pos_++] == '-';
      if (!Unary(v, depth + 1)) return false;
      if (neg) v = 0 - v;
      return true;
    }
    return Primary(v, depth);
  }

  bool Primary(uintptr_t& v, int depth) {
    SkipWs();
    if (AtEnd()) return Fail("unexpected end of expression", pos_);
    const size_t start = pos_;
    const char c = s_[pos_];

    if (c == '(') {
      ++pos_;
      return Expr(v, depth + 1) && Expect(')');
    }
    if (c == '[') {
      ++pos_;
      uintptr_t addr = 0;
      if (!Expr(addr, depth + 1) || !Expect(']')) return false;
      uint64_t value = 0;
      if (!ReadRaw(addr, &value, sizeof(value))) return Fail(std::format("cannot read pointer at 0x{:X}", addr), start);
      v = static_cast<uintptr_t>(value);
      return true;
    }
    if (c == '#') {
      ++pos_;
      const size_t digits = pos_;
      uint64_t value = 0;
      while (!AtEnd() && s_[pos_] >= '0' && s_[pos_] <= '9') {
        const uint64_t d = static_cast<uint64_t>(s_[pos_] - '0');
        if (value > (UINT64_MAX - d) / 10) return Fail("decimal number does not fit in 64 bits", start);
        value = value * 10 + d;
        ++pos_;
      }
      if (pos_ == digits) return Fail("expected decimal digits after '#'", pos_);
      if (!AtEnd() && IsWordChar(s_[pos_])) return Fail("invalid decimal number", start);
      v = static_cast<uintptr_t>(value);
      return true;
    }
    if (c == '"') {
      ++pos_;
      const size_t nameBegin = pos_;
      while (!AtEnd() && s_[pos_] != '"') ++pos_;
      if (AtEnd()) return Fail("unterminated quoted module name", start);
      const std::string_view name = s_.substr(nameBegin, pos_ - nameBegin);
      ++pos_;
      if (name.empty()) return Fail("empty quoted module name", start);
      if (HMODULE h = detail::FindModuleHandle(name)) {
        v = reinterpret_cast<uintptr_t>(h);
        return true;
      }
      if (resolver_) {
        if (auto r = resolver_(name)) {
          v = *r;
          return true;
        }
      }
      return Fail(std::format("module '{}' is not loaded", name), start);
    }
    if (IsWordChar(c)) {
      while (!AtEnd() && IsWordChar(s_[pos_])) ++pos_;
      return Word(s_.substr(start, pos_ - start), start, v);
    }
    return Fail(std::format("unexpected '{}'", c), start);
  }

  // Order: explicit 0x number, loaded module, resolver symbol, bare hex number.
  bool Word(std::string_view w, size_t at, uintptr_t& v) {
    if (w.size() > 2 && w[0] == '0' && (w[1] == 'x' || w[1] == 'X')) {
      const std::string_view digits = w.substr(2);
      if (!AllHex(digits)) return Fail(std::format("invalid hex number '{}'", w), at);
      if (digits.size() > 16) return Fail(std::format("number '{}' does not fit in 64 bits", w), at);
      v = static_cast<uintptr_t>(HexValue(digits));
      return true;
    }
    if (HMODULE h = detail::FindModuleHandle(w)) {
      v = reinterpret_cast<uintptr_t>(h);
      return true;
    }
    if (resolver_) {
      if (auto r = resolver_(w)) {
        v = *r;
        return true;
      }
    }
    if (AllHex(w)) {
      if (w.size() > 16) return Fail(std::format("number '{}' does not fit in 64 bits", w), at);
      v = static_cast<uintptr_t>(HexValue(w));
      return true;
    }
    return Fail(std::format("unknown symbol '{}'", w), at);
  }

  std::string_view s_;
  const SymbolResolver& resolver_;
  size_t pos_ = 0;
  std::string error_;
};

}  // namespace

ExprResult EvalAddress(std::string_view expr, const SymbolResolver& resolver) {
  ExprResult r;
  if (expr.size() > kMaxExprLen) {
    r.error = std::format("expression is longer than {} characters", kMaxExprLen);
    return r;
  }
  try {
    Parser p(expr, resolver);
    uintptr_t v = 0;
    if (p.Run(v)) {
      r.ok = true;
      r.value = v;
    } else {
      r.error = p.Error();
    }
  } catch (...) {
    // Only a throwing resolver or allocation failure gets here.
    r.ok = false;
    r.value = 0;
    try {
      r.error = "expression evaluation failed";
    } catch (...) {
    }
  }
  return r;
}

}  // namespace cg::mem
