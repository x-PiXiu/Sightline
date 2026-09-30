cmake_minimum_required(VERSION 3.16)
project(SightlineServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ============ 核心库：net + logger（前任遗产，基础设施层） ============
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
target_compile_options(sightline_core PRIVATE -Wall)
find_package(Threads REQUIRED)
target_link_libraries(sightline_core PUBLIC Threads::Threads)

# ============ 基础设施隔离测试（手动 tick，绕过 EventLoop 的单组件验证） ============
foreach(t test_wheel test_timer test_periodic test_et_min test_logger)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/${t}.cpp)
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE sightline_core)
    endif()
endforeach()

# ============ 业务层（第 1 期起逐期实现：domain / application / adapters / main） ============
# main.cpp 就位后取消注释：
# add_executable(sightline_server main.cpp)
# target_link_libraries(sightline_server PRIVATE sightline_core)
