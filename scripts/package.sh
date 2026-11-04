#!/bin/bash
# Packages the build into self-contained macOS apps and a DMG:
#   dist/RealSense Viewer.app
#   dist/RealSense Depth Quality Tool.app
#   dist/RealSense Tools/          command line tools (rs-enumerate-devices, rs-fw-update, ...) and camera-access/
#   dist/RealSense-Viewer-<version>-macOS-arm64.dmg
# Every app and the Tools folder carry the USB broker realsense-usbd, its installer and the allowlist of this build
# (macos/broker/DESIGN.md 8.2, 8.3).
#
#   CLIENT_RUNTIME=1 scripts/package.sh   signs the viewer and the Depth Quality Tool with the hardened runtime
#                                         (DESIGN 3.7.4; only after the HW-3 soak passed with it)
set -euo pipefail
# The installer and the launcher refuse group/world-writable bundle directories (DESIGN 6.2)
umask 022

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/librealsense"
OUT="${BUILD_DIR:-$ROOT/build}/${BUILD_TYPE:-Release}"
DIST="$ROOT/dist"
DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-27.0}"
BUNDLE_PREFIX="${BUNDLE_PREFIX:-io.github.dolegadolegowski}"
USBD_ID=io.github.dolegadolegowski.realsense-usbd

[ -x "$OUT/realsense-viewer" ] || { echo "error: run scripts/build.sh first" >&2; exit 1; }
[ -x "$OUT/realsense-usbd" ] || { echo "error: $OUT/realsense-usbd missing, run scripts/build.sh first" >&2; exit 1; }

VERSION="$(sed -n 's/^#define RS2_API_MAJOR_VERSION *\([0-9]*\).*/\1/p' "$SRC/include/librealsense2/rs.h").$(sed -n 's/^#define RS2_API_MINOR_VERSION *\([0-9]*\).*/\1/p' "$SRC/include/librealsense2/rs.h").$(sed -n 's/^#define RS2_API_PATCH_VERSION *\([0-9]*\).*/\1/p' "$SRC/include/librealsense2/rs.h")"
PROTOCOL_VERSION="$(sed -n 's/^#define RSUSB_PROTOCOL_VERSION \([0-9]*\)u*$/\1/p' "$ROOT/macos/broker/rs-usb-broker-protocol.h")"
[ -n "$PROTOCOL_VERSION" ] || { echo "error: RSUSB_PROTOCOL_VERSION not found" >&2; exit 1; }
LIBS=(librealsense2.2.58.dylib librealsense2-gl.2.58.dylib libglfw.3.dylib)
TOOLS=(rs-enumerate-devices rs-fw-update rs-fw-logger rs-terminal rs-data-collect rs-convert rs-record rs-embed
       rs-on-chip-calib rs-rosbag-inspector rs-benchmark rs-macos-selftest rs-macos-usbdump rs-macos-release
       rs-hello-realsense rs-capture rs-depth rs-color rs-infrared rs-motion rs-pointcloud rs-align
       rs-align-advanced rs-multicam rs-measure rs-post-processing rs-record-playback rs-save-to-disk
       rs-sensor-control rs-distance rs-callback rs-hdr)
# Tools that may use the camera through the broker (DESIGN 8.3.3). Not listed, so still sudo: rs-terminal (raw
# hardware-monitor commands), tools that open no device, rs-macos-release.
BROKER_TOOLS=(rs-macos-selftest rs-macos-usbdump rs-enumerate-devices rs-fw-update rs-fw-logger rs-data-collect
              rs-record rs-on-chip-calib rs-benchmark rs-hello-realsense rs-capture rs-depth rs-color rs-infrared
              rs-motion rs-pointcloud rs-align rs-align-advanced rs-multicam rs-measure rs-post-processing
              rs-record-playback rs-save-to-disk rs-sensor-control rs-distance rs-callback rs-hdr)
BROKER_SCRIPTS=(install-camera-access.sh uninstall-camera-access.sh)

# cdhash_of <Mach-O>: the arm64 code directory hash the daemon admits (40 lowercase hex)
cdhash_of() {
    local h
    h="$(codesign -dvvv --arch arm64 "$1" 2>&1 | sed -n 's/^CDHash=//p')"
    [[ "$h" =~ ^[0-9a-f]{40}$ ]] || { echo "error: no arm64 cdhash for $1" >&2; return 1; }
    echo "$h"
}

# write_allowlist <out.plist> <build number> <daemon> <client>...: DESIGN 5.1, records "<cdhash> <file>" lines in
# $CDHASH_RECORD for the check after bundle signing
write_allowlist() {
    local out="$1" number="$2" daemon="$3" f h daemon_hash sorted build_id
    shift 3
    daemon_hash="$(cdhash_of "$daemon")"
    : >"$CDHASH_RECORD"
    for f in "$daemon" "$@"; do
        h="$(cdhash_of "$f")"
        echo "$h $f" >>"$CDHASH_RECORD"
    done
    sorted="$(cut -d' ' -f1 "$CDHASH_RECORD" | LC_ALL=C sort -u)"
    [ "$(grep -c . <<<"$sorted")" -le 256 ] || { echo "error: more than 256 allowlisted cdhashes" >&2; return 1; }
    build_id="$(printf '%s' "$sorted" | shasum -a 256 | cut -d' ' -f1)"
    rm -f "$out"
    plutil -create xml1 "$out"
    plutil -insert Version -integer 1 "$out"
    plutil -insert BuildID -string "$build_id" "$out"
    plutil -insert BuildNumber -integer "$number" "$out"
    plutil -insert ProtocolVersion -integer "$PROTOCOL_VERSION" "$out"
    plutil -insert AllowedCDHashes -array "$out"
    while read -r h; do plutil -insert AllowedCDHashes -string "$h" -append "$out"; done <<<"$sorted"
    plutil -insert DaemonCDHash -string "$daemon_hash" "$out"
    chmod 644 "$out"
}

# verify_cdhashes: nothing allowlisted changed since write_allowlist
verify_cdhashes() {
    local h f
    while read -r h f; do
        [ "$(cdhash_of "$f")" = "$h" ] || { echo "error: cdhash of $f changed after the allowlist was written" >&2; return 1; }
    done <"$CDHASH_RECORD"
}
# end of allowlist functions

rm -rf "$DIST"
mkdir -p "$DIST"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
CDHASH_RECORD="$WORK/cdhashes.txt"

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

# realsense-usbd and the camera access scripts (DESIGN 8.2); the allowlist follows after signing
copy_broker() {
    local bin_dir="$1" res_dir="$2"
    cp "$OUT/realsense-usbd" "$bin_dir/realsense-usbd"
    chmod 755 "$bin_dir/realsense-usbd"
    mkdir -p "$res_dir"
    for s in "${BROKER_SCRIPTS[@]}"; do
        cp "$ROOT/macos/broker/$s" "$res_dir/$s"
        chmod 755 "$res_dir/$s"
    done
}

# 1. Assemble both apps and the Tools folder, unsigned
make_app() {
    local name="$1" target="$2" id="$3" summary="$4"
    local app="$DIST/$name.app"
    mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
    cp "$WORK/launcher" "$app/Contents/MacOS/$name"
    cp "$OUT/$target" "$app/Contents/MacOS/$target"
    cp "$OUT/rs-macos-release" "$app/Contents/MacOS/rs-macos-release"
    cp "$OUT/rs-macos-handback" "$app/Contents/MacOS/rs-macos-handback"
    copy_broker "$app/Contents/MacOS" "$app/Contents/Resources/realsense-usbd"
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
}

VIEWER_APP="$DIST/RealSense Viewer.app"
DQT_APP="$DIST/RealSense Depth Quality Tool.app"
make_app "RealSense Viewer" realsense-viewer realsense-viewer "RealSense Viewer $VERSION for Apple Silicon"
make_app "RealSense Depth Quality Tool" rs-depth-quality realsense-depth-quality "RealSense Depth Quality Tool $VERSION for Apple Silicon"

TOOLS_DIR="$DIST/RealSense Tools"
mkdir -p "$TOOLS_DIR/bin" "$TOOLS_DIR/camera-access"
copy_libs "$TOOLS_DIR/lib"
for t in "${TOOLS[@]}"; do
    [ -x "$OUT/$t" ] || continue
    cp "$OUT/$t" "$TOOLS_DIR/bin/$t"
    relink "$TOOLS_DIR/bin/$t" "@executable_path/../lib"
done
copy_broker "$TOOLS_DIR/camera-access" "$TOOLS_DIR/camera-access"
cat > "$TOOLS_DIR/README.txt" <<TXT
RealSense command line tools $VERSION for Apple Silicon macOS.

Camera access (recommended)
---------------------------
macOS only lets root take a camera over from its USB video driver. "Camera access" is a small system service
(realsense-usbd) that does this for the RealSense Viewer, the Depth Quality Tool and the tools below, so they run
without sudo and without a password, also on standard accounts. An administrator installs it once, either

  * by opening RealSense Viewer (copied to /Applications) and clicking "Install camera access for all users", or
  * from this folder:  sudo "./camera-access/install-camera-access.sh" --source "./camera-access"
    (run it from a copy in an administrator's folder or from the DMG an administrator opened; it refuses files
    other users could have changed).

Afterwards no sudo and no password are needed, e.g.:
    ./bin/rs-enumerate-devices
    ./bin/rs-macos-selftest
    ./bin/rs-fw-update -l

The apps and these tools must come from the same DMG: camera access only admits the programs of the build it
was installed from. Installing camera access from a newer DMG replaces it; then update the apps as well.
To remove it:  sudo "./camera-access/uninstall-camera-access.sh"

Without camera access
---------------------
Run the tools with sudo, e.g. "sudo ./bin/rs-enumerate-devices". rs-terminal always needs sudo. After a tool ran
with sudo the camera stays detached from macOS; "sudo ./bin/rs-macos-release" (or re-plugging it) gives it back
to macOS, e.g. to use the RGB camera as a webcam.
TXT

# 2. Sign every nested Mach-O (ad-hoc; the daemon with the hardened runtime and its fixed identifier)
cat > "$WORK/client.entitlements" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.cs.disable-library-validation</key><true/></dict></plist>
PLIST
sign_nested() {
    local f
    while IFS= read -r -d '' f; do
        case "$(file -b "$f")" in Mach-O*) ;; *) continue ;; esac
        local opts=()
        case "$(basename "$f")" in
            realsense-usbd) opts=(-o runtime -i "$USBD_ID") ;;
            realsense-viewer | rs-depth-quality)
                if [ "${CLIENT_RUNTIME:-0}" = 1 ]; then opts=(-o runtime --entitlements "$WORK/client.entitlements"); fi ;;
        esac
        codesign --force --sign - ${opts[@]+"${opts[@]}"} "$f" 2>/dev/null || { echo "error: cannot sign $f" >&2; return 1; }
    done < <(find "$1" -type f \( -name "*.dylib" -o -perm -u+x \) -print0)
}
sign_nested "$VIEWER_APP/Contents"
sign_nested "$DQT_APP/Contents"
sign_nested "$TOOLS_DIR"

# 3./4. Allowlist of this build, identical in both apps and the Tools folder
CLIENTS=("$VIEWER_APP/Contents/MacOS/realsense-viewer" "$DQT_APP/Contents/MacOS/rs-depth-quality"
         "$DQT_APP/Contents/MacOS/realsense-usbd" "$TOOLS_DIR/camera-access/realsense-usbd")
for t in "${BROKER_TOOLS[@]}"; do
    if [ -x "$TOOLS_DIR/bin/$t" ]; then CLIENTS+=("$TOOLS_DIR/bin/$t"); fi
done
write_allowlist "$WORK/allowlist.plist" "$(date -u +%Y%m%d%H%M)" "$VIEWER_APP/Contents/MacOS/realsense-usbd" "${CLIENTS[@]}"
DAEMON_HASHES="$(grep '/realsense-usbd$' "$CDHASH_RECORD" | cut -d' ' -f1 | sort -u | wc -l | tr -d ' ')"
[ "$DAEMON_HASHES" = 1 ] || { echo "error: the realsense-usbd copies differ" >&2; exit 1; }
for dir in "$VIEWER_APP/Contents/Resources/realsense-usbd" "$DQT_APP/Contents/Resources/realsense-usbd" \
           "$TOOLS_DIR/camera-access"; do
    cp "$WORK/allowlist.plist" "$dir/allowlist.plist"
done

# 5. Sign the bundles (seals Resources incl. the allowlist), verify, and prove the allowlisted code is unchanged
for app in "$VIEWER_APP" "$DQT_APP"; do
    codesign --force --sign - "$app"
    codesign --verify --strict --deep "$app"
    cmp -s "$WORK/allowlist.plist" "$app/Contents/Resources/realsense-usbd/allowlist.plist" \
        || { echo "error: allowlist in $app differs" >&2; exit 1; }
    echo "created $app"
done
cmp -s "$WORK/allowlist.plist" "$TOOLS_DIR/camera-access/allowlist.plist" \
    || { echo "error: allowlist in $TOOLS_DIR differs" >&2; exit 1; }
verify_cdhashes
echo "allowlisted $(plutil -extract AllowedCDHashes raw "$WORK/allowlist.plist") cdhashes, build $(plutil -extract BuildNumber raw "$WORK/allowlist.plist")"

# Disk image
STAGE="$WORK/dmg"
mkdir -p "$STAGE"
cp -R "$VIEWER_APP" "$DQT_APP" "$TOOLS_DIR" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
xattr -cr "$STAGE" 2>/dev/null || true   # no machine-specific extended attributes in the image
DMG="$DIST/RealSense-Viewer-$VERSION-macOS-arm64.dmg"
hdiutil create -volname "RealSense Viewer $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
echo "created $DMG"
