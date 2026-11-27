#!/usr/bin/env bash
#
# Build a self-contained, double-clickable Descry.app on macOS — and optionally
# a drag-to-Applications .dmg and/or a .zip of the bundle. The macOS
# counterpart of build_installer.ps1.
#
# It assembles dist/macos-arm64/Descry.app with:
#   - the descry binary (Contents/MacOS/descry)
#   - every linked dylib copied inside and re-pathed (Contents/Frameworks),
#     via dylibbundler, so the app runs without Homebrew installed
#   - an Info.plist (NSHighResolutionCapable = true for crisp Retina)
#   - a Descry.icns generated from resources/icon_*.png
#   - an ad-hoc code signature so Gatekeeper lets it launch locally
#
# Usage: ./package_macos.sh [--dmg] [--zip] [--no-build] [--sign "Developer ID App: …"]

set -euo pipefail

usage() {
    cat <<'EOF'
Build a self-contained Descry.app (and optionally a .dmg / .zip) on macOS.

Usage: ./package_macos.sh [--dmg] [--zip] [--no-build] [--sign <identity>]

  --dmg              Also produce dist/Descry-<version>.dmg (drag-to-Applications).
  --zip              Also produce dist/Descry-<version>-macos-arm64.zip (bare
                     app bundle, zipped with ditto so the signature survives).
  --no-build         Skip build.sh; reuse the existing build/descry.
  --sign <identity>  Code-sign with a Developer ID instead of ad-hoc ("-").
                     For distribution you still need to notarize separately.
  -h, --help         Show this help.
EOF
}

do_dmg=0
do_zip=0
do_build=1
sign_id="-"          # ad-hoc by default

while [ $# -gt 0 ]; do
    case "$1" in
        --dmg)      do_dmg=1; shift ;;
        --zip)      do_zip=1; shift ;;
        --no-build) do_build=0; shift ;;
        --sign)     sign_id="${2:?--sign needs an identity}"; shift 2 ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "package_macos.sh: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [ "$(uname -s)" != "Darwin" ]; then
    echo "package_macos.sh: this must run on macOS." >&2
    exit 1
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$root"

# --- version (single source of truth: src/version.h) ---------------------
version="$(sed -n 's/.*#define DESCRY_VERSION "\([0-9.]*\)".*/\1/p' src/version.h | head -n1)"
[ -n "$version" ] || { echo "could not read DESCRY_VERSION from src/version.h" >&2; exit 1; }
echo "Descry version : $version"

# --- build ---------------------------------------------------------------
if [ "$do_build" -eq 1 ]; then
    echo "Building (build.sh) ..."
    ./build.sh
fi
[ -x build/descry ] || { echo "build/descry missing — run without --no-build." >&2; exit 1; }

# --- assemble the .app skeleton ------------------------------------------
# dist/ mirrors the Windows installer layout (and timp/tivi's bundle scripts).
app="dist/macos-arm64/Descry.app"
mkdir -p dist/macos-arm64
echo "Assembling $app ..."
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$app/Contents/Frameworks"
cp build/descry "$app/Contents/MacOS/descry"
chmod +x "$app/Contents/MacOS/descry"

# --- Info.plist ----------------------------------------------------------
cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>                <string>Descry</string>
    <key>CFBundleDisplayName</key>         <string>Descry</string>
    <key>CFBundleExecutable</key>          <string>descry</string>
    <key>CFBundleIdentifier</key>          <string>com.fezcode.descry</string>
    <key>CFBundleVersion</key>             <string>${version}</string>
    <key>CFBundleShortVersionString</key>  <string>${version}</string>
    <key>CFBundlePackageType</key>         <string>APPL</string>
    <key>CFBundleIconFile</key>            <string>Descry</string>
    <key>CFBundleInfoDictionaryVersion</key> <string>6.0</string>
    <key>NSHighResolutionCapable</key>     <true/>
    <key>LSMinimumSystemVersion</key>      <string>11.0</string>
    <key>NSHumanReadableCopyright</key>    <string>Fezcode</string>
</dict>
</plist>
PLIST

# --- icon: resources/icon_*.png -> Descry.icns ---------------------------
if command -v iconutil >/dev/null 2>&1; then
    echo "Building Descry.icns ..."
    iconset="$(mktemp -d)/Descry.iconset"
    mkdir -p "$iconset"
    cp resources/icon_16.png  "$iconset/icon_16x16.png"
    cp resources/icon_32.png  "$iconset/icon_16x16@2x.png"
    cp resources/icon_32.png  "$iconset/icon_32x32.png"
    cp resources/icon_64.png  "$iconset/icon_32x32@2x.png"
    cp resources/icon_128.png "$iconset/icon_128x128.png"
    cp resources/icon_256.png "$iconset/icon_128x128@2x.png"
    cp resources/icon_256.png "$iconset/icon_256x256.png"
    # The largest source PNG is 256; upscale for the @2x / 512 slots.
    sips -z 512  512  resources/icon_256.png --out "$iconset/icon_256x256@2x.png" >/dev/null
    sips -z 512  512  resources/icon_256.png --out "$iconset/icon_512x512.png"     >/dev/null
    sips -z 1024 1024 resources/icon_256.png --out "$iconset/icon_512x512@2x.png"  >/dev/null
    iconutil -c icns "$iconset" -o "$app/Contents/Resources/Descry.icns"
else
    echo "warning: iconutil not found — app will use the default icon." >&2
fi

# --- bundle the dylibs (self-contained) ----------------------------------
if command -v dylibbundler >/dev/null 2>&1; then
    echo "Bundling dylibs (dylibbundler) ..."
    dylibbundler -cd -of -b \
        -x "$app/Contents/MacOS/descry" \
        -d "$app/Contents/Frameworks/" \
        -p "@executable_path/../Frameworks/"

    frameworks="$app/Contents/Frameworks"

    # Homebrew's "sdl2" is sdl2-compat: a thin libSDL2 that dlopen()s libSDL3
    # at startup. dylibbundler only follows link-time dependencies, so it
    # never sees SDL3 and the app dies with "SDL3 not found" on any Mac
    # without Homebrew. Carry SDL3 along by hand; the shim looks for
    # @loader_path/libSDL3.dylib first, which is this Frameworks folder.
    sdl2_lib="$(ls "$frameworks"/libSDL2*.dylib 2>/dev/null | head -n1 || true)"
    if [ -n "$sdl2_lib" ] && grep -aq 'libSDL3' "$sdl2_lib"; then
        sdl3_src=""
        for d in "$(brew --prefix sdl3 2>/dev/null || true)/lib" \
                 "$(pkg-config --variable=libdir sdl3 2>/dev/null || true)" \
                 /opt/homebrew/lib /usr/local/lib; do
            if [ -e "$d/libSDL3.0.dylib" ]; then sdl3_src="$d/libSDL3.0.dylib"; break; fi
        done
        if [ -z "$sdl3_src" ]; then
            echo "package_macos.sh: libSDL2 is sdl2-compat but libSDL3.0.dylib was not found." >&2
            echo "                  brew install sdl3" >&2
            exit 1
        fi
        echo "Bundling SDL3 for sdl2-compat ($sdl3_src) ..."
        cp -L "$sdl3_src" "$frameworks/libSDL3.0.dylib"
        chmod u+w "$frameworks/libSDL3.0.dylib"
        install_name_tool -id "@executable_path/../Frameworks/libSDL3.0.dylib" \
            "$frameworks/libSDL3.0.dylib"
        ln -sf libSDL3.0.dylib "$frameworks/libSDL3.dylib"
        # Pull in (and re-path) anything SDL3 itself links against.
        dylibbundler -of -b \
            -x "$frameworks/libSDL3.0.dylib" \
            -d "$frameworks/" \
            -p "@executable_path/../Frameworks/"
    fi

    # --- verify: nothing may point outside the bundle or the OS ----------
    echo "Verifying the bundle is self-contained ..."
    bad=0
    minos=""
    for f in "$app/Contents/MacOS/descry" "$frameworks"/*.dylib; do
        [ -L "$f" ] && continue
        while IFS= read -r dep; do
            case "$dep" in
                /usr/lib/*|/System/Library/*) ;;
                @executable_path/../Frameworks/*)
                    if [ ! -e "$frameworks/${dep#@executable_path/../Frameworks/}" ]; then
                        echo "  missing from bundle: $dep (needed by $(basename "$f"))" >&2; bad=1
                    fi ;;
                @loader_path/*|@rpath/*)
                    if [ ! -e "$frameworks/$(basename "$dep")" ]; then
                        echo "  missing from bundle: $dep (needed by $(basename "$f"))" >&2; bad=1
                    fi ;;
                *)  echo "  outside the bundle: $dep (needed by $(basename "$f"))" >&2; bad=1 ;;
            esac
        done < <(otool -L "$f" | tail -n +2 | awk '{print $1}')
        while IFS= read -r rp; do
            case "$rp" in
                @*|/usr/lib*|/System/Library/*) ;;
                *)  echo "  rpath outside the bundle: $rp (in $(basename "$f"))" >&2; bad=1 ;;
            esac
        done < <(otool -l "$f" | awk '/LC_RPATH/{r=1} r && $1=="path"{print $2; r=0}')
        m="$(otool -l "$f" | awk '/LC_BUILD_VERSION/{b=1} b && $1=="minos"{print $2; b=0}' | head -n1)"
        if [ -n "$m" ]; then
            minos="$(printf '%s\n%s\n' "$minos" "$m" | sort -t. -k1,1n -k2,2n | tail -n1)"
        fi
    done
    if [ "$bad" -ne 0 ]; then
        echo "package_macos.sh: the app is not self-contained (see above)." >&2
        exit 1
    fi

    # Prebuilt Homebrew libraries target the macOS they were built on. State
    # the real minimum so older systems get Finder's clear "requires macOS N"
    # instead of a crash at launch.
    if [ -n "$minos" ]; then
        /usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion $minos" \
            "$app/Contents/Info.plist"
        echo "Minimum macOS: $minos"
    fi
else
    echo "warning: dylibbundler not found — the .app will still depend on" >&2
    echo "         Homebrew dylibs. Install it for a self-contained app:"  >&2
    echo "             brew install dylibbundler"                            >&2
fi

# --- code sign (ad-hoc by default so it launches locally) ----------------
echo "Code-signing ($([ "$sign_id" = "-" ] && echo ad-hoc || echo "$sign_id")) ..."
codesign --force --deep --sign "$sign_id" "$app"

echo "Built: $app"

# --- optional .dmg -------------------------------------------------------
if [ "$do_dmg" -eq 1 ]; then
    command -v hdiutil >/dev/null 2>&1 || { echo "hdiutil missing" >&2; exit 1; }
    mkdir -p dist
    dmg="dist/Descry-${version}.dmg"
    echo "Building $dmg ..."
    stage="$(mktemp -d)"
    cp -R "$app" "$stage/"
    ln -s /Applications "$stage/Applications"
    rm -f "$dmg"
    hdiutil create -volname "Descry ${version}" -srcfolder "$stage" \
        -ov -format UDZO "$dmg" >/dev/null
    echo "Built: $dmg"
fi

# --- optional .zip -------------------------------------------------------
# ditto (not zip) so the bundle's symlinks and code signature survive the
# round-trip; --keepParent keeps Descry.app as the archive's top-level entry.
if [ "$do_zip" -eq 1 ]; then
    mkdir -p dist
    # Arch from the bundled binary, not uname -m (which reports x86_64 under
    # Rosetta), so the name always matches what is actually inside.
    zip_arch="$(lipo -archs "$app/Contents/MacOS/descry" 2>/dev/null | tr ' ' '-')"
    [ -n "$zip_arch" ] || zip_arch="arm64"
    zipfile="dist/Descry-${version}-macos-${zip_arch}.zip"
    echo "Building $zipfile ..."
    rm -f "$zipfile"
    ditto -c -k --sequesterRsrc --keepParent "$app" "$zipfile"
    echo "Built: $zipfile"
fi

echo "Done. Double-click $app, or drag it to /Applications."
