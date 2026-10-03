# Cross-compile from Linux/macOS with MinGW-w64 (posix threads variant).
#   cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_prefix x86_64-w64-mingw32)
find_program(_cc NAMES ${_prefix}-gcc-posix ${_prefix}-gcc REQUIRED)
find_program(_cxx NAMES ${_prefix}-g++-posix ${_prefix}-g++ REQUIRED)
find_program(_rc NAMES ${_prefix}-windres REQUIRED)

set(CMAKE_C_COMPILER ${_cc})
set(CMAKE_CXX_COMPILER ${_cxx})
set(CMAKE_RC_COMPILER ${_rc})

set(CMAKE_FIND_ROOT_PATH /usr/${_prefix})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
