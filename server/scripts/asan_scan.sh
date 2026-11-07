#!/usr/bin/env bash
# asan_scan.sh —— ep08：ASan+UBSan 全测试扫描（pkg-config 动态取库路径）
set -u
cd /mnt/e/workspace/Demo/Sightline/server || exit 1

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"
BASE="-std=c++20 -I include -DSIGHTLINE_HAS_LUA=1 -DSIGHTLINE_HAS_DB=1"
LUAINC=$(pkg-config --cflags lua5.4 2>/dev/null)
LUALIB=$(pkg-config --libs lua5.4 2>/dev/null)
DBINC=$(pkg-config --cflags libmongoc-1.0 2>/dev/null)" -I/usr/include/mysql"
DBLIB=$(pkg-config --libs libmongoc-1.0 2>/dev/null)" -lmysqlclient -lhiredis"
INC="$BASE $LUAINC $DBINC -I third_party/sol2/include"
LIBS="$LUALIB $DBLIB -lpthread"
echo "INC=$INC"; echo "LIBS=$LIBS"

run() {
    local name="$1"; shift
    echo "══ $name ══"
    g++ $SAN $INC "$@" $LIBS -o "/tmp/asan_$name" 2>/tmp/cc_err.log
    if [ $? -ne 0 ]; then echo "  ✗ 编译失败"; tail -3 /tmp/cc_err.log; return 1; fi
    "/tmp/asan_$name" > "/tmp/asan_${name}_out.log" 2>&1
    local rc=$?
    if grep -q "ERROR: AddressSanitizer\|ERROR: LeakSanitizer\|runtime error" "/tmp/asan_${name}_out.log"; then
        echo "  ✗ 检出问题（rc=$rc）"
        grep -m3 -A5 "ERROR\|runtime error" "/tmp/asan_${name}_out.log"
    else
        echo "  ✓ 干净（rc=$rc）"
    fi
}

run test_lobby    tests/test_lobby.cpp src/lobby/lobby_service.cpp src/logger/logger.cpp
run test_match    tests/test_match.cpp src/match/match_service.cpp src/lobby/lobby_service.cpp src/logger/logger.cpp
run test_wire     tests/test_wire.cpp
run test_ports    tests/test_ports.cpp src/logger/logger.cpp
run test_config   tests/test_config.cpp src/adapters/config_service.cpp src/adapters/audit_wal.cpp src/logger/logger.cpp
run test_persistence tests/test_persistence.cpp src/adapters/persistence.cpp src/adapters/mysql_store.cpp \
    src/adapters/redis_store.cpp src/adapters/mongo_store.cpp src/adapters/audit_wal.cpp \
    src/adapters/config_service.cpp src/logger/logger.cpp
run test_v48      tests/test_v48.cpp src/match/match_service.cpp src/lobby/lobby_service.cpp \
    src/adapters/config_service.cpp src/adapters/audit_wal.cpp src/logger/logger.cpp
run test_lua_policy tests/test_lua_policy.cpp src/adapters/scripting_lua.cpp src/logger/logger.cpp
echo "══ 扫描完成 ══"
