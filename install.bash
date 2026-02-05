#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build"}"
PREFIX="${PREFIX:-/usr/local/hakoniwa}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
HAKO_PDU_ENDPOINT_PREFIX="${HAKO_PDU_ENDPOINT_PREFIX:-$PREFIX}"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DHAKO_PDU_ENDPOINT_PREFIX="$HAKO_PDU_ENDPOINT_PREFIX"

cmake --build "$BUILD_DIR" --config "$CMAKE_BUILD_TYPE"
cmake --install "$BUILD_DIR" --config "$CMAKE_BUILD_TYPE"
