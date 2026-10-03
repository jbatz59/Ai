#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace cg::mem {

struct Section {
  std::string name;              // ".text", ".rdata", ".data", ...
  uintptr_t start = 0;
  size_t size = 0;               // VirtualSize
  uint32_t characteristics = 0;  // IMAGE_SCN_*
  bool Executable() const { return characteristics & IMAGE_SCN_MEM_EXECUTE; }
  bool Writable() const { return characteristics & IMAGE_SCN_MEM_WRITE; }
  bool Contains(uintptr_t a) const { return a >= start && a < start + size; }
};

struct Module {
  std::string name;   // lower-case file name, e.g. "mafiadefinitiveedition.exe"
  std::wstring path;
  uintptr_t base = 0;
  size_t size = 0;    // SizeOfImage
  std::vector<Section> sections;

  bool Valid() const { return base != 0; }
  bool Contains(uintptr_t a) const { return a >= base && a < base + size; }
  const Section* FindSection(std::string_view name) const;
  const Section* SectionOf(uintptr_t a) const;
  uintptr_t Rva(uintptr_t a) const { return a - base; }

  static Module FromHandle(HMODULE h);                    // parses PE headers in memory
  static const Module& Main();                            // host exe, cached after first call
  static std::optional<Module> Find(std::string_view name);   // case-insensitive
  static std::vector<Module> All();                       // snapshot of loaded modules
  static std::optional<Module> Containing(uintptr_t addr);
};

// "mafiadefinitiveedition.exe+0x1A2B3C" or raw hex when outside any module.
std::string Describe(uintptr_t addr);

// Exported symbol lookup ("kernel32.dll", "Sleep").
std::optional<uintptr_t> FindExport(std::string_view module, std::string_view symbol);

}  // namespace cg::mem
