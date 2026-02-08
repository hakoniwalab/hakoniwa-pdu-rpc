#!/usr/bin/env bash
set -euo pipefail

PREFIX=${PREFIX:-/usr/local/hakoniwa}
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build"}"
PY_INSTALL_DIR="${PREFIX}/share/hakoniwa-pdu-rpc/python"
CM_INSTALL_DIR="${PREFIX}/lib/cmake/hakoniwa_pdu_rpc"

say() {
  printf "%s\n" "$*"
}

if [[ ! -w "${PREFIX}" ]]; then
  say "Note: writing to ${PREFIX} usually requires sudo."
fi

MANIFEST="$BUILD_DIR/install_manifest.txt"

if [[ -f "$MANIFEST" ]]; then
  say "Removing CMake-installed files using ${BUILD_DIR}/install_manifest.txt"
  while IFS= read -r file; do
    if [[ -e "$file" || -L "$file" ]]; then
      rm -f "$file"
    fi
  done < "$MANIFEST"
else
  say "install_manifest.txt not found; falling back to header cleanup."
  say "Removing installed headers from ${PREFIX}/include"
  while IFS= read -r src; do
    rel=${src#"${ROOT_DIR}/include/"}
    dest="${PREFIX}/include/${rel}"
    if [[ -f "${dest}" ]]; then
      rm -f "${dest}"
    fi
  done < <(find "${ROOT_DIR}/include" -type f \( -name "*.h" -o -name "*.hpp" \) | sort)
fi

if [[ -d "${PREFIX}/include/hakoniwa" ]]; then
  find "${PREFIX}/include/hakoniwa" -type d -empty -delete
fi

if [[ -d "${CM_INSTALL_DIR}" ]]; then
  find "${CM_INSTALL_DIR}" -type d -empty -delete
fi

if [[ -d "${PY_INSTALL_DIR}" ]]; then
  say "Removing Python package from ${PY_INSTALL_DIR}"
  rm -rf "${PY_INSTALL_DIR}"
fi
