cmake_minimum_required(VERSION 3.20)
project(downsee C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# --- vendored Lua 5.4 (no upstream CMake; we ship our own next to it) ----
add_subdirectory(vendor/lua)

# --- vendored md4c (CommonMark parser; ships its own CMakeLists) ---------
set(BUILD_MD2HTML_EXECUTABLE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS        OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/md4c)

# --- system libs via MSYS2 mingw64 pkg-config ----------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2     REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(LIBPNG   REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(LIBJPEG  REQUIRED IMPORTED_TARGET libjpeg)

# --- the binary -----------------------------------------------------------
add_executable(downsee
  src/main.c
  src/buffer.c
  src/font.c
  src/icons.c
  src/image.c
  src/lua_host.c
  src/markdown.c
  src/regex.c
  src/vault.c
)
target_include_directories(downsee PRIVATE src vendor/nanosvg)
target_link_libraries(downsee PRIVATE
  PkgConfig::SDL2
  PkgConfig::FREETYPE
  PkgConfig::HARFBUZZ
  PkgConfig::LIBPNG
  PkgConfig::LIBJPEG
  lua_static
  md4c
  md4c-html
)
if(WIN32)
  target_link_libraries(downsee PRIVATE comdlg32)
endif()

# Stage data/ next to the executable so the binary can find init.lua
add_custom_command(TARGET downsee POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:downsee>/data)
