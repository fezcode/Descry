#!/usr/bin/env bash
#
# Configure and build Descry (build/descry) with CMake + Ninja on macOS/Linux.
# The macOS/Linux counterpart of build.ps1. Wraps the documented build:
#
#     cmake -G Ninja -B build
#     ninja -C build
#
# On macOS it auto-points pkg-config at Homebrew's keg-only libjpeg so the
# cmake configure step finds SDL2 / FreeType / HarfBuzz / libpng / libjpeg
# even from a vanilla shell.

set -euo pipefail

usage() {
    cat <<'EOF'
Build Descry (build/descry) with CMake + Ninja on macOS/Linux.

Usage: ./build.sh [--clean] [--run] [--config <type>]

  --clean          Remove the build directory before configuring.
  --run            Launch build/descry after a successful build.
  --config <type>  CMAKE_BUILD_TYPE: Release (default), Debug,
                   RelWithDebInfo, MinSizeRel.
  -h, --help       Show this help.
EOF
}

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$root/build"

config="Release"
clean=0
run=0

while [ $# -gt 0 ]; do
    case "$1" in
        --clean)    clean=1; shift ;;
        --run)      run=1; shift ;;
        --config)   config="${2:?--config needs a value}"; shift 2 ;;
        --config=*) config="${1#*=}"; shift ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "build.sh: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# --- toolchain check ------------------------------------------------------
for tool in cmake ninja pkg-config; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "build.sh: required tool '$tool' not found on PATH." >&2
        echo "  macOS:  brew install cmake ninja pkg-config sdl2 freetype harfbuzz libpng jpeg" >&2
        echo "  Linux:  apt install cmake ninja-build pkg-config libsdl2-dev libfreetype-dev libharfbuzz-dev libpng-dev libjpeg-dev" >&2
        exit 1
    fi
done

# --- macOS: make keg-only Homebrew libjpeg visible to pkg-config ----------
if [ "$(uname -s)" = "Darwin" ] && command -v brew >/dev/null 2>&1; then
    brew_prefix="$(brew --prefix)"
    extra_pc="$brew_prefix/opt/jpeg/lib/pkgconfig:$brew_prefix/lib/pkgconfig"
    export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:+$PKG_CONFIG_PATH:}$extra_pc"
fi

echo "Config    : $config"
echo "Build dir : $build_dir"

# --- clean ----------------------------------------------------------------
if [ "$clean" -eq 1 ] && [ -d "$build_dir" ]; then
    echo "Cleaning $build_dir ..."
    rm -rf "$build_dir"
fi

# --- configure (only when there is no cache; Ninja is single-config, so a
#     -config change on an existing tree needs --clean) -------------------
if [ ! -f "$build_dir/CMakeCache.txt" ]; then
    echo "Configuring (cmake -G Ninja) ..."
    cmake -G Ninja -B "$build_dir" -S "$root" "-DCMAKE_BUILD_TYPE=$config"
else
    echo "Reusing CMake cache (pass --clean to reconfigure)."
fi

# --- build ----------------------------------------------------------------
echo "Building (ninja) ..."
cmake --build "$build_dir"

exe="$build_dir/descry"
if [ ! -x "$exe" ]; then
    echo "build.sh: build succeeded but $exe is missing." >&2
    exit 1
fi
echo "Built: $exe"

# --- optional run ---------------------------------------------------------
if [ "$run" -eq 1 ]; then
    echo "Launching descry ..."
    "$exe"
fi
