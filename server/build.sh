#!/usr/bin/env bash
# Sightline 服务器统一构建入口（第 2 期起）
# 用法：./build.sh [server|test|all]   （默认 all：先 server 后单测）
# 旗标说明：-Werror=switch 把"switch 漏枚举 case"从警告升级为编译错误——
#          FrameWriter 对领域事件的穷尽性由编译器强制（D2 决策：A 方案，2026-10-09）
set -euo pipefail
cd "$(dirname "$0")"

CXX=${CXX:-g++}
FLAGS="-std=c++20 -Wall -Wswitch -Werror=switch -I include"
# 注：不加 -Wextra——其 missing-field-initializers 对 Event 的"部分初始化+成员默认值"模式是噪音

build_server() {
    # shellcheck disable=SC2086
    $CXX $FLAGS main.cpp src/net/*.cpp src/logger/*.cpp src/lobby/lobby_service.cpp \
        -o sightline_server -lpthread
    echo "✓ sightline_server 编译完成"
}

build_test() {
    # shellcheck disable=SC2086
    $CXX $FLAGS tests/test_lobby.cpp src/lobby/lobby_service.cpp -o /tmp/test_lobby
    /tmp/test_lobby
}

case "${1:-all}" in
    server) build_server ;;
    test)   build_test ;;
    all)    build_server; build_test ;;
    *)      echo "用法: ./build.sh [server|test|all]"; exit 2 ;;
esac
