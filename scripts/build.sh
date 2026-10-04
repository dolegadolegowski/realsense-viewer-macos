#!/bin/bash
# Builds librealsense (with RealSense Viewer and tools) natively for Apple Silicon macOS.
#
#   scripts/build.sh            Release build into ./build
#   BUILD_TYPE=Debug scripts/build.sh
#   VIEWER_TESTS=ON scripts/build.sh   also builds realsense-viewer-tests (GUI tests driven by ImGui Test Engine)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/librealsense"
BUILD="${BUILD_DIR:-$ROOT/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-27.0}"   # macOS 27 "Golden Gate"
JOBS="$(sysctl -n hw.ncpu)"
# Keep absolute build paths (and with them the local user name) out of the binaries (__FILE__ in log/assert macros)
PREFIX_MAP="-ffile-prefix-map=$ROOT=. -ffile-prefix-map=$BUILD=build"

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

# macOS helpers: hardware self-test and the tool that hands cameras back to macOS
OUT="$BUILD/$BUILD_TYPE"
clang++ -std=c++14 -O2 -arch arm64 -mmacosx-version-min="$DEPLOYMENT_TARGET" $PREFIX_MAP -I"$SRC/include" \
    "$ROOT/tools/selftest/rs-macos-selftest.cpp" -L"$OUT" -lrealsense2 -Wl,-rpath,@executable_path \
    -o "$OUT/rs-macos-selftest"
clang -O2 -Wall -arch arm64 -mmacosx-version-min="$DEPLOYMENT_TARGET" $PREFIX_MAP \
    "$ROOT/tools/release/rs-macos-release.c" -framework IOKit -framework CoreFoundation \
    -o "$OUT/rs-macos-release"

echo
echo "Build finished: $OUT"
echo "Run the viewer:     sudo \"$OUT/realsense-viewer\""
echo "Hardware self-test: sudo \"$OUT/rs-macos-selftest\""
echo "Create the apps:    scripts/package.sh"
