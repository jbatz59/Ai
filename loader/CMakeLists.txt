# Loaders. Pick ONE way to get Consigliere.dll into the game (see docs/INSTALL.md):
#   xinput1_4.dll  proxy placed next to mafiadefinitiveedition.exe (the game imports xinput1_4)
#   ConsigliereInjector.exe  manual injection into a running game
#   or Ultimate ASI Loader (third party) with Consigliere.dll renamed to Consigliere.asi
add_library(proxy_xinput1_4 SHARED proxy/xinput1_4.cpp proxy/xinput1_4.def)
set_target_properties(proxy_xinput1_4 PROPERTIES PREFIX "" OUTPUT_NAME "xinput1_4")

add_executable(ConsigliereInjector injector/main.cpp)
target_link_libraries(ConsigliereInjector PRIVATE psapi)
if(MSVC)
  target_link_options(ConsigliereInjector PRIVATE /SUBSYSTEM:CONSOLE)
else()
  target_link_options(ConsigliereInjector PRIVATE -municode)
endif()
