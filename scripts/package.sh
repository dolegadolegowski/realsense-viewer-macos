#!/bin/bash
# Packages the build into self-contained macOS apps and a DMG:
#   dist/RealSense Viewer.app
#   dist/RealSense Depth Quality Tool.app
#   dist/RealSense Tools/          command line tools (rs-enumerate-devices, rs-fw-update, ...) and camera-access/
#   dist/RealSense-Viewer-<version>-macOS-arm64.dmg
# Every app and the Tools folder carry the shared RealSense camera service: the daemon realsense-usbd, its manifest
# service.plist and the install / uninstall / ensure scripts (macos/broker, bundle layout in its README.md). The
# service admits every program of the user at the Mac's screen, so nothing here depends on which tools are packaged.
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
KIT="$ROOT/macos/broker"
USBD_ID=io.github.dolegadolegowski.realsense-usbd

[ -x "$OUT/realsense-viewer" ] || { echo "error: run scripts/build.sh first" >&2; exit 1; }
[ -x "$OUT/realsense-usbd" ] || { echo "error: $OUT/realsense-usbd missing, run scripts/build.sh first" >&2; exit 1; }

VERSION="$(sed -n 's/^#define RS2_API_MAJOR_VERSION *\([0-9]*\).*/\1/p' "$SRC/include/librealsense2/rs.h").$(sed -n 's/^#define RS2_API_MINOR_VERSION *\([0-9]*\).*/\1/p' "$SRC/include/librealsense2/rs.h").$(sed -n 's/^#define RS2_API_PATCH_VERSION *\([0-9]*\).*/\1/p' "$SRC/include/librealsense2/rs.h")"
LIBS=(librealsense2.2.58.dylib librealsense2-gl.2.58.dylib libglfw.3.dylib)
TOOLS=(rs-enumerate-devices rs-fw-update rs-fw-logger rs-terminal rs-data-collect rs-convert rs-record rs-embed
       rs-on-chip-calib rs-rosbag-inspector rs-benchmark rs-macos-selftest rs-macos-usbdump rs-macos-release
       rs-hello-realsense rs-capture rs-depth rs-color rs-infrared rs-motion rs-pointcloud rs-align
       rs-align-advanced rs-multicam rs-measure rs-post-processing rs-record-playback rs-save-to-disk
       rs-sensor-control rs-distance rs-callback rs-hdr)
SERVICE_SCRIPTS=(install-camera-access.sh uninstall-camera-access.sh ensure-camera-access.sh)
for f in make-service-manifest.sh client/RealSenseCameraAccess.swift "${SERVICE_SCRIPTS[@]}"; do
    [ -f "$KIT/$f" ] || { echo "error: $KIT/$f missing" >&2; exit 1; }
done

# cdhash_of <Mach-O>: its arm64 code directory hash (40 lowercase hex), the form service.plist names the daemon by
cdhash_of() {
    local h
    h="$(codesign -dvvv --arch arm64 "$1" 2>&1 | sed -n 's/^CDHash=//p')"
    [[ "$h" =~ ^[0-9a-f]{40}$ ]] || { echo "error: no arm64 cdhash for $1" >&2; return 1; }
    echo "$h"
}

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

# Launcher (see macos/launcher/main.swift), with the camera service client every application compiles in unchanged
swiftc -O -target "arm64-apple-macos$DEPLOYMENT_TARGET" "$ROOT/macos/launcher/main.swift" \
    "$KIT/client/RealSenseCameraAccess.swift" -o "$WORK/launcher"

# The camera service, in the kit's packaging order: sign the daemon (hardened runtime, its fixed identifier), then
# write the manifest that names it by cdhash. Every bundle gets these same two files, and the bundle signatures come
# last because they seal Resources, where the manifest lies.
cp "$OUT/realsense-usbd" "$WORK/realsense-usbd"
chmod 755 "$WORK/realsense-usbd"
codesign --force --sign - -o runtime -i "$USBD_ID" "$WORK/realsense-usbd"
"$KIT/make-service-manifest.sh" "$WORK/realsense-usbd" "$WORK/service.plist"
chmod 644 "$WORK/service.plist"

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

# copy_camera_service <dir for realsense-usbd> <dir for service.plist and the scripts>
copy_camera_service() {
    local bin_dir="$1" res_dir="$2" s
    mkdir -p "$bin_dir" "$res_dir"
    cp "$WORK/realsense-usbd" "$bin_dir/realsense-usbd"
    cp "$WORK/service.plist" "$res_dir/service.plist"
    for s in "${SERVICE_SCRIPTS[@]}"; do
        cp "$KIT/$s" "$res_dir/$s"
        chmod 755 "$res_dir/$s"
    done
}

# 1. Assemble both apps and the Tools folder; only the daemon is signed so far
make_app() {
    local name="$1" target="$2" id="$3" summary="$4"
    local app="$DIST/$name.app"
    mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
    cp "$WORK/launcher" "$app/Contents/MacOS/$name"
    cp "$OUT/$target" "$app/Contents/MacOS/$target"
    cp "$OUT/rs-macos-release" "$app/Contents/MacOS/rs-macos-release"
    cp "$OUT/rs-macos-handback" "$app/Contents/MacOS/rs-macos-handback"
    copy_camera_service "$app/Contents/MacOS" "$app/Contents/Resources/realsense-usbd"
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
copy_camera_service "$TOOLS_DIR/camera-access" "$TOOLS_DIR/camera-access"
cat > "$TOOLS_DIR/README.txt" <<TXT
RealSense command line tools $VERSION for Apple Silicon macOS.

RealSense camera service
------------------------
macOS only lets root take a camera over from its USB video driver. The RealSense camera service (realsense-usbd, a
small system service) does this for every RealSense application of this Mac: RealSense Viewer, the Depth Quality
Tool, the tools below and other applications built for it, such as IntelFaceScan. With it they run without sudo and
without a password, also on standard accounts. It is installed once per Mac and does not belong to one DMG or one
application:

  * RealSense Viewer installs it at its first start (the macOS administrator prompt, once), or
  * from this folder, as the user logged in at the Mac's screen:  ./camera-access/ensure-camera-access.sh
    (asks the service whether it answers and shows the administrator prompt only when it has to be installed), or
  * as root, for example over SSH:  sudo ./camera-access/install-camera-access.sh --source ./camera-access

The installer refuses files that other users could have changed: run it from a copy in a folder that only
administrators can change, or from the DMG an administrator opened. It never replaces a newer service with an
older one, so it does not matter which application installs it first.

Afterwards no sudo and no password are needed, e.g.:
    ./bin/rs-enumerate-devices
    ./bin/rs-macos-selftest
    ./bin/rs-fw-update -l
    ./bin/rs-terminal

Is it installed and answering?  ./camera-access/ensure-camera-access.sh --check
    (prints "ok <service version>" and exits 0 when it is; exit status 10 means not installed)
To remove it:  sudo ./camera-access/uninstall-camera-access.sh
    (for every RealSense application of this Mac; each of them offers to install it again)

Without the camera service
--------------------------
Run the tools with sudo, e.g. "sudo ./bin/rs-enumerate-devices". After a tool ran with sudo the camera stays
detached from macOS; "sudo ./bin/rs-macos-release" (or re-plugging it) gives it back to macOS, e.g. to use the RGB
camera as a webcam.
TXT

# 2. Sign every nested Mach-O (ad-hoc). realsense-usbd keeps the signature the manifest names.
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
            realsense-usbd) continue ;;
            realsense-viewer | rs-depth-quality)
                if [ "${CLIENT_RUNTIME:-0}" = 1 ]; then opts=(-o runtime --entitlements "$WORK/client.entitlements"); fi ;;
        esac
        codesign --force --sign - ${opts[@]+"${opts[@]}"} "$f" 2>/dev/null || { echo "error: cannot sign $f" >&2; return 1; }
    done < <(find "$1" -type f \( -name "*.dylib" -o -perm -u+x \) -print0)
}
sign_nested "$VIEWER_APP/Contents"
sign_nested "$DQT_APP/Contents"
sign_nested "$TOOLS_DIR"

# 3. Sign the bundles (seals Resources incl. the manifest) and verify them
for app in "$VIEWER_APP" "$DQT_APP"; do
    codesign --force --sign - "$app"
    codesign --verify --strict --deep "$app"
    echo "created $app"
done

# 4. Every copy of the daemon is still the one the manifest names, and every manifest is the one written above
DAEMON_CDHASH="$(plutil -extract DaemonCDHash raw -expect string "$WORK/service.plist")"
BIN_DIRS=("$VIEWER_APP/Contents/MacOS" "$DQT_APP/Contents/MacOS" "$TOOLS_DIR/camera-access")
RES_DIRS=("$VIEWER_APP/Contents/Resources/realsense-usbd" "$DQT_APP/Contents/Resources/realsense-usbd"
          "$TOOLS_DIR/camera-access")
for i in 0 1 2; do
    bin_dir="${BIN_DIRS[$i]}"
    res_dir="${RES_DIRS[$i]}"
    codesign --verify --strict "$bin_dir/realsense-usbd"
    [ "$(cdhash_of "$bin_dir/realsense-usbd")" = "$DAEMON_CDHASH" ] \
        || { echo "error: $bin_dir/realsense-usbd is not the daemon service.plist names" >&2; exit 1; }
    cmp -s "$WORK/service.plist" "$res_dir/service.plist" \
        || { echo "error: $res_dir/service.plist differs from the manifest of this build" >&2; exit 1; }
done
manifest_value() { plutil -extract "$1" raw -expect integer "$WORK/service.plist"; }
echo "camera service version $(manifest_value ServiceVersion)," \
    "protocol $(manifest_value ProtocolMin)-$(manifest_value ProtocolMax), daemon cdhash $DAEMON_CDHASH"

# Disk image
STAGE="$WORK/dmg"
mkdir -p "$STAGE"
cp -R "$VIEWER_APP" "$DQT_APP" "$TOOLS_DIR" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
xattr -cr "$STAGE" 2>/dev/null || true   # no machine-specific extended attributes in the image
DMG="$DIST/RealSense-Viewer-$VERSION-macOS-arm64.dmg"
hdiutil create -volname "RealSense Viewer $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
echo "created $DMG"
