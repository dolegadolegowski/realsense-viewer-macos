#!/bin/bash
# Applies patches/*.patch to the librealsense submodule (idempotent).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/librealsense"

for patch in "$ROOT"/patches/*.patch; do
    name="$(basename "$patch")"
    if git -C "$SRC" apply --reverse --check "$patch" 2>/dev/null; then
        echo "patch already applied: $name"
    else
        git -C "$SRC" apply --whitespace=nowarn "$patch"
        echo "patch applied: $name"
    fi
done
