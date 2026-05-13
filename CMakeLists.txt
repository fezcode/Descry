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
  # Embed the multi-size .ico into the exe so Explorer + taskbar pick it
  # up automatically. CMake's RC language drives windres on MinGW; the
  # .rc references downsee.ico relative to its own directory.
  enable_language(RC)
  target_sources(downsee PRIVATE resources/downsee.rc)

  # Bundle the MinGW64 runtime DLLs (SDL2, freetype, harfbuzz, etc. and
  # all their transitive deps) next to the exe so the build dir is
  # self-contained — no PATH dependency on D:/Apps/msys64/mingw64/bin.
  # See cmake/CopyRuntimeDeps.cmake for the actual copy.
  get_filename_component(MINGW_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
  add_custom_command(TARGET downsee POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      "-DEXE=$<TARGET_FILE:downsee>"
      "-DOUT_DIR=$<TARGET_FILE_DIR:downsee>"
      "-DMINGW_BIN=${MINGW_BIN_DIR}"
      -P "${CMAKE_SOURCE_DIR}/cmake/CopyRuntimeDeps.cmake"
    COMMENT "Bundling MinGW runtime DLLs"
    VERBATIM)
endif()
