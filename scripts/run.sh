#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_ROOT}/build"
EXECUTABLE="${BUILD_DIR}/bin/cmm"

set -e

echo "=== Running Build Script ==="
bash "${SCRIPT_DIR}/build.sh"

echo ""
echo "=== Running Application ==="
if [ -f "${EXECUTABLE}" ]; then
    "${EXECUTABLE}" "$@"
else
    echo "Error: Executable not found at ${EXECUTABLE}"
    exit 1
fi
