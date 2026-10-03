cmake_minimum_required(VERSION 3.20)
project(Chroma VERSION 1.0.0 LANGUAGES C CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "Chroma targets Windows x64. On Linux use -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Mafia: Definitive Edition is 64-bit; configure an x64 build.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(CG_BUILD_TESTS "Build unit tests and the D3D11 test host" ON)

# Static CRT everywhere: the DLL must load into the game with zero extra runtime DLLs.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  add_link_options(-static -static-libgcc -static-libstdc++)
endif()

set(CG_VERSION_STRING "${PROJECT_VERSION}")
configure_file(src/version.h.in ${CMAKE_BINARY_DIR}/generated/cg_version.h @ONLY)

add_subdirectory(third_party)

# ---------------------------------------------------------------------------
# Core library: everything except the DLL entry point, so tests can link it.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CG_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)
list(FILTER CG_SOURCES EXCLUDE REGEX ".*/src/core/dllmain\\.cpp$")

add_library(cg_core STATIC ${CG_SOURCES})
target_include_directories(cg_core PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/generated PRIVATE ${CMAKE_SOURCE_DIR}/third_party/fonts)
target_link_libraries(cg_core PUBLIC cg_imgui cg_minhook cg_lua cg_json cg_stb
  d3d11 dxgi d3dcompiler dxguid dbghelp psapi shlwapi ole32 uuid)
if(MSVC)
  target_compile_options(cg_core PRIVATE /W4 /wd4100 /wd4201 /wd4324)
else()
  target_compile_options(cg_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type)
endif()

add_library(Chroma SHARED src/core/dllmain.cpp src/resources/chroma.rc)
target_include_directories(Chroma PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(Chroma PRIVATE cg_core)
set_target_properties(Chroma PROPERTIES PREFIX "" OUTPUT_NAME "Chroma")

# ---------------------------------------------------------------------------
# Loaders: proxy DLLs (drop next to the game exe) and a manual injector.
# ---------------------------------------------------------------------------
add_subdirectory(loader)

if(CG_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install layout == what the user copies into the game folder.
# ---------------------------------------------------------------------------
install(TARGETS Chroma RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(DIRECTORY bindings/ DESTINATION Chroma/bindings)
install(DIRECTORY scripts/ DESTINATION Chroma/scripts)
