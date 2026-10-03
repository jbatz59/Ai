# Unit tests (run anywhere a Windows exe runs, including Wine) and the D3D11 test host.
file(GLOB CG_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp)
add_executable(cg_tests ${CG_TEST_SOURCES})
target_link_libraries(cg_tests PRIVATE cg_core)

# A fake "game": D3D11 window + planted player struct + bindings file. Loads Consigliere.dll and
# can capture a screenshot of the overlay / exercise unload (see tests/host/host.cpp).
add_executable(cg_testhost host/host.cpp)
target_link_libraries(cg_testhost PRIVATE d3d11 dxgi)
add_dependencies(cg_testhost Consigliere)
if(NOT MSVC)
  target_link_options(cg_testhost PRIVATE -municode)
endif()
