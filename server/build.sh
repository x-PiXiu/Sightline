#!/usr/bin/env bash
# Sightline 服务器统一构建入口（第 2 期起）
# 用法：./build.sh [server|deps|test|all]   （默认 all：server → deps → 单测）
# ep08 步骤 1：deps 只跑依赖探针；SIGHTLINE_REQUIRE_DEPS=1 时缺库判失败（§5.7 lite 默认零前置）
# 旗标说明：-Werror=switch 把"switch 漏枚举 case"从警告升级为编译错误——
#          FrameWriter 对领域事件的穷尽性由编译器强制（D2 决策：A 方案，2026-10-09）
set -euo pipefail
cd "$(dirname "$0")"

CXX=${CXX:-g++}
FLAGS="-std=c++20 -Wall -Wswitch -Werror=switch -I include"
# 注：不加 -Wextra——其 missing-field-initializers 对 Event 的"部分初始化+成员默认值"模式是噪音

DEPS_FLAGS=""
if [[ "${SIGHTLINE_REQUIRE_DEPS:-0}" == "1" ]]; then
    DEPS_FLAGS="-DSIGHTLINE_REQUIRE_DEPS=1"
fi

# 步骤 4：lua5.4 + vendored sol2 条件接入（缺库=零依赖 lite 构建照跑·E2 同哲学；
# scripting_lua.cpp 有 #if 守卫，缺 lua 时 glob 进来也是空 TU）
LUA_LIB=""
LUA_INC=""
for d in /usr/include/lua5.4 /usr/local/include/lua5.4; do
    [ -f "$d/lua.h" ] && LUA_INC="-I$d"
done
if [ -n "$LUA_INC" ] && [ -f third_party/sol2/include/sol/sol.hpp ]; then
    FLAGS="$FLAGS $LUA_INC -I third_party/sol2/include -DSIGHTLINE_HAS_LUA=1"
    LUA_LIB="-llua5.4"
    echo "· Lua 5.4 + sol2 已接入（步骤 4）"
else
    echo "· Lua 未接入（缺 lua5.4 或 sol2——零依赖 lite 构建）"
fi

build_deps() {
    # shellcheck disable=SC2086
    $CXX $FLAGS $DEPS_FLAGS tests/deps_smoke.cpp -o /tmp/deps_smoke
    /tmp/deps_smoke
    echo "✓ deps_smoke 完成"
}

build_server() {
    # shellcheck disable=SC2086
    $CXX $FLAGS main.cpp src/net/*.cpp src/logger/*.cpp src/lobby/lobby_service.cpp \
        src/match/match_service.cpp src/adapters/*.cpp \
        -o sightline_server -lpthread $LUA_LIB
    echo "✓ sightline_server 编译完成"
}

build_test() {
    # shellcheck disable=SC2086
    $CXX $FLAGS tests/test_lobby.cpp src/lobby/lobby_service.cpp -o /tmp/test_lobby
    /tmp/test_lobby
    # shellcheck disable=SC2086
    $CXX $FLAGS tests/test_match.cpp src/match/match_service.cpp src/lobby/lobby_service.cpp \
        src/logger/logger.cpp -o /tmp/test_match   # logger：BotDriver 日志依赖（2026-11-05 勘误——
                                                   #  对齐 test 头注释标准行，此前漏致链接失败）
    /tmp/test_match
    # shellcheck disable=SC2086
    $CXX $FLAGS tests/test_wire.cpp -o /tmp/test_wire     # v4 wire 结构体（header-only）
    /tmp/test_wire
    # shellcheck disable=SC2086
    $CXX $FLAGS tests/test_ports.cpp -o /tmp/test_ports   # ep08 步骤 2：端口/值对象/纯策略（零依赖）
    /tmp/test_ports
    # shellcheck disable=SC2086
    $CXX $FLAGS tests/test_config.cpp src/adapters/*.cpp src/logger/logger.cpp -o /tmp/test_config $LUA_LIB  # 步骤 3：配置体系（logger=scripting_lua 的日志依赖·步骤 4 连带）
    /tmp/test_config
    if [ -n "$LUA_INC" ]; then
        # shellcheck disable=SC2086
        $CXX $FLAGS tests/test_lua_policy.cpp src/adapters/scripting_lua.cpp src/logger/logger.cpp \
            -o /tmp/test_lua_policy $LUA_LIB   # 步骤 4：Lua 策略对拍/门禁
        /tmp/test_lua_policy
    fi
    # shellcheck disable=SC2086
    $CXX $FLAGS tests/test_v48.cpp src/match/match_service.cpp -o /tmp/test_v48  # 步骤 6：契约 v4.8
    /tmp/test_v48
}

case "${1:-all}" in
    server) build_server ;;
    deps)   build_deps ;;
    test)   build_test ;;
    all)    build_server; build_deps; build_test ;;
    *)      echo "用法: ./build.sh [server|deps|test|all]"; exit 2 ;;
esac
