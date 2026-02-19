#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-control-center"
MODULE_CACHE_DIR="${BUILD_DIR}/clang-module-cache"

mkdir -p "${MODULE_CACHE_DIR}"
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_Swift_FLAGS="-module-cache-path ${MODULE_CACHE_DIR}"

if [[ "${1:-}" == "--legacy" ]]; then
  shift
  APP_BIN="${BUILD_DIR}/kinect-control-center"
  cmake --build "${BUILD_DIR}" --target kinect-control-center -j4
  export DYLD_LIBRARY_PATH="${SCRIPT_DIR}/../libfreenect/build/lib:${DYLD_LIBRARY_PATH:-}"
  echo "Launching legacy Kinect Control Center..."
  echo "Binary: ${APP_BIN}"
  exec "${APP_BIN}" "$@"
fi

APP_BUNDLE="${BUILD_DIR}/macKinect.app"
CLANG_MODULE_CACHE_PATH="${MODULE_CACHE_DIR}" cmake --build "${BUILD_DIR}" --target macKinect -j4

echo "Launching macKinect..."
echo "Bundle: ${APP_BUNDLE}"
exec open -n "${APP_BUNDLE}"
