#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_ROOT}/build"
BUILD_TYPE="${BUILD_TYPE:-Debug}"

set -e

echo "=== Build Configuration ==="
echo "Project Root: ${PROJECT_ROOT}"
echo "Build Directory: ${BUILD_DIR}"
echo "Build Type: ${BUILD_TYPE}"
echo "VCPKG_ROOT: ${VCPKG_ROOT}"
echo "==========================="

if [ ! -d "${BUILD_DIR}" ]; then
    echo "Creating build directory..."
    mkdir -p "${BUILD_DIR}"
fi

cd "${BUILD_DIR}"

# Prefer an explicit LLVM_DIR, then llvm-config, then a couple of common paths.
if [ -z "${LLVM_DIR}" ]; then
    if command -v llvm-config >/dev/null 2>&1; then
        LLVM_DIR="$(llvm-config --cmakedir)"
    elif [ -d /usr/lib/cmake/llvm ]; then
        LLVM_DIR="/usr/lib/cmake/llvm"
    elif [ -d /opt/homebrew/opt/llvm/lib/cmake/llvm ]; then
        LLVM_DIR="/opt/homebrew/opt/llvm/lib/cmake/llvm"
    fi
fi

CMAKE_ARGS=(
    ".."
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    "-DLLVM_DIR=${LLVM_DIR}"
)

if [ -n "${VCPKG_ROOT}" ] && [ -f "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" ]; then
    CMAKE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
fi

echo "Running CMake configuration (LLVM_DIR=${LLVM_DIR})..."
cmake "${CMAKE_ARGS[@]}"

JOBS="$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
echo "Building project with ${JOBS} jobs..."
cmake --build . --config "${BUILD_TYPE}" -j"${JOBS}"

echo "Build completed successfully!"
echo "Executable: ${BUILD_DIR}/bin/cdd"
