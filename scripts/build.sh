#!/bin/bash
# Builds librealsense (with RealSense Viewer and tools) natively for Apple Silicon macOS.
#
#   scripts/build.sh            Release build into ./build
#   BUILD_TYPE=Debug scripts/build.sh
#   VIEWER_TESTS=ON scripts/build.sh   also builds realsense-viewer-tests (GUI tests driven by ImGui Test Engine)
#   scripts/build.sh --asan|--tsan|--fuzz   also builds that variant of the USB broker tests (macos/broker)
set -euo pipefail

DAEMON_FLAGS=()
for arg in "$@"; do
    case "$arg" in
        --asan | --tsan | --fuzz) DAEMON_FLAGS+=("$arg") ;;
        *) echo "usage: $0 [--asan] [--tsan] [--fuzz]" >&2; exit 2 ;;
    esac
done

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/librealsense"
BUILD="${BUILD_DIR:-$ROOT/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-27.0}"   # macOS 27 "Golden Gate"
JOBS="$(sysctl -n hw.ncpu)"
# Keep absolute build paths (and with them the local user name) out of the binaries (__FILE__ in log/assert macros)
PREFIX_MAP="-ffile-prefix-map=$ROOT=. -ffile-prefix-map=$BUILD=build"

# USB broker checks (macos/broker/DESIGN.md 1.1, 3.7.1, 8.1)
check_broker_header() {   # the daemon's and librealsense's copy (patch 0002) of the wire protocol must be identical
    if ! cmp -s "$ROOT/macos/broker/rs-usb-broker-protocol.h" "$SRC/src/libusb/broker/rs-usb-broker-protocol.h"; then
        echo "error: macos/broker/rs-usb-broker-protocol.h and its copy in librealsense src/libusb/broker differ" >&2
        return 1
    fi
}
check_usbd_hardening() {   # only system libraries, no LC_RPATH
    local libs loads
    [ -x "$1" ] || { echo "error: $1 was not built" >&2; return 1; }
    libs="$(otool -L "$1")" && loads="$(otool -l "$1")" || return 1
    if tail -n +2 <<<"$libs" | awk '{print $1}' | grep -v -e '^/usr/lib/' -e '^/System/Library/' >&2; then
        echo "error: $1 links the libraries above; only /usr/lib and /System/Library are allowed" >&2
        return 1
    fi
    if grep -q LC_RPATH <<<"$loads"; then
        echo "error: $1 has an LC_RPATH" >&2
        return 1
    fi
}
check_test_hook() {   # rs-usbd-tests drives librealsense's broker client through its internal test hook
    local syms
    syms="$(nm -gU "$1")" || return 1
    if ! grep -q use_endpoint_for_testing <<<"$syms"; then
        echo "error: $1 does not export usb_broker::use_endpoint_for_testing" >&2
        return 1
    fi
}
# end of checks

if [ "$(uname -m)" != "arm64" ]; then
    echo "error: this project targets Apple Silicon (arm64) Macs" >&2
    exit 1
fi
for tool in cmake git clang swiftc; do
    command -v "$tool" >/dev/null || { echo "error: '$tool' not found (brew install cmake; xcode-select --install)" >&2; exit 1; }
done

# librealsense sources + macOS patches
if [ ! -f "$SRC/CMakeLists.txt" ]; then
    git -C "$ROOT" submodule update --init --depth 1 third_party/librealsense
fi
"$ROOT/scripts/apply-patches.sh"

check_broker_header

cmake -S "$SRC" -B "$BUILD" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
    -DCMAKE_C_FLAGS="$PREFIX_MAP" \
    -DCMAKE_CXX_FLAGS="$PREFIX_MAP" \
    -DFORCE_RSUSB_BACKEND=ON \
    -DUSE_EXTERNAL_USB=ON \
    -DBUILD_EXAMPLES=ON \
    -DBUILD_GRAPHICAL_EXAMPLES=ON \
    -DBUILD_TOOLS=ON \
    -DBUILD_VIEWER_TESTS="${VIEWER_TESTS:-OFF}" \
    -DBUILD_UNIT_TESTS=OFF \
    -DBUILD_WITH_OPENMP=OFF \
    -DCHECK_FOR_UPDATES=OFF \
    -DENABLE_CCACHE=OFF

cmake --build "$BUILD" -j "$JOBS"

# macOS helpers: hardware self-test, and the tools that hand cameras and files back after a root run
OUT="$BUILD/$BUILD_TYPE"
clang++ -std=c++14 -O2 -arch arm64 -mmacosx-version-min="$DEPLOYMENT_TARGET" $PREFIX_MAP -I"$SRC/include" \
    "$ROOT/tools/selftest/rs-macos-selftest.cpp" -L"$OUT" -lrealsense2 -Wl,-rpath,@executable_path \
    -o "$OUT/rs-macos-selftest"
clang -O2 -Wall -arch arm64 -mmacosx-version-min="$DEPLOYMENT_TARGET" $PREFIX_MAP \
    "$ROOT/tools/release/rs-macos-release.c" -framework IOKit -framework CoreFoundation \
    -o "$OUT/rs-macos-release"
clang -O2 -Wall -arch arm64 -mmacosx-version-min="$DEPLOYMENT_TARGET" $PREFIX_MAP \
    "$ROOT/tools/release/rs-macos-handback.c" -o "$OUT/rs-macos-handback"
# Uses librealsense internals (exported by librealsense2.dylib)
clang++ -std=c++14 -O2 -arch arm64 -mmacosx-version-min="$DEPLOYMENT_TARGET" $PREFIX_MAP -I"$SRC/include" \
    -I"$SRC/src" -I"$SRC/third-party/rsutils/include" -I"$BUILD/libusb_install/include/libusb-1.0" \
    "$ROOT/tools/selftest/rs-macos-usbdump.cpp" -L"$OUT" -lrealsense2 -Wl,-rpath,@executable_path \
    -o "$OUT/rs-macos-usbdump"

# USB broker daemon realsense-usbd and its host tests (macos/broker/DESIGN.md 8.1). build-daemon.sh reads the same
# BUILD_DIR / BUILD_TYPE / MACOSX_DEPLOYMENT_TARGET as this script and leaves realsense-usbd in $OUT.
BUILD_DIR="$BUILD" BUILD_TYPE="$BUILD_TYPE" MACOSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
    "$ROOT/macos/broker/build-daemon.sh" ${DAEMON_FLAGS[@]+"${DAEMON_FLAGS[@]}"}
check_usbd_hardening "$OUT/realsense-usbd"
check_test_hook "$OUT/librealsense2.dylib"

echo
echo "Build finished: $OUT"
echo "Run the viewer:     sudo \"$OUT/realsense-viewer\"   (the build tree is not allowlisted for camera access)"
echo "Hardware self-test: sudo \"$OUT/rs-macos-selftest\""
echo "Create the apps:    scripts/package.sh"
