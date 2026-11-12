cmake_minimum_required(VERSION 3.16)
project(SightlineServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# 旗标与 build.sh 一致：-Werror=switch 强制 FrameWriter 事件穷尽（D2-17）
# 注：不上 -Wextra——missing-field-initializers 对 Event 部分初始化模式是噪音
add_compile_options(-Wall -Wswitch -Werror=switch)

find_package(Threads REQUIRED)
# ep09 步骤 5（ADR-22）：Pbkdf2Hasher（IPasswordHasher 实现·header-only）需要 OpenSSL libcrypto——
# 账号域与 DB 开关解耦（main 无条件构造 AccountService→vtable→EVP_sha256 符号）；DB 关时哈希不执行但符号必须可解析
find_library(CRYPTO_LIB NAMES crypto)
if(NOT CRYPTO_LIB)
    message(FATAL_ERROR "ep09 步骤 5：找不到 OpenSSL libcrypto（libssl-dev）——账号域 PBKDF2 硬依赖")
endif()
find_package(PkgConfig QUIET)

# ============ ep08 步骤 1：外部依赖（可选 · lite 默认零前置）============
# 整洁架构：五个第三方库只挂最外层适配器；实体 / 用例 / 核心 / 全部单测零依赖，
# 缺库环境照样 configure + build + ctest。显式打开开关时才要求库真实存在。
# 双模式开关见 docs/episodes/ep08/设计草案.md §5.7。
# —— 依赖探测（决定未显式指定时的默认值）——
# 设计（2026-11-07）：本仓库公开可克隆——裸机缺库保持零依赖可编（lite 模式 ctest 照绿）；
# 库齐环境（开发机/演示机/CLion）自动全开，不填 option 即得完整构建。显式 -D 永远优先。
include(FindPkgConfig OPTIONAL)
set(SIGHTLINE_DEPS_DB_FOUND OFF)
if(PkgConfig_FOUND)
    pkg_check_modules(HIREDIS_QUIET QUIET hiredis)
    pkg_check_modules(MONGOC_QUIET  QUIET libmongoc-1.0)
    find_path(MYSQL_INC_QUIET NAMES mysql/mysql.h)
    find_library(MYSQL_LIB_QUIET NAMES mysqlclient)
    if(HIREDIS_QUIET_FOUND AND MONGOC_QUIET_FOUND AND MYSQL_INC_QUIET AND MYSQL_LIB_QUIET)
        set(SIGHTLINE_DEPS_DB_FOUND ON)
    endif()
endif()
find_package(Lua 5.4 QUIET)
set(SIGHTLINE_DEPS_LUA_FOUND ${LUA_FOUND})   # sol2=vendored 恒在，只探 lua5.4

option(SIGHTLINE_ENABLE_LUA   "启用 Lua5.4 + sol2 策略适配层（默认=探测到 lua5.4 即开）" ${SIGHTLINE_DEPS_LUA_FOUND})
option(SIGHTLINE_ENABLE_DB    "启用 hiredis/libmysqlclient/libmongoc 持久化（默认=三库探测齐即开）" ${SIGHTLINE_DEPS_DB_FOUND})
option(SIGHTLINE_REQUIRE_DEPS "deps_smoke 缺任一依赖即判失败（装好库后开启）" OFF)

set(SIGHTLINE_OPTIONAL_LIBS "")
set(SIGHTLINE_OPTIONAL_INCS "")

if(SIGHTLINE_ENABLE_LUA)
    find_package(Lua 5.4 REQUIRED)
    # sol2 默认用仓库 vendored 版（third_party/sol2·v3.3.1 版本锁死·步骤 1 入仓）
    if(NOT SOL2_INCLUDE_DIR)
        set(SOL2_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sol2/include)
    endif()
    if(NOT EXISTS "${SOL2_INCLUDE_DIR}/sol/sol.hpp")
        message(FATAL_ERROR "SIGHTLINE_ENABLE_LUA=ON 但找不到 sol/sol.hpp（设 SOL2_INCLUDE_DIR 或拉 third_party/sol2）")
    endif()
    list(APPEND SIGHTLINE_OPTIONAL_LIBS ${LUA_LIBRARIES})
    list(APPEND SIGHTLINE_OPTIONAL_INCS ${LUA_INCLUDE_DIR} ${SOL2_INCLUDE_DIR})
endif()

if(SIGHTLINE_ENABLE_DB)
    if(NOT PkgConfig_FOUND)
        message(FATAL_ERROR "SIGHTLINE_ENABLE_DB=ON 需要 pkg-config")
    endif()
    # 步骤 5 实勘对齐（2026-11-06）：实际用 hiredis + libmysqlclient(C) + libmongoc(C·方案 B)
    # ——原稿的 libmariadbcpp/libmongocxx 是 C++ 连接器，本实现未用（头文件前置声明零依赖）。
    pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
    pkg_check_modules(MONGOC   REQUIRED IMPORTED_TARGET libmongoc-1.0)
    find_path(MYSQL_INCLUDE_DIR NAMES mysql/mysql.h)
    find_library(MYSQL_CLIENT_LIB NAMES mysqlclient)
    if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_CLIENT_LIB)
        message(FATAL_ERROR "SIGHTLINE_ENABLE_DB=ON 但找不到 mysql 客户端（libmysqlclient-dev）")
    endif()
    list(APPEND SIGHTLINE_OPTIONAL_LIBS PkgConfig::HIREDIS PkgConfig::MONGOC ${MYSQL_CLIENT_LIB})
    list(APPEND SIGHTLINE_OPTIONAL_INCS ${MYSQL_INCLUDE_DIR})
endif()

message(STATUS "[ep08] Lua=${SIGHTLINE_ENABLE_LUA} DB=${SIGHTLINE_ENABLE_DB} REQUIRE_DEPS=${SIGHTLINE_REQUIRE_DEPS}")

# ============ 底座：第 1 期网络库（前任遗产收敛版）+ 日志 ============
add_library(sightline_core STATIC
    src/net/channel.cpp
    src/net/epoll.cpp
    src/net/event_loop.cpp
    src/net/heap_scheduler.cpp
    src/net/inet_address.cpp
    src/net/socket.cpp
    src/logger/logger.cpp
)
target_include_directories(sightline_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sightline_core PUBLIC Threads::Threads)

# ============ 第 2 期：业务用例层（整洁架构内层——零 net 依赖，可纯单测） ============
add_library(sightline_lobby STATIC
    src/lobby/lobby_service.cpp
)
target_include_directories(sightline_lobby PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# 依赖规则自检：sightline_lobby 不得链接/包含 net —— 违反会在 CI grep 或 code review 抓出

# ============ 第 3 期：对局用例层（与 lobby 并列——D3-15：只认 PlayerId，与 lobby 互不 include） ============
add_library(sightline_match STATIC
    src/match/match_session.cpp
)
target_include_directories(sightline_match PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# 依赖规则自检：sightline_match 不得链接/包含 net，也不得包含 lobby——组合归适配层 wireMatch

# ============ ep08 步骤 3：适配器实现（首批 src/adapters/*.cpp）============
# 只挂最外层；实体 / 用例 / 全部单测仍零外部依赖。新增适配器 .cpp 需在此显式登记。
add_library(sightline_adapters STATIC
    src/adapters/config_service.cpp
    src/adapters/audit_wal.cpp            # 零依赖（lite 审计 WAL 也用）——无条件入编
)
target_include_directories(sightline_adapters PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# 适配器依赖底座（Logger）——build.sh 手工链接掩掉了这层；CMake 按库边界必须显式声明
target_link_libraries(sightline_adapters PUBLIC sightline_core)
# ep09 步骤 5：adapters（persistence.h→password_pbkdf2.h）PUBLIC 挂 crypto——
# 静态库传递链接：server/test_config/test_persistence 等链接方自动获得 -lcrypto（DSO missing 根除）
target_link_libraries(sightline_adapters PUBLIC ${CRYPTO_LIB})
# 步骤 5：三库翻译单元（文件级 #if SIGHTLINE_HAS_DB 守卫——宏由本开关统一给）
if(SIGHTLINE_ENABLE_DB)
    target_sources(sightline_adapters PRIVATE
        src/adapters/persistence.cpp
        src/adapters/mysql_store.cpp
        src/adapters/redis_store.cpp
        src/adapters/mongo_store.cpp)
    target_compile_definitions(sightline_adapters PUBLIC SIGHTLINE_HAS_DB=1)
    if(SIGHTLINE_OPTIONAL_INCS)
        target_include_directories(sightline_adapters PUBLIC ${SIGHTLINE_OPTIONAL_INCS})
    endif()
    if(SIGHTLINE_OPTIONAL_LIBS)
        target_link_libraries(sightline_adapters PUBLIC ${SIGHTLINE_OPTIONAL_LIBS})
    endif()
endif()
# 步骤 4：Lua 适配层仅在 ENABLE_LUA=ON 时入编（宏同时门控 main.cpp 的接线与测试注册）
if(SIGHTLINE_ENABLE_LUA)
    target_sources(sightline_adapters PRIVATE src/adapters/scripting_lua.cpp)
    target_compile_definitions(sightline_adapters PUBLIC SIGHTLINE_HAS_LUA=1)
    target_include_directories(sightline_adapters PUBLIC ${LUA_INCLUDE_DIR} ${SOL2_INCLUDE_DIR})
endif()

# ============ 第 3 期：服务器本体（装配根） ============
add_executable(sightline_server
    main.cpp
)
target_link_libraries(sightline_server PRIVATE sightline_lobby sightline_match sightline_core sightline_adapters)
# ep08 步骤 4/5 的适配器 .cpp 落地后，可选库经此处挂到最外层（当前无 .cpp，仅预置接线位）
if(SIGHTLINE_OPTIONAL_LIBS)
    target_link_libraries(sightline_server PRIVATE ${SIGHTLINE_OPTIONAL_LIBS})
endif()
if(SIGHTLINE_OPTIONAL_INCS)
    target_include_directories(sightline_server PRIVATE ${SIGHTLINE_OPTIONAL_INCS})
endif()

# ============ 测试 ============
# ep08：add_test 必须配 enable_testing()，否则 ctest 报 "No tests were found"
enable_testing()

add_executable(test_lobby tests/test_lobby.cpp)
target_link_libraries(test_lobby PRIVATE sightline_lobby)
add_test(NAME test_lobby COMMAND test_lobby)

add_executable(test_match tests/test_match.cpp)
# sightline_core：人机驱动适配器（adapters/bot_driver.h）的日志依赖——驱动器同局回归测试要真身入场
target_link_libraries(test_match PRIVATE sightline_match sightline_lobby sightline_core)
add_test(NAME test_match COMMAND test_match)

add_executable(smoke_base tests/smoke_base.cpp)
target_link_libraries(smoke_base PRIVATE sightline_core)
add_test(NAME smoke_base COMMAND smoke_base)

# v4 wire 结构体单测（header-only；此前只在 build.sh 编译，CMake/ctest 漏注册）
add_executable(test_wire tests/test_wire.cpp)
target_include_directories(test_wire PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME test_wire COMMAND test_wire)

# ============ ep08 步骤 1：依赖探针（零依赖可编译，缺库不算失败）============
add_executable(deps_smoke tests/deps_smoke.cpp)
target_include_directories(deps_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(SIGHTLINE_REQUIRE_DEPS)
    target_compile_definitions(deps_smoke PRIVATE SIGHTLINE_REQUIRE_DEPS=1)
endif()
if(SIGHTLINE_OPTIONAL_INCS)
    target_include_directories(deps_smoke PRIVATE ${SIGHTLINE_OPTIONAL_INCS})
endif()
add_test(NAME deps_smoke COMMAND deps_smoke)

# ============ ep08 步骤 2：端口 / 值对象 / 纯策略单测（零依赖）============
add_executable(test_ports tests/test_ports.cpp)
target_include_directories(test_ports PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME test_ports COMMAND test_ports)

# ============ ep08 步骤 3：配置体系单测（零 Lua VM / 零 DB）============
add_executable(test_config tests/test_config.cpp)
target_link_libraries(test_config PRIVATE sightline_adapters)
add_test(NAME test_config COMMAND test_config)

# ============ ep08 步骤 5：持久化纯单测（零连接·ENABLE_DB 时才编）============
if(SIGHTLINE_ENABLE_DB)
    add_executable(test_persistence tests/test_persistence.cpp)
    target_link_libraries(test_persistence PRIVATE sightline_adapters)
    add_test(NAME test_persistence COMMAND test_persistence)
endif()

# ============ ep08 步骤 6：契约 v4.8 wire + 换枪状态机 ============
add_executable(test_v48 tests/test_v48.cpp)
target_link_libraries(test_v48 PRIVATE sightline_match)
add_test(NAME test_v48 COMMAND test_v48)

# ============ ep09 步骤 2：多房容器+Mailbox 单测（零 IO·header+match_session）============
add_executable(test_shards tests/test_shards.cpp)
target_link_libraries(test_shards PRIVATE sightline_match)
add_test(NAME test_shards COMMAND test_shards)

# ============ ep09 步骤 4：PatchAggregator 两阶段收口（纯逻辑零 IO·header-only）============
add_executable(test_patch_agg tests/test_patch_agg.cpp)
target_include_directories(test_patch_agg PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME test_patch_agg COMMAND test_patch_agg)

# ============ ep09 步骤 5：账号域纯逻辑（PBKDF2 往返·header-only+libcrypto）============
add_executable(test_account tests/test_account.cpp)
target_include_directories(test_account PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_account PRIVATE ${CRYPTO_LIB})
add_test(NAME test_account COMMAND test_account)

# ============ ep09 步骤 8：ADR-20 双模式对拍金标准（按房分片子流逐字节）============
add_executable(test_dualmode_diff tests/test_dualmode_diff.cpp)
target_link_libraries(test_dualmode_diff PRIVATE sightline_match sightline_lobby sightline_core Threads::Threads)
add_test(NAME test_dualmode_diff COMMAND test_dualmode_diff)

# ============ ep09 步骤 3：WorkerPool 并发（真线程·TSan L1 靶场）============
add_executable(test_workers tests/test_workers.cpp)
target_link_libraries(test_workers PRIVATE sightline_match sightline_lobby sightline_core Threads::Threads)
add_test(NAME test_workers COMMAND test_workers)

# ============ ep08 步骤 4：Lua 策略对拍/门禁（ENABLE_LUA 时才编）============
if(SIGHTLINE_ENABLE_LUA)
    add_executable(test_lua_policy tests/test_lua_policy.cpp)
    target_link_libraries(test_lua_policy PRIVATE sightline_adapters sightline_core ${LUA_LIBRARIES})   # core=logger（scripting_lua 日志依赖）
    # WORKING_DIRECTORY=源码根：测试读 assets/scripts/damage.lua 相对路径（build.sh 从源码根跑·同约定）
    add_test(NAME test_lua_policy COMMAND test_lua_policy WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
