#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build"}"
MANIFEST="$BUILD_DIR/install_manifest.txt"

if [[ ! -f "$MANIFEST" ]]; then
  echo "install manifest not found: $MANIFEST" >&2
  echo "Run install.bash first so CMake can record installed files." >&2
  exit 1
fi

while IFS= read -r file; do
  if [[ -e "$file" || -L "$file" ]]; then
    rm -f "$file"
  fi
done < "$MANIFEST"
