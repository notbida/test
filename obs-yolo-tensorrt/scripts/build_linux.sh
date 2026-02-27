#!/usr/bin/env bash
# build_linux.sh  –  One-shot build script for Linux (Ubuntu 20.04/22.04)
#
# Prerequisites:
#   - NVIDIA driver ≥ 525
#   - CUDA 11.8 or 12.x  (from NVIDIA CUDA repo)
#   - TensorRT 8.6+ or 9.x  (from NVIDIA TensorRT repo)
#   - OBS Studio dev headers  (obs-dev or build from source)
#   - OpenCV 4.x  (libopencv-dev)
#   - CMake ≥ 3.22
#
# Usage:
#   chmod +x scripts/build_linux.sh
#   ./scripts/build_linux.sh [--cuda-arch "75;86;89"] [--trt-root /path/to/trt]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "${SCRIPT_DIR}")"
BUILD_DIR="${ROOT_DIR}/build"

CUDA_ARCH="75;86;89"
TRT_ROOT=""
BUILD_TYPE="Release"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --cuda-arch)   CUDA_ARCH="$2";  shift 2 ;;
    --trt-root)    TRT_ROOT="$2";   shift 2 ;;
    --debug)       BUILD_TYPE="Debug"; shift ;;
    *)             echo "Unknown arg: $1"; exit 1 ;;
  esac
done

echo "=== obs-yolo-tensorrt Linux build ==="
echo "  Build type : ${BUILD_TYPE}"
echo "  CUDA archs : ${CUDA_ARCH}"
echo "  TRT root   : ${TRT_ROOT:-auto-detect}"
echo "  Build dir  : ${BUILD_DIR}"
echo ""

# Detect TensorRT if not specified
if [[ -z "${TRT_ROOT}" ]]; then
  for candidate in /usr/local/tensorrt /opt/TensorRT /usr/include; do
    if [[ -f "${candidate}/include/NvInfer.h" || -f "${candidate}/NvInfer.h" ]]; then
      TRT_ROOT="$(dirname "$(find ${candidate} -name NvInfer.h | head -1)")"
      TRT_ROOT="$(realpath "${TRT_ROOT}/..")"
      echo "  Auto-detected TRT at: ${TRT_ROOT}"
      break
    fi
  done
fi

CMAKE_EXTRA_ARGS=()
if [[ -n "${TRT_ROOT}" ]]; then
  CMAKE_EXTRA_ARGS+=(-DTRT_ROOT="${TRT_ROOT}")
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake "${ROOT_DIR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCMAKE_CUDA_ARCHITECTURES="${CUDA_ARCH}" \
  "${CMAKE_EXTRA_ARGS[@]}"

cmake --build . --parallel "$(nproc)"

echo ""
echo "=== Build succeeded ==="
echo "  Plugin:   ${BUILD_DIR}/obs-yolo-tensorrt.so"
echo "  colorbot: ${BUILD_DIR}/colorbot"
echo ""
echo "Install plugin:"
echo "  sudo cmake --install ${BUILD_DIR}"
echo ""
echo "Or manually:"
echo "  sudo cp ${BUILD_DIR}/obs-yolo-tensorrt.so /usr/lib/obs-plugins/"
