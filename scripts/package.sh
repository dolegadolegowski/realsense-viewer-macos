#!/bin/bash
# Packages the build into self-contained macOS apps and a DMG:
#   dist/RealSense Viewer.app
#   dist/RealSense Depth Quality Tool.app
#   dist/RealSense Tools/          command line tools (rs-enumerate-devices, rs-fw-update, ...)
#   dist/RealSense-Viewer-<version>-macOS-arm64.dmg
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/librealsense"
OUT="${BUILD_DIR:-$ROOT/build}/${BUILD_TYPE:-Release}"
DIST="$ROOT/dist"
DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-27.0}"
BUNDLE_PREFIX="${BUNDLE_PREFIX:-io.github.dolegadolegowski}"

[ -x "$OUT/realsense-viewer" ] || { echo "error: run scripts/build.sh first" >&2; exit 1; }

VERSION="$(sed -n 's/^#define RS2_API_MAJOR_VERSION *\([0-9]*\).*/\1/p' "$SRC/include/librealsense2/rs.h").$(sed -n 's/^#define RS2_API_MINOR_VERSION *\([0-9]*\).*/\1/p' "$SRC/include/librealsense2/rs.h").$(sed -n 's/^#define RS2_API_PATCH_VERSION *\([0-9]*\).*/\1/p' "$SRC/include/librealsense2/rs.h")"
LIBS=(librealsense2.2.58.dylib librealsense2-gl.2.58.dylib libglfw.3.dylib)
TOOLS=(rs-enumerate-devices rs-fw-update rs-fw-logger rs-terminal rs-data-collect rs-convert rs-record rs-embed
       rs-on-chip-calib rs-rosbag-inspector rs-benchmark rs-macos-selftest rs-macos-release
       rs-hello-realsense rs-capture rs-depth rs-color rs-infrared rs-motion rs-pointcloud rs-align
       rs-align-advanced rs-multicam rs-measure rs-post-processing rs-record-playback rs-save-to-disk
       rs-sensor-control rs-distance rs-callback rs-hdr)

rm -rf "$DIST"
mkdir -p "$DIST"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Application icon from the librealsense artwork
ICONSET="$WORK/AppIcon.iconset"
mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
    sips -z $s $s "$SRC/common/res/icon_512.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    sips -z $((s * 2)) $((s * 2)) "$SRC/common/res/icon_512.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$WORK/AppIcon.icns"

# Launcher (see macos/launcher/main.swift)
swiftc -O -target "arm64-apple-macos$DEPLOYMENT_TARGET" "$ROOT/macos/launcher/main.swift" -o "$WORK/launcher"

# Points a Mach-O at the bundled libraries instead of the build tree
relink() {
    local file="$1" rpath="$2"
    otool -l "$file" | awk '/LC_RPATH/{getline; getline; print $2}' | while read -r old; do
        install_name_tool -delete_rpath "$old" "$file" 2>/dev/null || true
    done
    install_name_tool -add_rpath "$rpath" "$file"
}

copy_libs() {
    local dest="$1"
    mkdir -p "$dest"
    for lib in "${LIBS[@]}"; do
        cp -L "$OUT/$lib" "$dest/$lib"
        chmod 644 "$dest/$lib"
        install_name_tool -id "@rpath/$lib" "$dest/$lib"
        relink "$dest/$lib" "@loader_path"
    done
}

make_app() {
    local name="$1" target="$2" id="$3" summary="$4"
    local app="$DIST/$name.app"
    mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
    cp "$WORK/launcher" "$app/Contents/MacOS/$name"
    cp "$OUT/$target" "$app/Contents/MacOS/$target"
    cp "$OUT/rs-macos-release" "$app/Contents/MacOS/rs-macos-release"
    cp "$OUT/rs-macos-handback" "$app/Contents/MacOS/rs-macos-handback"
    relink "$app/Contents/MacOS/$target" "@executable_path/../Frameworks"
    copy_libs "$app/Contents/Frameworks"
    cp "$WORK/AppIcon.icns" "$app/Contents/Resources/AppIcon.icns"
    cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>                 <string>$name</string>
    <key>CFBundleDisplayName</key>          <string>$name</string>
    <key>CFBundleIdentifier</key>           <string>$BUNDLE_PREFIX.$id</string>
    <key>CFBundleExecutable</key>           <string>$name</string>
    <key>CFBundleIconFile</key>             <string>AppIcon</string>
    <key>CFBundlePackageType</key>          <string>APPL</string>
    <key>CFBundleShortVersionString</key>   <string>$VERSION</string>
    <key>CFBundleVersion</key>              <string>$VERSION</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleGetInfoString</key>        <string>$summary</string>
    <key>LSMinimumSystemVersion</key>       <string>$DEPLOYMENT_TARGET</string>
    <key>LSArchitecturePriority</key>       <array><string>arm64</string></array>
    <key>LSApplicationCategoryType</key>    <string>public.app-category.developer-tools</string>
    <key>LSUIElement</key>                  <true/>
    <key>NSHighResolutionCapable</key>      <true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
    <key>NSHumanReadableCopyright</key>     <string>librealsense: Copyright RealSense, Inc. (Apache 2.0). macOS port: Apache 2.0.</string>
    <key>RSLaunchTarget</key>               <string>$target</string>
</dict>
</plist>
PLIST
    # ad-hoc signature (Apple Silicon requires every Mach-O to be signed)
    find "$app/Contents" -type f \( -name "*.dylib" -o -perm -u+x \) -exec codesign --force --sign - {} \; 2>/dev/null
    codesign --force --sign - "$app"
    echo "created $app"
}

make_app "RealSense Viewer" realsense-viewer realsense-viewer "RealSense Viewer $VERSION for Apple Silicon"
make_app "RealSense Depth Quality Tool" rs-depth-quality realsense-depth-quality "RealSense Depth Quality Tool $VERSION for Apple Silicon"

# Command line tools (run with sudo)
TOOLS_DIR="$DIST/RealSense Tools"
mkdir -p "$TOOLS_DIR/bin"
copy_libs "$TOOLS_DIR/lib"
for t in "${TOOLS[@]}"; do
    [ -x "$OUT/$t" ] || continue
    cp "$OUT/$t" "$TOOLS_DIR/bin/$t"
    relink "$TOOLS_DIR/bin/$t" "@executable_path/../lib"
done
find "$TOOLS_DIR" -type f -exec codesign --force --sign - {} \; 2>/dev/null
cat > "$TOOLS_DIR/README.txt" <<TXT
RealSense command line tools $VERSION for Apple Silicon macOS.

macOS only lets root take the camera over from the system USB video driver, so run them with sudo, e.g.:
    sudo ./bin/rs-enumerate-devices
    sudo ./bin/rs-macos-selftest
    sudo ./bin/rs-fw-update -l

After a tool exits the camera stays detached from macOS; "sudo ./bin/rs-macos-release" (or re-plugging it)
gives it back to macOS, e.g. to use the RGB camera as a webcam.
TXT

# Disk image
STAGE="$WORK/dmg"
mkdir -p "$STAGE"
cp -R "$DIST/RealSense Viewer.app" "$DIST/RealSense Depth Quality Tool.app" "$TOOLS_DIR" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
xattr -cr "$STAGE" 2>/dev/null || true   # no machine-specific extended attributes in the image
DMG="$DIST/RealSense-Viewer-$VERSION-macOS-arm64.dmg"
hdiutil create -volname "RealSense Viewer $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
echo "created $DMG"
