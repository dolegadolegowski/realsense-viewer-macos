#!/bin/bash
# Builds realsense-usbd and its host test binaries (DESIGN 8.1, daemon part). Runs nothing, needs no root.
#
#   macos/broker/build-daemon.sh <out dir> [<libusb-1.0.a>] [<deployment target>] [--librealsense <dir>]
#                                [--asan] [--tsan] [--no-fuzz]
#   BUILD_DIR=<build> [BUILD_TYPE=Release] [MACOSX_DEPLOYMENT_TARGET=27.0] macos/broker/build-daemon.sh [--asan] [--tsan] [--fuzz]
#
# Outputs in <out dir>: realsense-usbd, rs-usbd-tests, rs-usbd-testd, rs-usbd-fuzz (ASan+UBSan), and with --asan /
# --tsan also asan/rs-usbd-tests (ASan+UBSan) and tsan/rs-usbd-tests. usbd/rs-usbd-pids.h is generated.
# --librealsense <dir> (a directory with librealsense2.dylib built from this tree) adds the H3 client cases to the plain
# rs-usbd-tests. The second form is how scripts/build.sh calls it: <out dir> is <build>/usbd, libusb and librealsense
# come from that build, and realsense-usbd is also copied to <build>/<BUILD_TYPE>.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BROKER="$ROOT/macos/broker"
LRS_SRC="$ROOT/third_party/librealsense"
USAGE="usage: build-daemon.sh <out dir> [<libusb-1.0.a>] [<deployment target>] [--librealsense <dir>] [--asan] [--tsan] [--no-fuzz]"
LIBUSB_A="$ROOT/build/libusb_install/lib/libusb-1.0.a"
DEPLOYMENT_TARGET="27.0"
LRS_LIB=""
COPY_TO=""
ASAN=0 TSAN=0 FUZZ=1
if [ $# -eq 0 ] || [ "${1#--}" != "$1" ]; then
    [ -n "${BUILD_DIR:-}" ] || { echo "$USAGE" >&2; exit 64; }
    OUT="$BUILD_DIR/usbd"
    LIBUSB_A="$BUILD_DIR/libusb_install/lib/libusb-1.0.a"
    DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-27.0}"
    COPY_TO="$BUILD_DIR/${BUILD_TYPE:-Release}"
    LRS_LIB="$COPY_TO"
else
    OUT="$1"
    shift
fi
POSITIONAL=0
while [ $# -gt 0 ]; do
    a="$1"
    shift
    case "$a" in
        --asan) ASAN=1 ;;
        --tsan) TSAN=1 ;;
        --fuzz) FUZZ=1 ;;
        --no-fuzz) FUZZ=0 ;;
        --librealsense) LRS_LIB="${1:?--librealsense needs a directory}"; shift ;;
        -*) echo "error: unknown option $a" >&2; exit 64 ;;
        *)
            POSITIONAL=$((POSITIONAL + 1))
            if [ "$POSITIONAL" -eq 1 ]; then LIBUSB_A="$a"; else DEPLOYMENT_TARGET="$a"; fi ;;
    esac
done

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
LIBUSB_INC="$(cd "$(dirname "$LIBUSB_A")/.." && pwd)/include/libusb-1.0"
[ -f "$LIBUSB_A" ] || { echo "error: $LIBUSB_A not found" >&2; exit 1; }
[ -f "$LIBUSB_INC/libusb.h" ] || { echo "error: $LIBUSB_INC/libusb.h not found" >&2; exit 1; }
# Keep absolute build paths (and with them the local user name) out of the binaries
PREFIX_MAP="-ffile-prefix-map=$ROOT=. -ffile-prefix-map=$OUT=build"
JOBS="$(sysctl -n hw.ncpu)"

"$ROOT/scripts/gen-usbd-pids.sh" "$ROOT/third_party/librealsense" "$OUT/usbd/rs-usbd-pids.h" >/dev/null

CORE=("$BROKER"/daemon/core/*.cpp)
DAEMON=("$BROKER"/daemon/*.cpp)
MOCK=("$BROKER/tests/backend-mock.cpp" "$BROKER/tests/policy-fake.cpp")
FRAMEWORKS=(-framework IOKit -framework CoreFoundation -framework Security -framework SystemConfiguration)
BASE=(-std=c++17 -arch arm64 -mmacosx-version-min="$DEPLOYMENT_TARGET" $PREFIX_MAP -Wall -Wextra
      -I "$BROKER" -I "$OUT/usbd" -I "$LIBUSB_INC")

# compile <variant> <extra flags...> -- <sources...>: objects in $OUT/obj/<variant>, list in OBJS
compile() {
    local variant="$1"; shift
    local flags=()
    while [ "$1" != "--" ]; do flags+=("$1"); shift; done
    shift
    mkdir -p "$OUT/obj/$variant"
    OBJS=()
    local pids=()
    for src in "$@"; do
        local rel="${src#"$BROKER"/}"
        rel="${rel#"$ROOT"/}"
        local obj="$OUT/obj/$variant/${rel//\//_}.o"
        OBJS+=("$obj")
        if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ -n "$(find "$BROKER" "$OUT/usbd" -name '*.h' -newer "$obj" -print -quit)" ]; then
            clang++ "${BASE[@]}" "${flags[@]}" -c "$src" -o "$obj" &
            pids+=($!)
            if [ "${#pids[@]}" -ge "$JOBS" ]; then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
        fi
    done
    for p in "${pids[@]+"${pids[@]}"}"; do wait "$p"; done
}

# realsense-usbd: core + daemon, static libusb, system frameworks only (3.7.1)
compile release -O2 -- "${CORE[@]}" "${DAEMON[@]}"
clang++ "${BASE[@]}" -O2 "${OBJS[@]}" "$LIBUSB_A" "${FRAMEWORKS[@]}" -o "$OUT/realsense-usbd"

# Host tests (H2 / H4 daemon side): core + mock + fake policy, no libusb; with librealsense2 also H3
if [ -n "$LRS_LIB" ]; then
    [ -f "$LRS_LIB/librealsense2.dylib" ] || { echo "error: $LRS_LIB/librealsense2.dylib not found" >&2; exit 1; }
    compile tests-h3 -O1 -g -DRSUSBD_H3 -isystem "$LRS_SRC/include" -isystem "$LRS_SRC/src" -isystem "$LRS_SRC/third-party/rsutils/include" \
        -- "${CORE[@]}" "${MOCK[@]}" "$BROKER/tests/rs-usbd-tests.cpp"
    clang++ "${BASE[@]}" -g "${OBJS[@]}" "${FRAMEWORKS[@]}" -L "$LRS_LIB" -lrealsense2 -Wl,-rpath,"$LRS_LIB" \
        -o "$OUT/rs-usbd-tests"
else
    compile tests -O1 -g -- "${CORE[@]}" "${MOCK[@]}" "$BROKER/tests/rs-usbd-tests.cpp"
    clang++ "${BASE[@]}" -g "${OBJS[@]}" "${FRAMEWORKS[@]}" -o "$OUT/rs-usbd-tests"
fi

# H6: mock backend behind the real policy
compile tests -O1 -g -- "${CORE[@]}" "$BROKER/tests/backend-mock.cpp" "$BROKER/daemon/policy-macos.cpp" "$BROKER/tests/rs-usbd-testd.cpp"
clang++ "${BASE[@]}" -g "${OBJS[@]}" "${FRAMEWORKS[@]}" -o "$OUT/rs-usbd-testd"

SAN_ASAN=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
if [ "$FUZZ" = 1 ]; then
    # H5: Apple clang ships no libFuzzer runtime; rs-usbd-fuzz has its own bounded mutation driver
    WIRE="$ROOT/third_party/librealsense/src/libusb/broker/broker-wire.cpp"
    FUZZ_SRC=("${CORE[@]}" "${MOCK[@]}" "$BROKER/daemon/snapshot-libusb.cpp" "$BROKER/tests/fuzz/rs-usbd-fuzz.cpp")
    FUZZ_DEF=()
    if [ -f "$WIRE" ]; then FUZZ_SRC+=("$WIRE"); FUZZ_DEF=(-DRSUSBD_CLIENT_VALIDATOR -I "$(dirname "$WIRE")"); fi
    compile fuzz "${SAN_ASAN[@]}" -fsanitize-coverage=trace-pc-guard "${FUZZ_DEF[@]+"${FUZZ_DEF[@]}"}" -- "${FUZZ_SRC[@]}"
    clang++ "${BASE[@]}" "${SAN_ASAN[@]}" "${OBJS[@]}" "${FRAMEWORKS[@]}" -o "$OUT/rs-usbd-fuzz"
fi
if [ "$ASAN" = 1 ]; then
    mkdir -p "$OUT/asan"
    compile asan "${SAN_ASAN[@]}" -- "${CORE[@]}" "${MOCK[@]}" "$BROKER/tests/rs-usbd-tests.cpp"
    clang++ "${BASE[@]}" "${SAN_ASAN[@]}" "${OBJS[@]}" "${FRAMEWORKS[@]}" -o "$OUT/asan/rs-usbd-tests"
fi
if [ "$TSAN" = 1 ]; then
    mkdir -p "$OUT/tsan"
    compile tsan -O1 -g -fsanitize=thread -- "${CORE[@]}" "${MOCK[@]}" "$BROKER/tests/rs-usbd-tests.cpp"
    clang++ "${BASE[@]}" -g -fsanitize=thread "${OBJS[@]}" "${FRAMEWORKS[@]}" -o "$OUT/tsan/rs-usbd-tests"
fi

# 3.7.1: only system libraries, no LC_RPATH
BAD="$(otool -L "$OUT/realsense-usbd" | tail -n +2 | awk '{print $1}' | grep -vE '^(/usr/lib/|/System/Library/)' || true)"
[ -z "$BAD" ] || { echo "error: realsense-usbd links non-system libraries: $BAD" >&2; exit 1; }
if otool -l "$OUT/realsense-usbd" | grep -q LC_RPATH; then
    echo "error: realsense-usbd has an LC_RPATH" >&2; exit 1
fi
if [ -n "$COPY_TO" ]; then
    mkdir -p "$COPY_TO"
    cp -f "$OUT/realsense-usbd" "$COPY_TO/realsense-usbd"
fi
echo "built in $OUT"
