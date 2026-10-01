cmake_minimum_required(VERSION 3.16)
project(SightlineServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ============ 前任遗产：毕设网络库原版（network 底座 + logger + 依赖闭包） ============
# 目录层级保留 common/ 前缀——源码 include 路径与毕设一致，未做任何修改
add_library(sightline_core STATIC
    src/common/network/channel.cpp
    src/common/network/epoll.cpp
    src/common/network/event_loop.cpp
    src/common/network/HierarchicalTimingWheel.cpp
    src/common/network/inet_address.cpp
    src/common/network/socket.cpp
    src/common/logger/logger.cpp
    src/common/thread_pool/thread_pool.cpp
    src/common/config/config_manager.cpp
)
target_include_directories(sightline_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_package(Threads REQUIRED)
find_package(yaml-cpp REQUIRED)   # config_manager 依赖（毕设原版配置读取）
target_link_libraries(sightline_core PUBLIC Threads::Threads yaml-cpp)

# ============ 冒烟测试：验证底座可运行 ============
add_executable(smoke_base tests/smoke_base.cpp)
target_link_libraries(smoke_base PRIVATE sightline_core)

# ============ 第 1 期待开发：Acceptor / ClientConnection / main ============
# 设计图定稿后按 docs/episodes/ep01/ 实施
