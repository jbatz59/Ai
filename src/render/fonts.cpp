#include "render/fonts.h"

#include <inter_font.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>

#include <windows.h>

#include <imgui.h>

#include "core/log.h"

namespace cg::render::fonts {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxFontFileBytes = 64ull * 1024 * 1024;

fs::path FontsDir() {
  wchar_t buf[MAX_PATH] = {};
  const UINT n = GetWindowsDirectoryW(buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return fs::path(buf) / L"Fonts";
  const DWORD m = GetEnvironmentVariableW(L"WINDIR", buf, MAX_PATH);
  if (m > 0 && m < MAX_PATH) return fs::path(buf) / L"Fonts";
  return fs::path(L"C:\\Windows\\Fonts");
}

bool LooksLikeSfnt(const unsigned char* d, size_t n) {
  if (n < 12) return false;
  const uint32_t tag = (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | uint32_t(d[3]);
  return tag == 0x00010000u || tag == 0x74727565u /*true*/ || tag == 0x4F54544Fu /*OTTO*/ || tag == 0x74746366u /*ttcf*/;
}

// Reads a font file into an IM_ALLOC buffer (ownership passes to the atlas). Win32 I/O so paths
// with any characters work and nothing throws.
void* ReadFontFile(const fs::path& path, int* outSize) {
  *outSize = 0;
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return nullptr;
  void* data = nullptr;
  LARGE_INTEGER size{};
  if (GetFileSizeEx(h, &size) && size.QuadPart > 100 && static_cast<uint64_t>(size.QuadPart) <= kMaxFontFileBytes) {
    const DWORD want = static_cast<DWORD>(size.QuadPart);
    data = IM_ALLOC(want);
    DWORD got = 0;
    if (data == nullptr || !ReadFile(h, data, want, &got, nullptr) || got != want ||
        !LooksLikeSfnt(static_cast<const unsigned char*>(data), got)) {
      if (data != nullptr) IM_FREE(data);
      data = nullptr;
    } else {
      *outSize = static_cast<int>(got);
    }
  }
  CloseHandle(h);
  return data;
}

ImFont* AddFile(ImFontAtlas* atlas, const fs::path& path, float size, ImFontConfig cfg) {
  int bytes = 0;
  void* data = ReadFontFile(path, &bytes);
  if (data == nullptr) return nullptr;
  cfg.FontDataOwnedByAtlas = true;
  cfg.Flags |= ImFontFlags_NoLoadError;
  if (cfg.Name[0] == '\0') {
    const std::string file = path.filename().string();
    std::strncpy(cfg.Name, file.c_str(), sizeof(cfg.Name) - 1);
    cfg.Name[sizeof(cfg.Name) - 1] = '\0';
  }
  // On failure the atlas has already released `data` (ownership was transferred).
  return atlas->AddFontFromMemoryTTF(data, bytes, size, &cfg);
}

ImFont* AddFirst(ImFontAtlas* atlas, const fs::path& dir, std::initializer_list<const wchar_t*> files, float size,
                 const ImFontConfig& cfg, std::wstring* chosen = nullptr) {
  for (const wchar_t* f : files) {
    if (ImFont* font = AddFile(atlas, dir / f, size, cfg)) {
      if (chosen) *chosen = f;
      return font;
    }
  }
  return nullptr;
}

// Inter: the minimalist UI typeface, embedded so the menu looks identical on every system.
ImFont* AddInter(ImFontAtlas* atlas, float size, const char* name) {
  ImFontConfig cfg;
  cfg.OversampleH = 2;
  cfg.OversampleV = 1;
  cfg.SizePixels = size;
  std::strncpy(cfg.Name, name, sizeof(cfg.Name) - 1);
  return atlas->AddFontFromMemoryCompressedTTF(inter_compressed_data, static_cast<int>(inter_compressed_size), size, &cfg);
}

ImFont* AddEmbedded(ImFontAtlas* atlas, float size, const char* name) {
  ImFontConfig cfg;
  cfg.SizePixels = size;   // explicit reference size so icon fonts may still be merged into it
  std::strncpy(cfg.Name, name, sizeof(cfg.Name) - 1);
  return atlas->AddFontDefaultVector(&cfg);
}

}  // namespace

void Load(float scale, Fonts& out) {
  out = Fonts{};
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGuiIO& io = ImGui::GetIO();
  ImFontAtlas* atlas = io.Fonts;
  const fs::path dir = FontsDir();

  ImFontConfig text;
  text.OversampleH = 2;
  text.OversampleV = 1;

  std::wstring bodyFile = L"Inter (embedded)";
  out.body = AddInter(atlas, kBodySize, "Inter");
  if (out.body == nullptr) out.body = AddFirst(atlas, dir, {L"segoeui.ttf"}, kBodySize, text, &bodyFile);
  if (out.body == nullptr) {
    out.body = AddEmbedded(atlas, kBodySize, "Embedded (body)");
    bodyFile = L"embedded";
  }

  // Icons go into the body font: restricted to the Private Use Area so the icon font never
  // replaces a regular character the text font lacks.
  static const ImWchar kIconOnly[] = {0x0001, 0xE6FF, 0xF900, 0xFFFF, 0};
  std::wstring iconFile;
  if (out.body != nullptr) {
    ImFontConfig icons;
    icons.MergeMode = true;
    icons.DstFont = out.body;
    icons.GlyphMinAdvanceX = kBodySize * 1.15f;   // fixed advance keeps icon columns aligned
    icons.GlyphOffset = ImVec2(0.0f, kBodySize * 0.17f);   // icon fonts sit on the baseline; centre them on the text line
    icons.GlyphExcludeRanges = kIconOnly;
    icons.OversampleH = 1;
    icons.OversampleV = 1;
    if (AddFirst(atlas, dir, {L"SegoeIcons.ttf", L"segmdl2.ttf"}, kBodySize * 0.92f, icons, &iconFile) != nullptr) {
      out.hasIcons = true;
      out.icons = out.body;
    }
  }

  // Minimalist: one typeface, hierarchy comes from size and spacing rather than heavy weights.
  out.bold = out.body;
  out.title = out.body;

  ImFontConfig mono;
  mono.OversampleH = 2;
  mono.OversampleV = 1;
  std::wstring monoFile;
  out.mono = AddFirst(atlas, dir, {L"consola.ttf", L"lucon.ttf", L"cour.ttf"}, kMonoSize, mono, &monoFile);
  if (out.mono == nullptr) {
    // The embedded font is monospaced; reuse it when it already is the body font.
    out.mono = (bodyFile == L"embedded") ? out.body : AddEmbedded(atlas, kMonoSize, "Embedded (mono)");
    monoFile = L"embedded";
  }
  if (out.mono == nullptr) out.mono = out.body;

  if (out.body != nullptr) io.FontDefault = out.body;

  auto narrow = [](const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return s.empty() ? std::string("none") : s;
  };
  log::Info("render", "Fonts: body={} icons={} mono={} bold={} (ui scale {:.2f}, sizes are dynamic)", narrow(bodyFile),
            out.hasIcons ? narrow(iconFile) : std::string("none"), narrow(monoFile),
            out.bold != out.body ? "loaded" : "body", scale);
}

}  // namespace cg::render::fonts
