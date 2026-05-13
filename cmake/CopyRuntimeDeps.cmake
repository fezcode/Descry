# Copies non-system runtime DLL dependencies of EXE next to OUT_DIR. Run
# as `cmake -P` from a post-build step (see top-level CMakeLists.txt).
#
# Inputs (set via -D on the cmake -P invocation):
#   EXE         absolute path to the exe to scan
#   OUT_DIR     absolute path of the directory to copy DLLs into
#   MINGW_BIN   directory holding mingw64 DLLs (search hint + whitelist)
#
# What gets copied: only resolved DLLs whose path lives under MINGW_BIN.
# Everything else (Windows system DLLs in C:\Windows\System32 etc.) is
# excluded so we don't drag the OS libs into the bundle.

cmake_minimum_required(VERSION 3.20)

if(NOT EXE OR NOT OUT_DIR OR NOT MINGW_BIN)
    message(FATAL_ERROR
        "CopyRuntimeDeps: EXE, OUT_DIR, MINGW_BIN must all be set")
endif()

# Normalize to forward slashes so the regex match below works on Windows
# paths regardless of which separator GET_RUNTIME_DEPENDENCIES emits.
file(TO_CMAKE_PATH "${MINGW_BIN}" _mingw)
file(TO_CMAKE_PATH "${OUT_DIR}" _outdir)
string(REGEX REPLACE "/$" "" _mingw  "${_mingw}")
string(REGEX REPLACE "/$" "" _outdir "${_outdir}")

# Whitelist: include only DLLs whose resolved path lives in MINGW_BIN
# or in OUT_DIR. After the first build the DLLs sit next to the exe so
# the resolver may return OUT_DIR paths — without including OUT_DIR we'd
# match nothing on subsequent builds. Everything else (Windows system
# DLLs in C:/WINDOWS/... regardless of case) gets filtered out.
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES         "${EXE}"
    RESOLVED_DEPENDENCIES_VAR   _resolved
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    DIRECTORIES         "${_mingw}"
    PRE_EXCLUDE_REGEXES "^(api-ms|ext-ms)-.*"
    POST_INCLUDE_REGEXES "^${_mingw}/" "^${_outdir}/"
    POST_EXCLUDE_REGEXES ".*"
)

# After the first build the DLLs sit next to the exe, so the resolver
# may return paths inside OUT_DIR instead of MINGW_BIN. Re-derive the
# canonical source path from each dep's basename and copy from there
# so subsequent builds pick up upgrades from MSYS2.
set(_n 0)
foreach(_dep IN LISTS _resolved)
    get_filename_component(_name "${_dep}" NAME)
    set(_src "${_mingw}/${_name}")
    if(NOT EXISTS "${_src}")
        # Resolved to something outside the mingw bin and we can't find
        # a canonical copy — fall back to the resolved path itself.
        set(_src "${_dep}")
    endif()
    file(COPY "${_src}" DESTINATION "${OUT_DIR}")
    message(STATUS "  bundled: ${_name}")
    math(EXPR _n "${_n} + 1")
endforeach()

message(STATUS "CopyRuntimeDeps: ${_n} DLL(s) bundled into ${OUT_DIR}")

if(_unresolved)
    foreach(_u IN LISTS _unresolved)
        message(STATUS "  unresolved (skipped): ${_u}")
    endforeach()
endif()
