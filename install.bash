#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build"}"
PREFIX="${PREFIX:-/usr/local/hakoniwa}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
HAKO_PDU_ENDPOINT_PREFIX="${HAKO_PDU_ENDPOINT_PREFIX:-$PREFIX}"
PY_PACKAGE_DIR=python
PY_INSTALL_DIR="${PREFIX}/share/hakoniwa-pdu-rpc/python"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DHAKO_PDU_ENDPOINT_PREFIX="$HAKO_PDU_ENDPOINT_PREFIX"

cmake --build "$BUILD_DIR" --config "$CMAKE_BUILD_TYPE"
cmake --install "$BUILD_DIR" --config "$CMAKE_BUILD_TYPE"

if [[ -d "${ROOT_DIR}/${PY_PACKAGE_DIR}/hakoniwa_pdu_rpc" ]]; then
  echo "Installing Python package to ${PY_INSTALL_DIR}"
  install -d "${PY_INSTALL_DIR}"
  cp -R "${ROOT_DIR}/${PY_PACKAGE_DIR}/hakoniwa_pdu_rpc" "${PY_INSTALL_DIR}/"
  if [[ -d "${ROOT_DIR}/config/schema" ]]; then
    install -d "${PY_INSTALL_DIR}/hakoniwa_pdu_rpc/schema"
    cp -R "${ROOT_DIR}/config/schema/." "${PY_INSTALL_DIR}/hakoniwa_pdu_rpc/schema/"
  fi
fi

echo "For python -m usage:"
echo "  export PYTHONPATH=\"${PY_INSTALL_DIR}:\$PYTHONPATH\""
