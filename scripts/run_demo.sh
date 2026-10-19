#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v cmake >/dev/null 2>&1; then
    echo "[EdgeGuard] CMake was not found. Install CMake 3.16+ and a C++17 compiler first."
    exit 1
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel

if [[ -x ./build/edgeguard ]]; then
    exec ./build/edgeguard run --config configs/edgeguard.conf --with-simulator
fi
exec ./build/Release/edgeguard run --config configs/edgeguard.conf --with-simulator
