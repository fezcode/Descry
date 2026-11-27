#!/usr/bin/env bash
#
# Check that a Windows Descry folder is self-contained: every DLL that
# descry.exe or one of its DLLs imports must sit in that folder or be part
# of Windows itself. Run from an MSYS2 shell (needs objdump):
#
#     tools/verify_bundle_windows.sh build
#
# Exits non-zero and names the gaps if anything would have to come from
# PATH (an MSYS2 install, say) on the user's machine.

set -euo pipefail

dir="${1:?usage: verify_bundle_windows.sh <folder with descry.exe>}"
[ -f "$dir/descry.exe" ] || { echo "no descry.exe in $dir" >&2; exit 2; }
command -v objdump >/dev/null 2>&1 || { echo "objdump not found (run from MSYS2)" >&2; exit 2; }

sys32="$(cygpath -u "${SYSTEMROOT:-C:\\Windows}")/System32"
bad=0
n=0

shopt -s nullglob
for f in "$dir"/descry.exe "$dir"/*.dll; do
    n=$((n + 1))
    while IFS= read -r dep; do
        [ -n "$dep" ] || continue
        case "$dep" in api-ms-*|ext-ms-*|API-MS-*|EXT-MS-*) continue ;; esac
        [ -e "$dir/$dep" ] && continue
        [ -e "$sys32/$dep" ] && continue
        echo "  missing: $dep (imported by $(basename "$f"))" >&2
        bad=1
    done < <(objdump -p "$f" | sed -n 's/^[[:space:]]*DLL Name: //p' | tr -d '\r')
done

# sdl2-compat's SDL2.dll loads SDL3.dll at run time, which is not an import.
if [ -f "$dir/SDL2.dll" ] && grep -aq 'SDL3\.dll' "$dir/SDL2.dll" && [ ! -f "$dir/SDL3.dll" ]; then
    echo "  missing: SDL3.dll (SDL2.dll is sdl2-compat and loads it at startup)" >&2
    bad=1
fi

if [ "$bad" -ne 0 ]; then
    echo "verify_bundle_windows: $dir is NOT self-contained." >&2
    exit 1
fi
echo "verify_bundle_windows: $dir is self-contained ($n binaries checked)."
