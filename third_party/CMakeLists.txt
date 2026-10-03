# Vendored dependencies. Versions are pinned in THIRD_PARTY.md.

# --- Dear ImGui (MIT) -------------------------------------------------------
add_library(cg_imgui STATIC
  imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp imgui/imgui_demo.cpp
  imgui/backends/imgui_impl_dx11.cpp imgui/backends/imgui_impl_win32.cpp)
target_include_directories(cg_imgui PUBLIC imgui imgui/backends)
target_compile_definitions(cg_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(cg_imgui PUBLIC d3d11 d3dcompiler dxgi dwmapi imm32)

# --- MinHook (BSD-2) --------------------------------------------------------
add_library(cg_minhook STATIC
  minhook/src/buffer.c minhook/src/hook.c minhook/src/trampoline.c minhook/src/hde/hde64.c)
target_include_directories(cg_minhook PUBLIC minhook/include PRIVATE minhook/src)
target_include_directories(cg_minhook INTERFACE minhook/src/hde)

# --- Lua 5.4 (MIT) — used for Consigliere's own user scripts ---------------
file(GLOB CG_LUA_SOURCES CONFIGURE_DEPENDS lua/*.c)
add_library(cg_lua STATIC ${CG_LUA_SOURCES})
target_include_directories(cg_lua PUBLIC lua)
if(NOT MSVC)
  target_compile_options(cg_lua PRIVATE -w)
endif()

# --- nlohmann/json (MIT, header only) --------------------------------------
add_library(cg_json INTERFACE)
target_include_directories(cg_json INTERFACE nlohmann/include)
target_compile_definitions(cg_json INTERFACE JSON_USE_IMPLICIT_CONVERSIONS=1)
