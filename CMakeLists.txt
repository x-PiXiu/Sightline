# Sightline —— 整洁架构分层的单根 CMake（相比原毕设 per-directory CMake 更简单）
cmake_minimum_required(VERSION 3.16)
project(sightline C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------- Lua 5.4（配置热更脚本机：vendor 源码编译静态库，不含独立解释器 lua/luac） ----------
add_library(lua STATIC
    third_party/lua/lapi.c
    third_party/lua/lauxlib.c
    third_party/lua/lbaselib.c
    third_party/lua/lcode.c
    third_party/lua/lcorolib.c
    third_party/lua/lctype.c
    third_party/lua/ldblib.c
    third_party/lua/ldebug.c
    third_party/lua/ldo.c
    third_party/lua/ldump.c
    third_party/lua/lfunc.c
    third_party/lua/lgc.c
    third_party/lua/linit.c
    third_party/lua/liolib.c
    third_party/lua/llex.c
    third_party/lua/lmathlib.c
    third_party/lua/lmem.c
    third_party/lua/loadlib.c
    third_party/lua/lobject.c
    third_party/lua/lopcodes.c
    third_party/lua/loslib.c
    third_party/lua/lparser.c
    third_party/lua/lstate.c
    third_party/lua/lstring.c
    third_party/lua/lstrlib.c
    third_party/lua/ltable.c
    third_party/lua/ltablib.c
    third_party/lua/ltm.c
    third_party/lua/lundump.c
    third_party/lua/lutf8lib.c
    third_party/lua/lvm.c
    third_party/lua/lzio.c
)
target_include_directories(lua PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua)

# ---------- 框架与驱动层：毕带网络库 + 日志 + LuaVM（独立静态库，不含任何 game 头） ----------
add_library(sightline_net STATIC
    src/net/epoll.cpp
    src/net/channel.cpp
    src/net/event_loop.cpp
    src/net/socket.cpp
    src/net/inet_address.cpp
    src/net/heap_scheduler.cpp
    src/logger/logger.cpp
    src/lua/lua_vm.cpp
)
target_link_libraries(sightline_net PUBLIC Threads::Threads lua)

# ---------- 存储线程池（StorageIO 为 header-only：任务队列 + 回投，15 号 01 文档 D1） ----------

# ---------- 可执行（domain/application/adapters 均为 header-only） ----------
add_executable(sightline main.cpp)
target_link_libraries(sightline PRIVATE sightline_net)

# ---------- 单元测试（domain/application 零网络依赖，直接编译即可；logger 链接 net 库） ----------
add_executable(test_hitscan       tests/domain/test_hitscan.cpp)
add_executable(test_room          tests/domain/test_room.cpp)
add_executable(test_room_service  tests/application/test_room_service.cpp)
add_executable(test_logger        tests/logger/test_logger.cpp)
add_executable(test_lua           tests/lua/test_lua.cpp)
add_executable(test_storage       tests/storage/test_storage_io.cpp)
target_link_libraries(test_logger   PRIVATE sightline_net)
target_link_libraries(test_lua      PRIVATE sightline_net)
target_link_libraries(test_storage  PRIVATE sightline_net)

enable_testing()
add_test(NAME hitscan      COMMAND test_hitscan)
add_test(NAME room         COMMAND test_room)
add_test(NAME room_service COMMAND test_room_service)
add_test(NAME logger       COMMAND test_logger)
add_test(NAME lua          COMMAND test_lua WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME storage      COMMAND test_storage)
