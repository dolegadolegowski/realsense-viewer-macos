#!/bin/bash
# Writes service.plist, the manifest an application ships next to install-camera-access.sh (DESIGN 13, README.md):
# it says which camera service the application carries. The installer checks the daemon against it before it installs
# anything, and applications read ServiceVersion from it.
#
#   make-service-manifest.sh <signed realsense-usbd> <out service.plist>
#
# Keys: Version (integer 2), ServiceVersion, ProtocolMin, ProtocolMax (from `realsense-usbd --version`) and
# DaemonCDHash (the arm64 cdhash of the signed binary). Sign the daemon first and never again afterwards, because a
# new signature is a new cdhash:
#   codesign --force --sign - -o runtime -i io.github.dolegadolegowski.realsense-usbd <realsense-usbd>
# In an application bundle: copy the files, sign realsense-usbd, run this script, then sign the bundle (the bundle
# signature seals Resources, so the manifest must exist by then).
#
# Exit status: 0 written, 1 error (message on stderr, nothing written), 2 usage.
set -euo pipefail
unset CDPATH
export PATH=/usr/bin:/bin:/usr/sbin:/sbin

ME=make-service-manifest
LABEL=io.github.dolegadolegowski.realsense-usbd
MANIFEST_VERSION=2

die() { echo "$ME: error: $*" >&2; exit 1; }

if [ $# -ne 2 ] || [ -z "$1" ] || [ -z "$2" ]; then
    echo "usage: $0 <signed realsense-usbd> <out service.plist>" >&2
    exit 2
fi
OUT="$2"

# The daemon: a regular arm64 executable (what the installer accepts), by its absolute path so that it can be run
[ -f "$1" ] && [ ! -L "$1" ] || die "$1 is not a regular file"
BIN="$(cd "$(dirname -- "$1")" && pwd -P)/$(basename -- "$1")"
[ -x "$BIN" ] || die "$BIN is not executable"
case "$(file -b "$BIN")" in
    "Mach-O 64-bit executable arm64"*) ;;
    *) die "$BIN is not an arm64 Mach-O executable" ;;
esac

# Signature. The linker signs every arm64 binary ad hoc ("linker-signed"); that is not the signature the application
# ships, and signing later would change the cdhash written here.
SIGN_HINT="sign it first: codesign --force --sign - -o runtime -i $LABEL <realsense-usbd>"
codesign --verify --strict "$BIN" 2>/dev/null || die "$BIN is not signed or its signature is not valid ($SIGN_HINT)"
SIGNATURE="$(codesign -dvvv --arch arm64 "$BIN" 2>&1)" || die "cannot read the signature of $BIN"
case "$SIGNATURE" in
    *"linker-signed"*) die "$BIN is not signed, it only has the linker's signature ($SIGN_HINT)" ;;
esac
CDHASH="$(printf '%s\n' "$SIGNATURE" | sed -n 's/^CDHash=//p')"
[[ "$CDHASH" =~ ^[0-9a-f]{40}$ ]] || die "$BIN: no arm64 cdhash in its signature ($SIGN_HINT)"

# realsense-usbd --version: exactly one line "realsense-usbd <service version> protocol <min>-<max>", plain decimal
# numbers, service version 2 or more, 1 <= min <= max <= 999
VERSION_LINE="$("$BIN" --version 2>/dev/null)" || die "$BIN --version failed"
RE_VERSION='^realsense-usbd ([1-9][0-9]{0,8}) protocol ([1-9][0-9]{0,2})-([1-9][0-9]{0,2})$'
[[ "$VERSION_LINE" =~ $RE_VERSION ]] || die "$BIN --version printed something unexpected: $VERSION_LINE"
SERVICE_VERSION="${BASH_REMATCH[1]}"
PROTOCOL_MIN="${BASH_REMATCH[2]}"
PROTOCOL_MAX="${BASH_REMATCH[3]}"
[ "$SERVICE_VERSION" -ge 2 ] || die "$BIN --version: service version $SERVICE_VERSION (must be 2 or more)"
[ "$PROTOCOL_MIN" -le "$PROTOCOL_MAX" ] || die "$BIN --version: bad protocol range $PROTOCOL_MIN-$PROTOCOL_MAX"

# Written next to the destination and moved into place, so a failure leaves no partial manifest
OUT_DIR="$(dirname -- "$OUT")"
[ -d "$OUT_DIR" ] || die "$OUT_DIR is not a directory"
[ ! -d "$OUT" ] || die "$OUT is a directory"
TMP="$(mktemp "$OUT_DIR/.service.plist.XXXXXX")"
trap 'rm -f -- "$TMP"' EXIT
trap 'exit 1' HUP INT TERM
plutil -create xml1 "$TMP"
plutil -insert Version -integer "$MANIFEST_VERSION" "$TMP"
plutil -insert ServiceVersion -integer "$SERVICE_VERSION" "$TMP"
plutil -insert ProtocolMin -integer "$PROTOCOL_MIN" "$TMP"
plutil -insert ProtocolMax -integer "$PROTOCOL_MAX" "$TMP"
plutil -insert DaemonCDHash -string "$CDHASH" "$TMP"
plutil -lint -s "$TMP" >/dev/null || die "the generated manifest is not a valid property list"
chmod 644 "$TMP"
mv -f -- "$TMP" "$OUT"
echo "$ME: $OUT: realsense-usbd $SERVICE_VERSION, protocol $PROTOCOL_MIN-$PROTOCOL_MAX, cdhash $CDHASH"
