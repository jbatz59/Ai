#include "mem/module.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>

#include <windows.h>
#include <psapi.h>

#include "mem/basic_internal.h"
#include "mem/safe.h"

namespace cg::mem {
namespace {

constexpr size_t kMaxSections = 96;           // the Windows loader refuses images with more
constexpr size_t kMaxPathChars = 32768;
constexpr size_t kMaxModules = 8192;

std::wstring Widen(std::string_view s) {
  if (s.empty() || s.size() > static_cast<size_t>(INT32_MAX)) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string Narrow(std::wstring_view w) {
  if (w.empty() || w.size() > static_cast<size_t>(INT32_MAX)) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}

// Locale-independent lower-casing (module names compare like the loader does, not like the user's
// locale would).
std::wstring LowerW(std::wstring_view s) {
  std::wstring out(s);
  if (out.empty()) return out;
  const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), static_cast<int>(s.size()), out.data(),
                              static_cast<int>(out.size()), nullptr, nullptr, 0);
  if (n != static_cast<int>(s.size())) {
    out.assign(s);
    for (wchar_t& c : out)
      if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
  }
  return out;
}

std::wstring ModulePath(HMODULE h) {
  std::wstring buf(MAX_PATH, L'\0');
  while (buf.size() <= kMaxPathChars) {
    SetLastError(ERROR_SUCCESS);
    const DWORD n = GetModuleFileNameW(h, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    if (n < buf.size() && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      buf.resize(n);
      return buf;
    }
    buf.resize(buf.size() * 2);
  }
  return {};
}

std::string FileNameLower(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  const std::wstring_view file = slash == std::wstring::npos ? std::wstring_view(path) : std::wstring_view(path).substr(slash + 1);
  return Narrow(LowerW(file));
}

bool HasDllOrExeExtension(std::string_view name) {
  if (name.size() < 4) return false;
  const std::string_view ext = name.substr(name.size() - 4);
  auto ieq = [](std::string_view a, const char* b) {
    for (size_t i = 0; i < a.size(); ++i) {
      char c = a[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != b[i]) return false;
    }
    return true;
  };
  return ieq(ext, ".dll") || ieq(ext, ".exe");
}

}  // namespace

namespace detail {

HMODULE FindModuleHandle(std::string_view name) {
  if (name.empty() || name.size() > kMaxPathChars) return nullptr;
  const std::wstring w = Widen(name);
  if (w.empty() || w.find(L'\0') != std::wstring::npos) return nullptr;
  // GetModuleHandleW appends ".dll" itself when the name has no extension at all; the explicit
  // variants below also cover dotted names ("my.mod" -> "my.mod.dll") and executables.
  if (HMODULE h = GetModuleHandleW(w.c_str())) return h;
  if (HasDllOrExeExtension(name)) return nullptr;
  if (HMODULE h = GetModuleHandleW((w + L".dll").c_str())) return h;
  return GetModuleHandleW((w + L".exe").c_str());
}

}  // namespace detail

const Section* Module::FindSection(std::string_view sectionName) const {
  for (const Section& s : sections)
    if (s.name == sectionName) return &s;
  return nullptr;
}

const Section* Module::SectionOf(uintptr_t a) const {
  for (const Section& s : sections)
    if (s.Contains(a)) return &s;
  return nullptr;
}

Module Module::FromHandle(HMODULE h) {
  Module m;
  const uintptr_t base = reinterpret_cast<uintptr_t>(h);
  // Handles with the low bits set are LOAD_LIBRARY_AS_DATAFILE / AS_IMAGE_RESOURCE mappings: those
  // are laid out like the file on disk, not like a loaded image.
  if (!IsPlausiblePtr(base) || (base & 3) != 0) return m;

  IMAGE_DOS_HEADER dos{};
  if (!ReadRaw(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return m;
  if (dos.e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) || dos.e_lfanew > 0x100000) return m;
  const uintptr_t nt = base + static_cast<uintptr_t>(dos.e_lfanew);

  DWORD signature = 0;
  IMAGE_FILE_HEADER file{};
  WORD magic = 0;
  if (!ReadRaw(nt, &signature, sizeof(signature)) || signature != IMAGE_NT_SIGNATURE) return m;
  const uintptr_t fileHdr = nt + sizeof(DWORD);
  const uintptr_t optHdr = fileHdr + sizeof(IMAGE_FILE_HEADER);
  if (!ReadRaw(fileHdr, &file, sizeof(file)) || !ReadRaw(optHdr, &magic, sizeof(magic))) return m;

  DWORD sizeOfImage = 0;
  if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    if (file.SizeOfOptionalHeader < offsetof(IMAGE_OPTIONAL_HEADER64, SizeOfImage) + sizeof(DWORD)) return m;
    if (!ReadRaw(optHdr + offsetof(IMAGE_OPTIONAL_HEADER64, SizeOfImage), &sizeOfImage, sizeof(sizeOfImage))) return m;
  } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
    if (file.SizeOfOptionalHeader < offsetof(IMAGE_OPTIONAL_HEADER32, SizeOfImage) + sizeof(DWORD)) return m;
    if (!ReadRaw(optHdr + offsetof(IMAGE_OPTIONAL_HEADER32, SizeOfImage), &sizeOfImage, sizeof(sizeOfImage))) return m;
  } else {
    return m;
  }
  if (sizeOfImage == 0) return m;

  m.base = base;
  m.size = sizeOfImage;

  const size_t count = std::min<size_t>(file.NumberOfSections, kMaxSections);
  if (count > 0) {
    IMAGE_SECTION_HEADER headers[kMaxSections];
    const uintptr_t table = optHdr + file.SizeOfOptionalHeader;
    if (ReadRaw(table, headers, count * sizeof(IMAGE_SECTION_HEADER))) {
      m.sections.reserve(count);
      for (size_t i = 0; i < count; ++i) {
        const IMAGE_SECTION_HEADER& sh = headers[i];
        if (sh.VirtualAddress >= sizeOfImage) continue;
        size_t size = sh.Misc.VirtualSize ? sh.Misc.VirtualSize : sh.SizeOfRawData;
        size = std::min<size_t>(size, sizeOfImage - sh.VirtualAddress);
        if (size == 0) continue;
        Section s;
        const char* raw = reinterpret_cast<const char*>(sh.Name);
        s.name.assign(raw, static_cast<size_t>(std::find(raw, raw + IMAGE_SIZEOF_SHORT_NAME, '\0') - raw));
        s.start = base + sh.VirtualAddress;
        s.size = size;
        s.characteristics = sh.Characteristics;
        m.sections.push_back(std::move(s));
      }
    }
  }

  m.path = ModulePath(h);
  m.name = FileNameLower(m.path);
  return m;
}

const Module& Module::Main() {
  static const Module main = FromHandle(GetModuleHandleW(nullptr));
  return main;
}

std::optional<Module> Module::Find(std::string_view moduleName) {
  HMODULE h = detail::FindModuleHandle(moduleName);
  if (!h) return std::nullopt;
  Module m = FromHandle(h);
  if (!m.Valid()) return std::nullopt;
  return m;
}

std::vector<Module> Module::All() {
  std::vector<HMODULE> handles(512);
  for (;;) {
    DWORD needed = 0;
    const DWORD bytes = static_cast<DWORD>(handles.size() * sizeof(HMODULE));
    if (!K32EnumProcessModules(GetCurrentProcess(), handles.data(), bytes, &needed)) return {};
    const size_t count = needed / sizeof(HMODULE);
    if (count <= handles.size()) {
      handles.resize(count);
      break;
    }
    // Modules were loaded between the calls (or the first buffer was small): retry with headroom.
    if (count > kMaxModules) return {};
    handles.resize(count + 64);
  }

  std::vector<Module> out;
  out.reserve(handles.size());
  for (HMODULE h : handles) {
    Module m = FromHandle(h);
    if (m.Valid()) out.push_back(std::move(m));
  }
  return out;
}

std::optional<Module> Module::Containing(uintptr_t addr) {
  if (!IsPlausiblePtr(addr)) return std::nullopt;
  HMODULE h = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(addr), &h) || !h)
    return std::nullopt;
  Module m = FromHandle(h);
  if (!m.Valid() || !m.Contains(addr)) return std::nullopt;
  return m;
}

std::string Describe(uintptr_t addr) {
  if (IsPlausiblePtr(addr)) {
    HMODULE h = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &h) && h) {
      const std::string name = FileNameLower(ModulePath(h));
      const uintptr_t base = reinterpret_cast<uintptr_t>(h);
      if (!name.empty() && addr >= base) return std::format("{}+0x{:X}", name, addr - base);
    }
  }
  return std::format("0x{:X}", addr);
}

std::optional<uintptr_t> FindExport(std::string_view moduleName, std::string_view symbol) {
  if (symbol.empty() || symbol.find('\0') != std::string_view::npos) return std::nullopt;
  HMODULE h = detail::FindModuleHandle(moduleName);
  if (!h) return std::nullopt;
  const std::string sym(symbol);
  FARPROC p = GetProcAddress(h, sym.c_str());
  if (!p) return std::nullopt;
  return reinterpret_cast<uintptr_t>(p);
}

}  // namespace cg::mem
