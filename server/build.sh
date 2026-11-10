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

# 步骤 5：三库客户端条件接入（mysql+hiredis+libmongoc 三件齐才开——缺一即 lite，
# 与 E2「脚本是增强不是依赖」同哲学；persistence/store .cpp 有 #if 守卫=空 TU）
DB_LIBS=""
if [ -f /usr/include/mysql/mysql.h ] && [ -f /usr/include/hiredis/hiredis.h ] \
   && pkg-config --exists libmongoc-1.0 2>/dev/null; then
    FLAGS="$FLAGS -DSIGHTLINE_HAS_DB=1 $(pkg-config --cflags libmongoc-1.0)"
    DB_LIBS="$(pkg-config --libs libmongoc-1.0) -lmysqlclient -lhiredis"
    echo "· 三库客户端已接入（步骤 5·mysql+hiredis+libmongoc）"
else
    echo "· 三库未接入（缺 mysql/hiredis/libmongoc 之一——lite 构建）"
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
        src/match/match_session.cpp src/adapters/*.cpp \
        -o sightline_server -lpthread $LUA_LIB $DB_LIBS
    echo "✓ sightline_server 编译完成"
}

build_test() {
    # shellcheck disable=SC2086
    $CXX $FLAGS tests/test_lobby.cpp src/lobby/lobby_service.cpp -o /tmp/test_lobby
    /tmp/test_lobby
    # shellcheck disable=SC2086
    $CXX $FLAGS tests/test_match.cpp src/match/match_session.cpp src/lobby/lobby_service.cpp \
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
    $CXX $FLAGS tests/test_config.cpp src/adapters/*.cpp src/logger/logger.cpp -o /tmp/test_config $LUA_LIB $DB_LIBS  # 步骤 3：配置体系（logger/db=步骤 4/5 连带）
    /tmp/test_config
    if [ -n "$LUA_INC" ]; then
        # shellcheck disable=SC2086
        $CXX $FLAGS tests/test_lua_policy.cpp src/adapters/scripting_lua.cpp src/logger/logger.cpp \
            -o /tmp/test_lua_policy $LUA_LIB   # 步骤 4：Lua 策略对拍/门禁
        /tmp/test_lua_policy
    fi
    if [ -n "$DB_LIBS" ]; then
        # shellcheck disable=SC2086
        $CXX $FLAGS tests/test_patch_agg.cpp -o /tmp/test_patch_agg
        /tmp/test_patch_agg   # ep09 步骤 4：PatchAggregator 两阶段收口（G5·纯逻辑零 IO）
        $CXX $FLAGS tests/test_shards.cpp src/match/match_session.cpp -o /tmp/test_shards
        /tmp/test_shards   # ep09 步骤 2：多房容器+Mailbox（G1/G3/E8 契约·零 IO）
        $CXX $FLAGS -pthread tests/test_workers.cpp src/match/match_session.cpp src/lobby/lobby_service.cpp src/logger/logger.cpp -o /tmp/test_workers $LUA_LIB
        /tmp/test_workers   # ep09 步骤 3：WorkerPool 并发（真线程·TSan L1 靶场）
        $CXX $FLAGS tests/test_persistence.cpp src/adapters/persistence.cpp \
            src/adapters/mysql_store.cpp src/adapters/redis_store.cpp src/adapters/mongo_store.cpp \
            src/adapters/audit_wal.cpp src/logger/logger.cpp \
            -o /tmp/test_persistence $LUA_LIB $DB_LIBS   # 步骤 5：持久化纯单测（不连库）
        /tmp/test_persistence
        # shellcheck disable=SC2086
        $CXX $FLAGS tests/test_db_integration.cpp src/adapters/persistence.cpp src/adapters/mysql_store.cpp src/adapters/redis_store.cpp src/adapters/mongo_store.cpp src/adapters/audit_wal.cpp src/logger/logger.cpp -o /tmp/test_db_integration $LUA_LIB $DB_LIBS   # 步骤 5：真库冒烟（env 门控·缺省跳过）
        /tmp/test_db_integration
    fi
    # shellcheck disable=SC2086
    $CXX $FLAGS tests/test_v48.cpp src/match/match_session.cpp -o /tmp/test_v48  # 步骤 6：契约 v4.8
    /tmp/test_v48
}

case "${1:-all}" in
    server) build_server ;;
    deps)   build_deps ;;
    test)   build_test ;;
    all)    build_server; build_deps; build_test ;;
    *)      echo "用法: ./build.sh [server|deps|test|all]"; exit 2 ;;
esac
