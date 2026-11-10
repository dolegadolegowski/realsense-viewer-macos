#!/bin/bash
# Builds realsense-usbd and its host test binaries (DESIGN 8.1, daemon part). Runs nothing it built, needs no root.
# Everything it needs besides this folder comes as an argument, so the folder builds the same in any repository.
#
#   build-daemon.sh <out dir> --libusb <path to libusb-1.0.a> --librealsense-src <librealsense source dir>
#                   [--deployment-target <macOS version, default 27.0>]
#                   [--librealsense-lib <dir with librealsense2.dylib>]
#                   [--daemon-only] [--asan] [--tsan] [--no-fuzz]
#
# --libusb              the static libusb to link; its headers are taken from ../include/libusb-1.0 next to it
# --librealsense-src    librealsense sources: gen-usbd-pids.sh reads the PID lists from them. When they carry the
#                       broker client (librealsense/macos-usb-broker.patch applied), its copy of the protocol header
#                       must equal ours, and the fuzzer also checks the client's reply validator.
# --librealsense-lib    a librealsense2.dylib built from those sources with the patch: adds the H3 client cases to
#                       rs-usbd-tests
# --daemon-only         only realsense-usbd (the test options are then ignored)
#
# Outputs in <out dir>: realsense-usbd always; unless --daemon-only also rs-usbd-tests, rs-usbd-testd, rs-usbd-fuzz
# (ASan+UBSan, unless --no-fuzz), and with --asan / --tsan asan/rs-usbd-tests (ASan+UBSan) and tsan/rs-usbd-tests.
# usbd/rs-usbd-pids.h is generated, obj/ holds the objects (only what changed is compiled again).
# The test binaries look for the fixtures in this folder's tests/fixtures (--fixtures <dir> overrides).
set -euo pipefail

KIT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
USAGE="usage: build-daemon.sh <out dir> --libusb <libusb-1.0.a> --librealsense-src <dir> [--deployment-target <version>]
                       [--librealsense-lib <dir>] [--daemon-only] [--asan] [--tsan] [--no-fuzz]"
usage() { echo "$USAGE" >&2; exit 64; }

[ $# -ge 1 ] && [ -n "$1" ] && [ "${1#-}" = "$1" ] || usage
OUT="$1"
shift
LIBUSB_A=""
LRS_SRC=""
LRS_LIB=""
DEPLOYMENT_TARGET="27.0"
DAEMON_ONLY=0 ASAN=0 TSAN=0 FUZZ=1
while [ $# -gt 0 ]; do
    a="$1"
    shift
    case "$a" in
        --libusb | --librealsense-src | --librealsense-lib | --deployment-target)
            [ $# -ge 1 ] && [ -n "$1" ] || { echo "error: $a needs a value" >&2; usage; }
            case "$a" in
                --libusb) LIBUSB_A="$1" ;;
                --librealsense-src) LRS_SRC="$1" ;;
                --librealsense-lib) LRS_LIB="$1" ;;
                --deployment-target) DEPLOYMENT_TARGET="$1" ;;
            esac
            shift ;;
        --daemon-only) DAEMON_ONLY=1 ;;
        --asan) ASAN=1 ;;
        --tsan) TSAN=1 ;;
        --no-fuzz) FUZZ=0 ;;
        *) echo "error: unknown argument $a" >&2; usage ;;
    esac
done
[ -n "$LIBUSB_A" ] || { echo "error: --libusb is required" >&2; usage; }
[ -n "$LRS_SRC" ] || { echo "error: --librealsense-src is required" >&2; usage; }
[[ "$DEPLOYMENT_TARGET" =~ ^[0-9]+(\.[0-9]+){0,2}$ ]] \
    || { echo "error: bad --deployment-target $DEPLOYMENT_TARGET" >&2; usage; }

[ -f "$LIBUSB_A" ] || { echo "error: $LIBUSB_A not found" >&2; exit 1; }
LIBUSB_A="$(cd "$(dirname "$LIBUSB_A")" && pwd)/$(basename "$LIBUSB_A")"
LIBUSB_INC="$(cd "$(dirname "$LIBUSB_A")/.." && pwd)/include/libusb-1.0"
[ -f "$LIBUSB_INC/libusb.h" ] || { echo "error: $LIBUSB_INC/libusb.h not found" >&2; exit 1; }
[ -d "$LRS_SRC" ] || { echo "error: $LRS_SRC is not a directory" >&2; exit 1; }
LRS_SRC="$(cd "$LRS_SRC" && pwd)"
if [ "$DAEMON_ONLY" = 0 ] && [ -n "$LRS_LIB" ]; then
    [ -f "$LRS_LIB/librealsense2.dylib" ] || { echo "error: $LRS_LIB/librealsense2.dylib not found" >&2; exit 1; }
    LRS_LIB="$(cd "$LRS_LIB" && pwd)"
fi

# 1.1: the client compiles the same protocol header; the copy the client patch adds to librealsense must not differ
CLIENT_DIR="$LRS_SRC/src/libusb/broker"
CLIENT_HEADER="$CLIENT_DIR/rs-usb-broker-protocol.h"
if [ -f "$CLIENT_HEADER" ] && ! cmp -s "$KIT/rs-usb-broker-protocol.h" "$CLIENT_HEADER"; then
    echo "error: $CLIENT_HEADER differs from $KIT/rs-usb-broker-protocol.h" >&2
    echo "       (apply the current librealsense/macos-usb-broker.patch to the librealsense sources)" >&2
    exit 1
fi

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
JOBS="$(sysctl -n hw.ncpu)"
"$KIT/gen-usbd-pids.sh" "$LRS_SRC" "$OUT/usbd/rs-usbd-pids.h" >/dev/null

CORE=("$KIT"/daemon/core/*.cpp)
DAEMON=("$KIT"/daemon/*.cpp)
MOCK=("$KIT/tests/backend-mock.cpp" "$KIT/tests/policy-fake.cpp")
FRAMEWORKS=(-framework IOKit -framework CoreFoundation -framework Security -framework SystemConfiguration)
# The prefix maps keep absolute build paths (and with them the local user name) out of realsense-usbd
BASE=(-std=c++17 -arch arm64 -mmacosx-version-min="$DEPLOYMENT_TARGET"
      "-ffile-prefix-map=$KIT=." "-ffile-prefix-map=$OUT=build"
      -Wall -Wextra -I "$KIT" -I "$OUT/usbd" -I "$LIBUSB_INC")
# Test binaries only (they stay in the build folder): where the fixtures are unless --fixtures says otherwise
TESTS=(-O1 -g "-DRSUSBD_FIXTURES=\"$KIT/tests/fixtures\"")

# compile <variant> <extra flags...> -- <sources...>: objects in $OUT/obj/<variant>, list in OBJS. A source is compiled
# again when it, or any header of this folder or the generated one, is newer than its object.
HEADER_DIRS=("$KIT/daemon" "$KIT/tests" "$KIT/rs-usb-broker-protocol.h" "$OUT/usbd")
compile() {
    local variant="$1"; shift
    local flags=()
    while [ "$1" != "--" ]; do flags+=("$1"); shift; done
    shift
    mkdir -p "$OUT/obj/$variant"
    OBJS=()
    local pids=()
    for src in "$@"; do
        local rel="${src#"$KIT"/}"
        rel="${rel#"$LRS_SRC"/}"
        local obj="$OUT/obj/$variant/${rel//\//_}.o"
        OBJS+=("$obj")
        if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] \
            || [ -n "$(find "${HEADER_DIRS[@]}" -name '*.h' -newer "$obj" -print -quit)" ]; then
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

# 3.7.1: only system libraries, no LC_RPATH
BAD="$(otool -L "$OUT/realsense-usbd" | tail -n +2 | awk '{print $1}' | grep -vE '^(/usr/lib/|/System/Library/)' || true)"
[ -z "$BAD" ] || { echo "error: realsense-usbd links non-system libraries: $BAD" >&2; exit 1; }
if otool -l "$OUT/realsense-usbd" | grep -q LC_RPATH; then
    echo "error: realsense-usbd has an LC_RPATH" >&2; exit 1
fi
if [ "$DAEMON_ONLY" = 1 ]; then
    echo "built in $OUT"
    exit 0
fi

# Host tests (H2 / H4 daemon side): core + mock + fake policy, and the real policy for its admission case; no
# libusb. With librealsense2 also H3.
TEST_SRC=("${CORE[@]}" "${MOCK[@]}" "$KIT/daemon/policy-macos.cpp" "$KIT/tests/rs-usbd-tests.cpp")
if [ -n "$LRS_LIB" ]; then
    compile tests-h3 "${TESTS[@]}" -DRSUSBD_H3 -isystem "$LRS_SRC/include" -isystem "$LRS_SRC/src" \
        -isystem "$LRS_SRC/third-party/rsutils/include" -- "${TEST_SRC[@]}"
    clang++ "${BASE[@]}" -g "${OBJS[@]}" "${FRAMEWORKS[@]}" -L "$LRS_LIB" -lrealsense2 -Wl,-rpath,"$LRS_LIB" \
        -o "$OUT/rs-usbd-tests"
else
    compile tests "${TESTS[@]}" -- "${TEST_SRC[@]}"
    clang++ "${BASE[@]}" -g "${OBJS[@]}" "${FRAMEWORKS[@]}" -o "$OUT/rs-usbd-tests"
fi

# H6: mock backend behind the real policy
compile tests "${TESTS[@]}" -- "${CORE[@]}" "$KIT/tests/backend-mock.cpp" "$KIT/daemon/policy-macos.cpp" \
    "$KIT/tests/rs-usbd-testd.cpp"
clang++ "${BASE[@]}" -g "${OBJS[@]}" "${FRAMEWORKS[@]}" -o "$OUT/rs-usbd-testd"

SAN_ASAN=("-fsanitize=address,undefined" -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
if [ "$FUZZ" = 1 ]; then
    # H5: Apple clang ships no libFuzzer runtime; rs-usbd-fuzz has its own bounded mutation driver
    WIRE="$CLIENT_DIR/broker-wire.cpp"
    FUZZ_SRC=("${CORE[@]}" "${MOCK[@]}" "$KIT/daemon/snapshot-libusb.cpp" "$KIT/tests/fuzz/rs-usbd-fuzz.cpp")
    FUZZ_DEF=()
    if [ -f "$WIRE" ]; then FUZZ_SRC+=("$WIRE"); FUZZ_DEF=(-DRSUSBD_CLIENT_VALIDATOR -I "$CLIENT_DIR"); fi
    compile fuzz "${TESTS[@]}" "${SAN_ASAN[@]}" -fsanitize-coverage=trace-pc-guard \
        "${FUZZ_DEF[@]+"${FUZZ_DEF[@]}"}" -- "${FUZZ_SRC[@]}"
    clang++ "${BASE[@]}" -g "${SAN_ASAN[@]}" "${OBJS[@]}" "${FRAMEWORKS[@]}" -o "$OUT/rs-usbd-fuzz"
fi
if [ "$ASAN" = 1 ]; then
    mkdir -p "$OUT/asan"
    compile asan "${TESTS[@]}" "${SAN_ASAN[@]}" -- "${TEST_SRC[@]}"
    clang++ "${BASE[@]}" -g "${SAN_ASAN[@]}" "${OBJS[@]}" "${FRAMEWORKS[@]}" -o "$OUT/asan/rs-usbd-tests"
fi
if [ "$TSAN" = 1 ]; then
    mkdir -p "$OUT/tsan"
    compile tsan "${TESTS[@]}" -fsanitize=thread -- "${TEST_SRC[@]}"
    clang++ "${BASE[@]}" -g -fsanitize=thread "${OBJS[@]}" "${FRAMEWORKS[@]}" -o "$OUT/tsan/rs-usbd-tests"
fi
echo "built in $OUT"
