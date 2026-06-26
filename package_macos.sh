#!/usr/bin/env bash
#
# Build a self-contained, double-clickable Descry.app on macOS — and optionally
# a drag-to-Applications .dmg. The macOS counterpart of build_installer.ps1.
#
# It assembles build/Descry.app with:
#   - the descry binary (Contents/MacOS/descry)
#   - every linked dylib copied inside and re-pathed (Contents/Frameworks),
#     via dylibbundler, so the app runs without Homebrew installed
#   - an Info.plist (NSHighResolutionCapable = true for crisp Retina)
#   - a Descry.icns generated from resources/icon_*.png
#   - an ad-hoc code signature so Gatekeeper lets it launch locally
#
# Usage: ./package_macos.sh [--dmg] [--no-build] [--sign "Developer ID App: …"]

set -euo pipefail

usage() {
    cat <<'EOF'
Build a self-contained Descry.app (and optionally a .dmg) on macOS.

Usage: ./package_macos.sh [--dmg] [--no-build] [--sign <identity>]

  --dmg              Also produce dist/Descry-<version>.dmg (drag-to-Applications).
  --no-build         Skip build.sh; reuse the existing build/descry.
  --sign <identity>  Code-sign with a Developer ID instead of ad-hoc ("-").
                     For distribution you still need to notarize separately.
  -h, --help         Show this help.
EOF
}

do_dmg=0
do_build=1
sign_id="-"          # ad-hoc by default

while [ $# -gt 0 ]; do
    case "$1" in
        --dmg)      do_dmg=1; shift ;;
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

# --- version (single source of truth: src/main.c) ------------------------
version="$(sed -n 's/.*#define DESCRY_VERSION "\([0-9.]*\)".*/\1/p' src/main.c | head -n1)"
[ -n "$version" ] || { echo "could not read DESCRY_VERSION from src/main.c" >&2; exit 1; }
echo "Descry version : $version"

# --- build ---------------------------------------------------------------
if [ "$do_build" -eq 1 ]; then
    echo "Building (build.sh) ..."
    ./build.sh
fi
[ -x build/descry ] || { echo "build/descry missing — run without --no-build." >&2; exit 1; }

# --- assemble the .app skeleton ------------------------------------------
app="build/Descry.app"
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

echo "Done. Double-click $app, or drag it to /Applications."
