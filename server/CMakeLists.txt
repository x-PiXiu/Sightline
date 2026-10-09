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

# ============ 底座：第 1 期网络库（前任遗产收敛版）+ 日志 ============
add_library(sightline_core STATIC
    src/net/channel.cpp
    src/net/epoll.cpp
    src/net/event_loop.cpp
    src/net/HierarchicalTimingWheel.cpp
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

# ============ 第 2 期：服务器本体（装配根） ============
add_executable(sightline_server
    main.cpp
)
target_link_libraries(sightline_server PRIVATE sightline_lobby sightline_core)

# ============ 测试 ============
add_executable(test_lobby tests/test_lobby.cpp)
target_link_libraries(test_lobby PRIVATE sightline_lobby)
add_test(NAME test_lobby COMMAND test_lobby)

add_executable(smoke_base tests/smoke_base.cpp)
target_link_libraries(smoke_base PRIVATE sightline_core)
