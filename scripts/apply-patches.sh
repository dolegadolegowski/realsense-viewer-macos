#!/bin/bash
# Applies the macOS patches to the librealsense submodule (idempotent): patches/*.patch, then the camera service
# client that the kit carries for every application (macos/broker/librealsense/macos-usb-broker.patch).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/librealsense"

for patch in "$ROOT"/patches/*.patch "$ROOT/macos/broker/librealsense/macos-usb-broker.patch"; do
    name="$(basename "$patch")"
    [ -f "$patch" ] || { echo "error: $patch not found" >&2; exit 1; }
    if git -C "$SRC" apply --reverse --check "$patch" 2>/dev/null; then
        echo "patch already applied: $name"
    else
        git -C "$SRC" apply --whitespace=nowarn "$patch"
        echo "patch applied: $name"
    fi
done
