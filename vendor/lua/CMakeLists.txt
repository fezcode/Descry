file(GLOB LUA_SOURCES src/*.c)
list(REMOVE_ITEM LUA_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/lua.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/luac.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/onelua.c)

add_library(lua_static STATIC ${LUA_SOURCES})
target_include_directories(lua_static PUBLIC src)

# luaconf.h auto-detects _WIN32; only force the macro on platforms that
# need package.loadlib (dlopen) wired up explicitly.
if(APPLE)
  target_compile_definitions(lua_static PRIVATE LUA_USE_MACOSX)
elseif(UNIX AND NOT WIN32)
  target_compile_definitions(lua_static PRIVATE LUA_USE_LINUX)
endif()

set_target_properties(lua_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
