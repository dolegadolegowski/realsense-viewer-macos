#!/bin/bash
# Builds librealsense (with RealSense Viewer and tools) natively for Apple Silicon macOS.
#
#   scripts/build.sh            Release build into ./build
#   BUILD_TYPE=Debug scripts/build.sh
#   VIEWER_TESTS=ON scripts/build.sh   also builds realsense-viewer-tests (GUI tests driven by ImGui Test Engine)
#   scripts/build.sh --asan|--tsan     also builds that sanitizer variant of the camera service tests (macos/broker)
#   scripts/build.sh --fuzz            accepted for older command lines: the fuzzer rs-usbd-fuzz is always built
set -euo pipefail

DAEMON_FLAGS=()
for arg in "$@"; do
    case "$arg" in
        --asan | --tsan) DAEMON_FLAGS+=("$arg") ;;
        --fuzz) ;;
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

# Camera service checks that belong to this build (macos/broker/build-daemon.sh checks its own output as well)
# check_broker_header runs before the long build: the kit's wire protocol header and the copy that its patch adds to
# librealsense must be identical
check_broker_header() {
    if ! cmp -s "$ROOT/macos/broker/rs-usb-broker-protocol.h" "$SRC/src/libusb/broker/rs-usb-broker-protocol.h"; then
        echo "error: macos/broker/rs-usb-broker-protocol.h and its copy in librealsense src/libusb/broker differ" >&2
        return 1
    fi
}
check_usbd_hardening() {   # the daemon that package.sh ships: only system libraries, no LC_RPATH
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
check_test_hook() {   # rs-usbd-tests drives librealsense's camera service client through its internal test hook
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

# librealsense sources + macOS patches (patches/ and the kit's camera service client)
if [ ! -f "$SRC/CMakeLists.txt" ]; then
    git -C "$ROOT" submodule update --init --depth 1 third_party/librealsense
fi
"$ROOT/scripts/apply-patches.sh"

check_broker_header

# CMAKE_DISABLE_FIND_PACKAGE_glfw3: always build the GLFW in the librealsense tree (patched to a shared library that
# package.sh bundles). A GLFW found on the build machine, from Homebrew for example, would be linked by its absolute
# path and be missing in the apps.
cmake -S "$SRC" -B "$BUILD" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_DISABLE_FIND_PACKAGE_glfw3=ON \
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

# Camera service daemon realsense-usbd and its host tests. macos/broker is a kit that other applications copy
# unchanged, so it takes every input as an argument; realsense-usbd is copied next to the tools for package.sh.
check_test_hook "$OUT/librealsense2.dylib"
"$ROOT/macos/broker/build-daemon.sh" "$BUILD/usbd" --libusb "$BUILD/libusb_install/lib/libusb-1.0.a" \
    --librealsense-src "$SRC" --librealsense-lib "$OUT" --deployment-target "$DEPLOYMENT_TARGET" \
    ${DAEMON_FLAGS[@]+"${DAEMON_FLAGS[@]}"}
cp -f "$BUILD/usbd/realsense-usbd" "$OUT/realsense-usbd"
check_usbd_hardening "$OUT/realsense-usbd"

echo
echo "Build finished: $OUT"
echo "Run the viewer:       \"$OUT/realsense-viewer\""
echo "Hardware self-test:   \"$OUT/rs-macos-selftest\""
echo "    Both reach the camera through the RealSense camera service when it is installed on this Mac (it admits"
echo "    programs from the build tree like any other); without the service, run them with sudo."
echo "Camera service tests: \"$BUILD/usbd/rs-usbd-tests\" && \\"
echo "                      \"$ROOT/macos/broker/tests/test-camera-access.sh\" \"$OUT/realsense-usbd\""
echo "Create the apps:      scripts/package.sh"
