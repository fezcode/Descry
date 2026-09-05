cmake_minimum_required(VERSION 3.20)
project(descry C)

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
add_executable(descry
  src/main.c
  src/buffer.c
  src/decor.c
  src/font.c
  src/graph.c
  src/icon_raster.c
  src/icons.c
  src/image.c
  src/inputfield.c
  src/lua_host.c
  src/markdown.c
  src/mermaid.c
  src/regex.c
  src/reveal.c
  src/spell.c
  src/tabs.c
  src/vault.c
)
if(APPLE)
  # The native menu bar and the window-corner clip are the only Objective-C
  # in the tree; ARC keeps their Cocoa objects alive without hand-written
  # retains.
  enable_language(OBJC)
  target_sources(descry PRIVATE src/macos_menu.m src/macos_window.m)
  set_source_files_properties(src/macos_menu.m src/macos_window.m
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

target_include_directories(descry PRIVATE src vendor/nanosvg)
target_link_libraries(descry PRIVATE
  PkgConfig::SDL2
  PkgConfig::FREETYPE
  PkgConfig::HARFBUZZ
  PkgConfig::LIBPNG
  PkgConfig::LIBJPEG
  lua_static
  md4c
  md4c-html
)
if(APPLE)
  # QuartzCore: CALayer, for the rounded-corner clip in macos_window.m.
  target_link_libraries(descry PRIVATE "-framework Cocoa" "-framework QuartzCore")
endif()
if(WIN32)
  # comdlg32: native file dialogs.  wininet: GET remote images in preview.
  target_link_libraries(descry PRIVATE comdlg32 wininet)
  # Embed the multi-size .ico into the exe so Explorer + taskbar pick it
  # up automatically. CMake's RC language drives windres on MinGW; the
  # .rc references descry.ico relative to its own directory.
  enable_language(RC)
  target_sources(descry PRIVATE resources/descry.rc)

  # Bundle the MinGW64 runtime DLLs (SDL2, freetype, harfbuzz, etc. and
  # all their transitive deps) next to the exe so the build dir is
  # self-contained — no PATH dependency on D:/Apps/msys64/mingw64/bin.
  # See cmake/CopyRuntimeDeps.cmake for the actual copy.
  get_filename_component(MINGW_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
  add_custom_command(TARGET descry POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      "-DEXE=$<TARGET_FILE:descry>"
      "-DOUT_DIR=$<TARGET_FILE_DIR:descry>"
      "-DMINGW_BIN=${MINGW_BIN_DIR}"
      -P "${CMAKE_SOURCE_DIR}/cmake/CopyRuntimeDeps.cmake"
    COMMENT "Bundling MinGW runtime DLLs"
    VERBATIM)
endif()

# --- unit tests (opt-in: -DDESCRY_TESTS=ON, or build.ps1 -Tests) --------
# Pure-C tests with no SDL/window dependency: tabs.c/buffer.c round-trips and
# tab-strip scroll math, the hoswl.h (Hisashi menubar client) DSL compiler /
# click parser, the icon_raster.c SVG -> RGBA pipeline (supersampling,
# knockouts), and the reveal.c file-manager command builders.
option(DESCRY_TESTS "Build unit tests" OFF)
if(DESCRY_TESTS)
  enable_testing()
  add_executable(test_tabs tests/test_tabs.c src/tabs.c src/buffer.c)
  add_executable(test_hoswl tests/test_hoswl.c)
  add_executable(test_icons tests/test_icons.c src/icon_raster.c)
  add_executable(test_reveal tests/test_reveal.c src/reveal.c)
  add_executable(test_markdown tests/test_markdown.c src/markdown.c)
  add_executable(test_graph tests/test_graph.c src/graph.c)
  add_executable(test_inputfield tests/test_inputfield.c src/inputfield.c)
  target_link_libraries(test_icons PRIVATE m)
  target_link_libraries(test_markdown PRIVATE md4c)
  target_link_libraries(test_graph PRIVATE m)
  foreach(t test_tabs test_hoswl test_icons test_reveal test_markdown test_graph
            test_inputfield)
    target_include_directories(${t} PRIVATE src vendor/nanosvg)
    # The tests are written with assert(); Release adds -DNDEBUG, which would
    # compile every check out and leave a test that passes by doing nothing.
    # -U comes after the config flags on the command line, so it wins.
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${t} PRIVATE -UNDEBUG)
    endif()
  endforeach()
  add_test(NAME tabs COMMAND test_tabs)
  add_test(NAME hoswl COMMAND test_hoswl)
  add_test(NAME icons COMMAND test_icons)
  add_test(NAME reveal COMMAND test_reveal)
  add_test(NAME markdown COMMAND test_markdown)
  add_test(NAME graph COMMAND test_graph)
  add_test(NAME inputfield COMMAND test_inputfield)
endif()
