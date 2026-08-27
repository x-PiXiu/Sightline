# Sightline —— 整洁架构分层的单根 CMake（相比原毕设 per-directory CMake 更简单）
cmake_minimum_required(VERSION 3.16)
project(sightline CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------- 框架与驱动层：毕带网络库 + 日志（独立静态库，不含任何 game 头） ----------
add_library(sightline_net STATIC
    src/net/epoll.cpp
    src/net/channel.cpp
    src/net/event_loop.cpp
    src/net/socket.cpp
    src/net/inet_address.cpp
    src/net/HierarchicalTimingWheel.cpp
    src/logger/logger.cpp
)
target_link_libraries(sightline_net PUBLIC Threads::Threads)

# ---------- 可执行（domain/application/adapters 均为 header-only） ----------
add_executable(sightline main.cpp)
target_link_libraries(sightline PRIVATE sightline_net)

# ---------- 单元测试（domain/application 零网络依赖，直接编译即可；logger 链接 net 库） ----------
add_executable(test_hitscan       tests/domain/test_hitscan.cpp)
add_executable(test_room          tests/domain/test_room.cpp)
add_executable(test_room_service  tests/application/test_room_service.cpp)
add_executable(test_logger        tests/logger/test_logger.cpp)
target_link_libraries(test_logger PRIVATE sightline_net)

enable_testing()
add_test(NAME hitscan      COMMAND test_hitscan)
add_test(NAME room         COMMAND test_room)
add_test(NAME room_service COMMAND test_room_service)
add_test(NAME logger       COMMAND test_logger)
