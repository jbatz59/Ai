#pragma once
#include <filesystem>
#include <string_view>

#include <windows.h>

namespace cg::paths {

// Must be called first (from DllMain/bootstrap) with Chroma's own module handle.
// Creates DataDir() and its standard subfolders if missing.
void Init(HMODULE self);

HMODULE Self();
const std::filesystem::path& ModuleDir();   // folder containing Chroma.dll
const std::filesystem::path& DataDir();     // ModuleDir()/Chroma
const std::filesystem::path& GameExe();     // full path of the host process exe

// DataDir()/rel — e.g. Data(L"config.json"), Data(L"bindings"), Data(L"scripts"), Data(L"profiles"), Data(L"crash")
std::filesystem::path Data(std::wstring_view rel);

}  // namespace cg::paths
