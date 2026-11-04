#!/bin/bash
# Builds the deployment bundle that is attached to the GitHub release next to the DMG, and prints its sha256 (the
# README's one-line command pins it). The bundle is reproducible: the same two scripts always give the same bytes
# (fixed order, owner, modes and times, no extended attributes, gzip -n), so anyone can rebuild it from the
# repository and compare the hash.
#
#   scripts/deploy/make-bundle.sh [output.tar.gz]
#       default output: dist/rs-deploy-<pinned tag>.tar.gz
#
# Contents: rs-deploy/deploy-macs.sh and rs-deploy/deploy/remote-install.sh (the layout deploy-macs.sh expects).
set -euo pipefail
unset CDPATH

ROOT="$(cd "$(dirname "$0")/../.." && pwd -P)"
CTL="$ROOT/scripts/deploy-macs.sh"
RI="$ROOT/scripts/deploy/remote-install.sh"
TAG="$(sed -n 's/^PINNED_TAG=//p' "$CTL")"
[[ "$TAG" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || { echo "make-bundle: no PINNED_TAG in $CTL" >&2; exit 1; }
[ "$TAG" = "$(sed -n 's/^PINNED_TAG=//p' "$RI")" ] || { echo "make-bundle: pinned tags differ" >&2; exit 1; }
OUT="${1:-$ROOT/dist/rs-deploy-$TAG.tar.gz}"
/bin/bash -n "$CTL"
/bin/bash -n "$RI"

ST="$(mktemp -d "${TMPDIR:-/tmp}/rs-bundle.XXXXXX")"
trap 'rm -rf -- "$ST"' EXIT
mkdir -p "$ST/rs-deploy/deploy"
cat -- "$CTL" >"$ST/rs-deploy/deploy-macs.sh"
cat -- "$RI" >"$ST/rs-deploy/deploy/remote-install.sh"
chmod 755 "$ST/rs-deploy" "$ST/rs-deploy/deploy" "$ST/rs-deploy/deploy-macs.sh" "$ST/rs-deploy/deploy/remote-install.sh"
find "$ST/rs-deploy" -exec touch -h -t 202601010000.00 {} +
mkdir -p "$(dirname "$OUT")"
(
    cd "$ST"
    COPYFILE_DISABLE=1 tar --format ustar --uid 0 --gid 0 --uname root --gname wheel \
        --no-xattrs --no-acls --no-mac-metadata -n -cf - \
        rs-deploy rs-deploy/deploy-macs.sh rs-deploy/deploy rs-deploy/deploy/remote-install.sh
) | gzip -n -9 >"$OUT.tmp"
mv -f -- "$OUT.tmp" "$OUT"
echo "$(shasum -a 256 "$OUT" | cut -d' ' -f1)  $OUT"
