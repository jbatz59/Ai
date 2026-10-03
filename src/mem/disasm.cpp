#include "mem/disasm.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <string_view>

#include <windows.h>

#include "hde64.h"

#include "mem/basic_internal.h"
#include "mem/safe.h"

namespace cg::mem {
namespace {

using detail::kPageSize;

constexpr size_t kMaxInsnLen = 15;
constexpr size_t kMaxRangeInsns = 65536;
constexpr size_t kMaxRangeBytes = 1u << 20;
constexpr size_t kPrevWindow = 48;

constexpr const char* kReg64[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
constexpr const char* kReg32[16] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
                                    "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
constexpr const char* kReg16[16] = {"ax",  "cx",  "dx",   "bx",   "sp",   "bp",   "si",   "di",
                                    "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
constexpr const char* kReg8Rex[16] = {"al",  "cl",  "dl",   "bl",   "spl",  "bpl",  "sil",  "dil",
                                      "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
constexpr const char* kReg8Legacy[8] = {"al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"};
constexpr const char* kCond[16] = {"o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"};
constexpr const char* kAlu[8] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
constexpr const char* kShift[8] = {"rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar"};
constexpr const char* kGroup3[8] = {"test", "test", "not", "neg", "mul", "imul", "div", "idiv"};

std::string Hex(uint64_t v) { return std::format("0x{:X}", v); }
uint64_t MaskBits(int bits) { return bits >= 64 ? ~0ull : (1ull << bits) - 1; }

bool IsLegacyPrefix(uint8_t c) {
  switch (c) {
    case 0xF0: case 0xF2: case 0xF3: case 0x2E: case 0x36: case 0x3E: case 0x26: case 0x64: case 0x65: case 0x66: case 0x67:
      return true;
    default: return false;
  }
}

size_t ImmBytes(uint32_t flags) {
  return ((flags & F_IMM8) ? 1 : 0) + ((flags & F_IMM16) ? 2 : 0) + ((flags & F_IMM32) ? 4 : 0) + ((flags & F_IMM64) ? 8 : 0);
}
size_t DispBytes(uint32_t flags) {
  if (flags & F_DISP32) return 4;
  if (flags & F_DISP16) return 2;
  if (flags & F_DISP8) return 1;
  return 0;
}

// Immediate as encoded, sign-extended from its own width.
int64_t ImmSigned(const hde64s& hs) {
  if (hs.flags & F_IMM64) return static_cast<int64_t>(hs.imm.imm64);
  if (hs.flags & F_IMM32) return static_cast<int32_t>(hs.imm.imm32);
  if (hs.flags & F_IMM16) return static_cast<int16_t>(hs.imm.imm16);
  if (hs.flags & F_IMM8) return static_cast<int8_t>(hs.imm.imm8);
  return 0;
}

std::string Db(const uint8_t* bytes, size_t n) {
  std::string s = "db";
  for (size_t i = 0; i < n; ++i) s += std::format(" 0x{:02X}", bytes[i]);
  return s;
}

std::string Op(std::string_view m) { return std::string(m); }
std::string Op(std::string_view m, const std::string& a) { return std::format("{} {}", m, a); }
std::string Op(std::string_view m, const std::string& a, const std::string& b) { return std::format("{} {}, {}", m, a, b); }
std::string Op(std::string_view m, const std::string& a, const std::string& b, const std::string& c) {
  return std::format("{} {}, {}, {}", m, a, b, c);
}

// Compact Intel-syntax printer. Returns "" for anything it does not know; the caller prints "db".
class Printer {
 public:
  Printer(const hde64s& hs, const Insn& insn)
      : hs_(hs), insn_(insn), rex_(hs.rex != 0),
        opSize_(hs.rex_w ? 64 : (hs.p_66 ? 16 : 32)),
        reg_(static_cast<unsigned>(hs.modrm_reg | (hs.rex_r << 3))),
        rm_(static_cast<unsigned>(hs.modrm_rm | (hs.rex_b << 3))),
        mand_(hs.p_rep ? hs.p_rep : (hs.p_66 ? 0x66 : 0)) {}

  std::string Print() const {
    if (hs_.flags & F_PREFIX_REX2) return {};
    const uint8_t* b = insn_.bytes;
    const size_t len = insn_.length;
    size_t i = 0;
    while (i < len && (IsLegacyPrefix(b[i]) || (b[i] & 0xF0) == 0x40)) ++i;
    if (i >= len) return {};
    const uint8_t op = b[i];
    if (op == 0xC4 || op == 0xC5 || op == 0x62 || op == 0xD5) return {};   // VEX / EVEX / REX2: not printed
    std::string text;
    if (op == 0x0F) {
      if (i + 1 >= len || b[i + 1] == 0x38 || b[i + 1] == 0x3A) return {};
      text = TwoByte(b[i + 1]);
    } else {
      if (op == 0x8F && (i + 1 >= len || (b[i + 1] & 0x38) != 0)) return {};   // XOP, not pop
      text = OneByte(op);
    }
    if (text.empty()) return text;
    if (hs_.p_lock) text = "lock " + text;
    if (insn_.ripRelative) text += " ; " + Hex(insn_.target);
    return text;
  }

 private:
  bool HasModrm() const { return (hs_.flags & F_MODRM) != 0; }
  bool RegForm() const { return hs_.modrm_mod == 3; }

  std::string R(unsigned idx, int bits) const {
    idx &= 15;
    switch (bits) {
      case 64: return kReg64[idx];
      case 32: return kReg32[idx];
      case 16: return kReg16[idx];
      default: return (rex_ || idx >= 8) ? kReg8Rex[idx] : kReg8Legacy[idx];
    }
  }
  static std::string Xmm(unsigned idx) { return std::format("xmm{}", idx & 15); }

  static const char* SizePtr(int bytes) {
    switch (bytes) {
      case 1: return "byte ptr ";
      case 2: return "word ptr ";
      case 4: return "dword ptr ";
      case 8: return "qword ptr ";
      case 16: return "xmmword ptr ";
      default: return "";
    }
  }

  std::string Mem(int bytes, bool showSize) const {
    std::string s = showSize ? SizePtr(bytes) : "";
    if (hs_.p_seg == 0x64) s += "fs:";
    else if (hs_.p_seg == 0x65) s += "gs:";
    s += '[';
    const bool a32 = hs_.p_67 != 0;
    auto name = [a32](unsigned r) -> const char* { return a32 ? kReg32[r & 15] : kReg64[r & 15]; };
    int64_t disp = 0;
    if (hs_.flags & F_DISP8) disp = static_cast<int8_t>(hs_.disp.disp8);
    else if (hs_.flags & F_DISP32) disp = static_cast<int32_t>(hs_.disp.disp32);
    auto appendDisp = [&s, disp]() {
      if (disp > 0) s += "+" + Hex(static_cast<uint64_t>(disp));
      else if (disp < 0) s += "-" + Hex(static_cast<uint64_t>(-disp));
    };

    if (hs_.modrm_mod == 0 && hs_.modrm_rm == 5) {
      s += a32 ? "eip" : "rip";
      appendDisp();
      return s + ']';
    }
    bool hasBase = true, hasIndex = false;
    unsigned base = rm_, index = 0, scale = 1;
    if (hs_.flags & F_SIB) {
      base = static_cast<unsigned>(hs_.sib_base | (hs_.rex_b << 3));
      if (hs_.sib_base == 5 && hs_.modrm_mod == 0) hasBase = false;
      const unsigned idx = static_cast<unsigned>(hs_.sib_index | (hs_.rex_x << 3));
      if (idx != 4) {
        hasIndex = true;
        index = idx;
        scale = 1u << hs_.sib_scale;
      }
    }
    if (hasBase) s += name(base);
    if (hasIndex) {
      if (hasBase) s += '+';
      s += name(index);
      if (scale != 1) s += std::format("*{}", scale);
    }
    if (!hasBase && !hasIndex) s += Hex(a32 ? static_cast<uint32_t>(disp) : static_cast<uint64_t>(disp));
    else appendDisp();
    return s + ']';
  }

  std::string E(int bits, bool showSize) const { return RegForm() ? R(rm_, bits) : Mem(bits / 8, showSize); }
  std::string W() const { return RegForm() ? Xmm(rm_) : Mem(0, false); }
  std::string ImmAs(int bits) const { return Hex(static_cast<uint64_t>(ImmSigned(hs_)) & MaskBits(bits)); }
  std::string Target() const { return Hex(insn_.target); }
  const char* Sse() const { return mand_ == 0xF3 ? "ss" : mand_ == 0xF2 ? "sd" : mand_ == 0x66 ? "pd" : "ps"; }
  std::string Rep() const { return hs_.p_rep == 0xF3 ? "rep " : ""; }
  std::string StringOp(const char* stem) const {
    const char* suffix = opSize_ == 64 ? "q" : opSize_ == 16 ? "w" : "d";
    return Rep() + stem + suffix;
  }

  std::string OneByte(uint8_t op) const {
    const int stackSize = hs_.p_66 ? 16 : 64;
    if (op < 0x40 && (op & 7) < 6) {
      if (!HasModrm() && (op & 7) < 4) return {};
      const char* m = kAlu[op >> 3];
      switch (op & 7) {
        case 0: return Op(m, E(8, false), R(reg_, 8));
        case 1: return Op(m, E(opSize_, false), R(reg_, opSize_));
        case 2: return Op(m, R(reg_, 8), E(8, false));
        case 3: return Op(m, R(reg_, opSize_), E(opSize_, false));
        case 4: return Op(m, "al", ImmAs(8));
        default: return Op(m, R(0, opSize_), ImmAs(opSize_));
      }
    }
    if (op >= 0x50 && op <= 0x57) return Op("push", R((op & 7u) | (hs_.rex_b << 3), stackSize));
    if (op >= 0x58 && op <= 0x5F) return Op("pop", R((op & 7u) | (hs_.rex_b << 3), stackSize));
    if (op >= 0x70 && op <= 0x7F) return Op(std::string("j") + kCond[op & 15], Target());
    if (op >= 0x91 && op <= 0x97) return Op("xchg", R((op & 7u) | (hs_.rex_b << 3), opSize_), R(0, opSize_));
    if (op >= 0xB0 && op <= 0xB7) return Op("mov", R((op & 7u) | (hs_.rex_b << 3), 8), ImmAs(8));
    if (op >= 0xB8 && op <= 0xBF) return Op("mov", R((op & 7u) | (hs_.rex_b << 3), opSize_), ImmAs(opSize_));

    const bool needsModrm = op == 0x63 || op == 0x69 || op == 0x6B || (op >= 0x80 && op <= 0x8F) || op == 0xC0 ||
                            op == 0xC1 || op == 0xC6 || op == 0xC7 || (op >= 0xD0 && op <= 0xD3) || op == 0xF6 ||
                            op == 0xF7 || op == 0xFE || op == 0xFF;
    if (needsModrm && !HasModrm()) return {};
    const unsigned sub = hs_.modrm_reg;

    switch (op) {
      case 0x63: return Op("movsxd", R(reg_, opSize_), E(32, true));
      case 0x68: return Op("push", ImmAs(stackSize));
      case 0x69: return Op("imul", R(reg_, opSize_), E(opSize_, false), ImmAs(opSize_));
      case 0x6A: return Op("push", ImmAs(stackSize));
      case 0x6B: return Op("imul", R(reg_, opSize_), E(opSize_, false), ImmAs(opSize_));
      case 0x80: return Op(kAlu[sub], E(8, true), ImmAs(8));
      case 0x81: case 0x83: return Op(kAlu[sub], E(opSize_, true), ImmAs(opSize_));
      case 0x84: return Op("test", E(8, false), R(reg_, 8));
      case 0x85: return Op("test", E(opSize_, false), R(reg_, opSize_));
      case 0x86: return Op("xchg", E(8, false), R(reg_, 8));
      case 0x87: return Op("xchg", E(opSize_, false), R(reg_, opSize_));
      case 0x88: return Op("mov", E(8, false), R(reg_, 8));
      case 0x89: return Op("mov", E(opSize_, false), R(reg_, opSize_));
      case 0x8A: return Op("mov", R(reg_, 8), E(8, false));
      case 0x8B: return Op("mov", R(reg_, opSize_), E(opSize_, false));
      case 0x8D: return RegForm() ? std::string() : Op("lea", R(reg_, opSize_), Mem(0, false));
      case 0x8F: return sub == 0 ? Op("pop", E(stackSize, true)) : std::string();
      case 0x90:
        if (hs_.rex_b) return Op("xchg", R(8, opSize_), R(0, opSize_));
        return hs_.p_rep == 0xF3 ? Op("pause") : Op("nop");
      case 0x98: return Op(hs_.rex_w ? "cdqe" : hs_.p_66 ? "cbw" : "cwde");
      case 0x99: return Op(hs_.rex_w ? "cqo" : hs_.p_66 ? "cwd" : "cdq");
      case 0xA4: return Rep() + "movsb";
      case 0xA5: return StringOp("movs");
      case 0xA8: return Op("test", "al", ImmAs(8));
      case 0xA9: return Op("test", R(0, opSize_), ImmAs(opSize_));
      case 0xAA: return Rep() + "stosb";
      case 0xAB: return StringOp("stos");
      case 0xC0: return Op(kShift[sub], E(8, true), Hex(hs_.imm.imm8));
      case 0xC1: return Op(kShift[sub], E(opSize_, true), Hex(hs_.imm.imm8));
      case 0xC2: return Op("ret", Hex(hs_.imm.imm16));
      case 0xC3: return Op("ret");
      case 0xC6: return sub == 0 ? Op("mov", E(8, true), ImmAs(8)) : std::string();
      case 0xC7: return sub == 0 ? Op("mov", E(opSize_, true), ImmAs(opSize_)) : std::string();
      case 0xC9: return Op("leave");
      case 0xCC: return Op("int3");
      case 0xCD: return Op("int", Hex(hs_.imm.imm8));
      case 0xD0: return Op(kShift[sub], E(8, true), "1");
      case 0xD1: return Op(kShift[sub], E(opSize_, true), "1");
      case 0xD2: return Op(kShift[sub], E(8, true), "cl");
      case 0xD3: return Op(kShift[sub], E(opSize_, true), "cl");
      case 0xE8: return Op("call", Target());
      case 0xE9: case 0xEB: return Op("jmp", Target());
      case 0xF4: return Op("hlt");
      case 0x9C: return Op(hs_.p_66 ? "pushf" : "pushfq");
      case 0x9D: return Op(hs_.p_66 ? "popf" : "popfq");
      case 0x9E: return Op("sahf");
      case 0x9F: return Op("lahf");
      case 0xF5: return Op("cmc");
      case 0xF8: return Op("clc");
      case 0xF9: return Op("stc");
      case 0xFC: return Op("cld");
      case 0xFD: return Op("std");
      case 0xF6: return sub <= 1 ? Op("test", E(8, true), ImmAs(8)) : Op(kGroup3[sub], E(8, true));
      case 0xF7: return sub <= 1 ? Op("test", E(opSize_, true), ImmAs(opSize_)) : Op(kGroup3[sub], E(opSize_, true));
      case 0xFE:
        if (sub > 1) return {};
        return Op(sub == 0 ? "inc" : "dec", E(8, true));
      case 0xFF:
        switch (sub) {
          case 0: return Op("inc", E(opSize_, true));
          case 1: return Op("dec", E(opSize_, true));
          case 2: return Op("call", E(64, true));
          case 4: return Op("jmp", E(64, true));
          case 6: return Op("push", E(stackSize, true));
          default: return {};
        }
      default: return {};
    }
  }

  std::string TwoByte(uint8_t op) const {
    switch (op) {
      case 0x05: return Op("syscall");
      case 0x0B: return Op("ud2");
      case 0x31: return Op("rdtsc");
      case 0xA2: return Op("cpuid");
      default: break;
    }
    if (op >= 0x80 && op <= 0x8F) return Op(std::string("j") + kCond[op & 15], Target());
    if (op >= 0xC8 && op <= 0xCF) return Op("bswap", R((op & 7u) | (hs_.rex_b << 3), hs_.rex_w ? 64 : 32));
    if (!HasModrm()) return {};
    const int gpr = hs_.rex_w ? 64 : 32;   // GPR operand of SSE conversions / movd
    const std::string x = Xmm(reg_);
    const unsigned sub = hs_.modrm_reg;

    if (op >= 0x40 && op <= 0x4F) return Op(std::string("cmov") + kCond[op & 15], R(reg_, opSize_), E(opSize_, false));
    if (op >= 0x90 && op <= 0x9F) return Op(std::string("set") + kCond[op & 15], E(8, true));
    if (mand_ == 0x66) {
      if (const char* m = PackedIntOp(op)) return Op(m, x, W());
    }

    switch (op) {
      case 0x1F: return sub == 0 ? Op("nop", E(opSize_, true)) : std::string();
      case 0x18: {
        static constexpr const char* kPrefetch[4] = {"prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2"};
        return (!RegForm() && sub < 4) ? Op(kPrefetch[sub], Mem(1, true)) : std::string();
      }
      case 0x10: case 0x11: {
        const char* m = mand_ == 0xF3 ? "movss" : mand_ == 0xF2 ? "movsd" : mand_ == 0x66 ? "movupd" : "movups";
        return op == 0x10 ? Op(m, x, W()) : Op(m, W(), x);
      }
      case 0x12: case 0x16: {
        if (mand_ != 0 && mand_ != 0x66) return {};
        const bool high = op == 0x16;
        if (RegForm()) return mand_ == 0 ? Op(high ? "movlhps" : "movhlps", x, Xmm(rm_)) : std::string();
        return Op(std::string(high ? "movh" : "movl") + (mand_ == 0x66 ? "pd" : "ps"), x, W());
      }
      case 0x13: case 0x17: {
        if ((mand_ != 0 && mand_ != 0x66) || RegForm()) return {};
        return Op(std::string(op == 0x17 ? "movh" : "movl") + (mand_ == 0x66 ? "pd" : "ps"), W(), x);
      }
      case 0x14: case 0x15: {
        if (mand_ != 0 && mand_ != 0x66) return {};
        return Op(std::string(op == 0x14 ? "unpckl" : "unpckh") + (mand_ == 0x66 ? "pd" : "ps"), x, W());
      }
      case 0x28: case 0x29: {
        if (mand_ != 0 && mand_ != 0x66) return {};
        const char* m = mand_ == 0x66 ? "movapd" : "movaps";
        return op == 0x28 ? Op(m, x, W()) : Op(m, W(), x);
      }
      case 0x2A:
        if (mand_ != 0xF3 && mand_ != 0xF2) return {};
        return Op(mand_ == 0xF3 ? "cvtsi2ss" : "cvtsi2sd", x, E(gpr, true));
      case 0x2C: case 0x2D: {
        if (mand_ != 0xF3 && mand_ != 0xF2) return {};
        const std::string m = std::string(op == 0x2C ? "cvtt" : "cvt") + (mand_ == 0xF3 ? "ss2si" : "sd2si");
        return Op(m, R(reg_, gpr), W());
      }
      case 0x2E: case 0x2F: {
        if (mand_ != 0 && mand_ != 0x66) return {};
        const std::string m = std::string(op == 0x2E ? "ucomis" : "comis") + (mand_ == 0x66 ? "d" : "s");
        return Op(m, x, W());
      }
      case 0x50:
        if ((mand_ != 0 && mand_ != 0x66) || !RegForm()) return {};
        return Op(mand_ == 0x66 ? "movmskpd" : "movmskps", R(reg_, 32), Xmm(rm_));
      case 0x51: return Op(std::string("sqrt") + Sse(), x, W());
      case 0x58: return Op(std::string("add") + Sse(), x, W());
      case 0x59: return Op(std::string("mul") + Sse(), x, W());
      case 0x5C: return Op(std::string("sub") + Sse(), x, W());
      case 0x5D: return Op(std::string("min") + Sse(), x, W());
      case 0x5E: return Op(std::string("div") + Sse(), x, W());
      case 0x5F: return Op(std::string("max") + Sse(), x, W());
      case 0xC2: return Op(std::string("cmp") + Sse(), x, W(), Hex(hs_.imm.imm8));
      case 0x54: case 0x55: case 0x56: case 0x57: {
        if (mand_ != 0 && mand_ != 0x66) return {};
        static constexpr const char* kLogic[4] = {"and", "andn", "or", "xor"};
        return Op(std::string(kLogic[op - 0x54]) + (mand_ == 0x66 ? "pd" : "ps"), x, W());
      }
      case 0x5A: {
        const char* m = mand_ == 0xF3 ? "cvtss2sd" : mand_ == 0xF2 ? "cvtsd2ss" : mand_ == 0x66 ? "cvtpd2ps" : "cvtps2pd";
        return Op(m, x, W());
      }
      case 0x5B: {
        if (mand_ == 0xF2) return {};
        const char* m = mand_ == 0xF3 ? "cvttps2dq" : mand_ == 0x66 ? "cvtps2dq" : "cvtdq2ps";
        return Op(m, x, W());
      }
      case 0x6E:
        if (mand_ != 0x66) return {};
        return Op(hs_.rex_w ? "movq" : "movd", x, E(gpr, true));
      case 0x7E:
        if (mand_ == 0x66) return Op(hs_.rex_w ? "movq" : "movd", E(gpr, true), x);
        if (mand_ == 0xF3) return Op("movq", x, W());
        return {};
      case 0x6F: case 0x7F: {
        if (mand_ != 0x66 && mand_ != 0xF3) return {};
        const char* m = mand_ == 0x66 ? "movdqa" : "movdqu";
        return op == 0x6F ? Op(m, x, W()) : Op(m, W(), x);
      }
      case 0x70: {
        const char* m = mand_ == 0x66 ? "pshufd" : mand_ == 0xF3 ? "pshufhw" : mand_ == 0xF2 ? "pshuflw" : nullptr;
        return m ? Op(m, x, W(), Hex(hs_.imm.imm8)) : std::string();
      }
      case 0x71: case 0x72: case 0x73: {
        if (mand_ != 0x66 || !RegForm()) return {};
        static constexpr const char* kShiftW[8] = {nullptr, nullptr, "psrlw", nullptr, "psraw", nullptr, "psllw", nullptr};
        static constexpr const char* kShiftD[8] = {nullptr, nullptr, "psrld", nullptr, "psrad", nullptr, "pslld", nullptr};
        static constexpr const char* kShiftQ[8] = {nullptr, nullptr, "psrlq", "psrldq", nullptr, nullptr, "psllq", "pslldq"};
        const char* m = (op == 0x71 ? kShiftW : op == 0x72 ? kShiftD : kShiftQ)[sub];
        return m ? Op(m, Xmm(rm_), Hex(hs_.imm.imm8)) : std::string();
      }
      case 0xC6:
        if (mand_ != 0 && mand_ != 0x66) return {};
        return Op(mand_ == 0x66 ? "shufpd" : "shufps", x, W(), Hex(hs_.imm.imm8));
      case 0xD6: return mand_ == 0x66 ? Op("movq", W(), x) : std::string();
      case 0xD7: return (mand_ == 0x66 && RegForm()) ? Op("pmovmskb", R(reg_, 32), Xmm(rm_)) : std::string();
      case 0xAE:
        if (mand_ != 0) return {};
        if (RegForm()) return sub == 5 ? Op("lfence") : sub == 6 ? Op("mfence") : sub == 7 ? Op("sfence") : std::string();
        if (sub == 2) return Op("ldmxcsr", Mem(4, true));
        if (sub == 3) return Op("stmxcsr", Mem(4, true));
        if (sub == 7) return Op("clflush", Mem(1, true));
        return {};
      case 0xA3: return Op("bt", E(opSize_, false), R(reg_, opSize_));
      case 0xAB: return Op("bts", E(opSize_, false), R(reg_, opSize_));
      case 0xB3: return Op("btr", E(opSize_, false), R(reg_, opSize_));
      case 0xBB: return Op("btc", E(opSize_, false), R(reg_, opSize_));
      case 0xBA: {
        static constexpr const char* kBt[4] = {"bt", "bts", "btr", "btc"};
        return sub >= 4 ? Op(kBt[sub - 4], E(opSize_, true), Hex(hs_.imm.imm8)) : std::string();
      }
      case 0xA4: return Op("shld", E(opSize_, false), R(reg_, opSize_), Hex(hs_.imm.imm8));
      case 0xA5: return Op("shld", E(opSize_, false), R(reg_, opSize_), "cl");
      case 0xAC: return Op("shrd", E(opSize_, false), R(reg_, opSize_), Hex(hs_.imm.imm8));
      case 0xAD: return Op("shrd", E(opSize_, false), R(reg_, opSize_), "cl");
      case 0xB0: return Op("cmpxchg", E(8, false), R(reg_, 8));
      case 0xB1: return Op("cmpxchg", E(opSize_, false), R(reg_, opSize_));
      case 0xC0: return Op("xadd", E(8, false), R(reg_, 8));
      case 0xC1: return Op("xadd", E(opSize_, false), R(reg_, opSize_));
      case 0xC7:
        if (sub == 1 && !RegForm()) return Op(hs_.rex_w ? "cmpxchg16b" : "cmpxchg8b", Mem(hs_.rex_w ? 16 : 8, true));
        if (sub == 6 && RegForm()) return Op("rdrand", R(rm_, opSize_));
        return {};
      case 0xB8: return mand_ == 0xF3 ? Op("popcnt", R(reg_, opSize_), E(opSize_, false)) : std::string();
      case 0xBC: return Op(mand_ == 0xF3 ? "tzcnt" : "bsf", R(reg_, opSize_), E(opSize_, false));
      case 0xBD: return Op(mand_ == 0xF3 ? "lzcnt" : "bsr", R(reg_, opSize_), E(opSize_, false));
      case 0xAF: return Op("imul", R(reg_, opSize_), E(opSize_, false));
      case 0xB6: return Op("movzx", R(reg_, opSize_), E(8, true));
      case 0xB7: return Op("movzx", R(reg_, opSize_), E(16, true));
      case 0xBE: return Op("movsx", R(reg_, opSize_), E(8, true));
      case 0xBF: return Op("movsx", R(reg_, opSize_), E(16, true));
      default: return {};
    }
  }

  // 66 0F xx integer SSE2 operations of the form "op xmm, xmm/m128".
  static const char* PackedIntOp(uint8_t op) {
    switch (op) {
      case 0x60: return "punpcklbw";
      case 0x61: return "punpcklwd";
      case 0x62: return "punpckldq";
      case 0x63: return "packsswb";
      case 0x64: return "pcmpgtb";
      case 0x65: return "pcmpgtw";
      case 0x66: return "pcmpgtd";
      case 0x67: return "packuswb";
      case 0x68: return "punpckhbw";
      case 0x69: return "punpckhwd";
      case 0x6A: return "punpckhdq";
      case 0x6B: return "packssdw";
      case 0x6C: return "punpcklqdq";
      case 0x6D: return "punpckhqdq";
      case 0x74: return "pcmpeqb";
      case 0x75: return "pcmpeqw";
      case 0x76: return "pcmpeqd";
      case 0xD4: return "paddq";
      case 0xD5: return "pmullw";
      case 0xDB: return "pand";
      case 0xDF: return "pandn";
      case 0xEB: return "por";
      case 0xEF: return "pxor";
      case 0xF8: return "psubb";
      case 0xF9: return "psubw";
      case 0xFA: return "psubd";
      case 0xFB: return "psubq";
      case 0xFC: return "paddb";
      case 0xFD: return "paddw";
      case 0xFE: return "paddd";
      default: return nullptr;
    }
  }

  const hde64s& hs_;
  const Insn& insn_;
  bool rex_;
  int opSize_;
  unsigned reg_;
  unsigned rm_;
  uint8_t mand_;
};

// Reads up to `n` (<= 15) bytes at addr, stopping early at an unreadable page. Returns bytes read.
size_t ReadUpTo(uintptr_t addr, uint8_t* out, size_t n) {
  const size_t first = std::min(n, kPageSize - static_cast<size_t>(addr & (kPageSize - 1)));
  if (!ReadRaw(addr, out, first)) return 0;
  if (first < n && ReadRaw(addr + first, out + first, n - first)) return n;
  return first;
}

}  // namespace

namespace detail {

bool DecodeBytes(const uint8_t* data, size_t avail, uintptr_t address, Insn& out, bool wantText) {
  out = Insn{};
  out.address = address;
  if (!data || avail == 0) return false;
  // HDE never reads past 15 bytes; the zero padding keeps a short buffer from being overrun.
  uint8_t buf[kMaxInsnLen + 1] = {};
  std::memcpy(buf, data, std::min(avail, kMaxInsnLen));
  hde64s hs;
  const unsigned len = hde64_disasm(buf, &hs);
  if ((hs.flags & F_ERROR) || len == 0 || len > kMaxInsnLen || len > avail) return false;

  out.length = static_cast<uint8_t>(len);
  std::memcpy(out.bytes, buf, len);
  // Immediates always end the instruction and the displacement sits right before them, whatever
  // mix of prefixes, escapes and ModRM/SIB bytes precedes.
  const size_t immSize = ImmBytes(hs.flags);
  const size_t dispSize = DispBytes(hs.flags);
  if (immSize + dispSize > len) return false;
  if (immSize) {
    out.immSize = static_cast<uint8_t>(immSize);
    out.immOffset = static_cast<uint8_t>(len - immSize);
  }
  if (dispSize) {
    out.dispSize = static_cast<uint8_t>(dispSize);
    out.dispOffset = static_cast<uint8_t>(len - immSize - dispSize);
  }
  out.ripRelative = (hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5;
  out.relBranch = (hs.flags & F_RELATIVE) != 0;
  if (out.relBranch) {
    out.target = address + len + static_cast<uintptr_t>(ImmSigned(hs));
  } else if (out.ripRelative) {
    uintptr_t t = address + len + static_cast<uintptr_t>(static_cast<int64_t>(static_cast<int32_t>(hs.disp.disp32)));
    if (hs.p_67) t &= 0xFFFFFFFFull;   // 32-bit address size: EIP-relative
    out.target = t;
  }
  if (wantText) {
    std::string text = Printer(hs, out).Print();
    out.text = text.empty() ? Db(out.bytes, out.length) : std::move(text);
  }
  return true;
}

bool DecodeAt(uintptr_t address, Insn& out, bool wantText) {
  uint8_t buf[kMaxInsnLen] = {};
  const size_t avail = ReadUpTo(address, buf, kMaxInsnLen);
  if (avail == 0) {
    out = Insn{};
    out.address = address;
    return false;
  }
  return DecodeBytes(buf, avail, address, out, wantText);
}

}  // namespace detail

bool Decode(uintptr_t address, Insn& out) {
  try {
    return detail::DecodeAt(address, out, true);
  } catch (...) {
    out.length = 0;
    return false;
  }
}

// Invalid but readable bytes become 1-byte "db" entries so a listing can continue past data; the
// range ends at the first unreadable byte.
std::vector<Insn> DecodeRange(uintptr_t address, size_t maxInstructions) {
  std::vector<Insn> out;
  maxInstructions = std::min(maxInstructions, kMaxRangeInsns);
  if (maxInstructions == 0) return out;
  try {
    const size_t want = std::min(maxInstructions * kMaxInsnLen, kMaxRangeBytes);
    std::vector<uint8_t> buf(want);
    size_t have = 0;
    while (have < want) {
      const size_t n = std::min(kPageSize - static_cast<size_t>((address + have) & (kPageSize - 1)), want - have);
      if (!ReadRaw(address + have, buf.data() + have, n)) break;
      have += n;
    }
    out.reserve(std::min<size_t>(maxInstructions, 1024));
    size_t off = 0;
    while (out.size() < maxInstructions && off < have) {
      Insn insn;
      if (detail::DecodeBytes(buf.data() + off, have - off, address + off, insn, true)) {
        off += insn.length;
        out.push_back(std::move(insn));
        continue;
      }
      if (have - off < kMaxInsnLen) break;   // may simply be cut off by the end of what was read
      Insn db;
      db.address = address + off;
      db.length = 1;
      db.bytes[0] = buf[off];
      db.text = Db(db.bytes, 1);
      out.push_back(std::move(db));
      ++off;
    }
  } catch (...) {
  }
  return out;
}

uintptr_t PreviousInstruction(uintptr_t nextRip) {
  if (!IsPlausiblePtr(nextRip) || nextRip < detail::kUserMin + kPrevWindow) return 0;
  uint8_t win[kPrevWindow];
  uintptr_t lo = nextRip - kPrevWindow;
  if (!ReadRaw(lo, win, kPrevWindow)) {
    const uintptr_t page = (nextRip - 1) & ~static_cast<uintptr_t>(kPageSize - 1);
    if (page <= lo) return 0;
    lo = page;
    if (!ReadRaw(lo, win, static_cast<size_t>(nextRip - lo))) return 0;
  }

  // Each start point in the window sweeps forward; an instruction that ends exactly at nextRip gets
  // a vote. Sweeps from different points resynchronise quickly on x64, so the true instruction
  // collects most of the votes.
  int votes[kMaxInsnLen + 1] = {};
  for (uintptr_t s = lo; s < nextRip; ++s) {
    uintptr_t cur = s;
    while (cur < nextRip) {
      Insn insn;
      if (!detail::DecodeBytes(win + (cur - lo), static_cast<size_t>(nextRip - cur), cur, insn, false)) break;
      if (cur + insn.length == nextRip) {
        ++votes[insn.length];
        break;
      }
      cur += insn.length;
    }
  }
  size_t best = 0;
  for (size_t k = kMaxInsnLen; k >= 1; --k)   // ties favour the longer instruction
    if (votes[k] > votes[best]) best = k;
  return best ? nextRip - best : 0;
}

}  // namespace cg::mem
