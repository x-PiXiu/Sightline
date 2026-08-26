#!/usr/bin/env bash
# WSL/Linux 一键构建+测试
set -e
cd "$(dirname "$0")/.."
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release > cmake.log 2>&1 || { tail -20 cmake.log; exit 1; }
if ! make -j"$(nproc)" 2> build.err; then
    echo "=== BUILD FAILED ==="
    grep -E "error|Error" build.err | head -30
    exit 1
fi
echo "=== BUILD OK ==="
echo "=== RUN TESTS ==="
ctest --output-on-failure
