cmake_minimum_required(VERSION 3.16)
project(SightlineServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ============ 前任遗产：毕设网络库原版（network 底座 + logger） ============
# 目录层级保留 common/ 前缀——源码 include 路径与毕设一致
# 迁移日手术：ConfigManager / ThreadPool / 配置热更新体系整块切除（net 层零业务依赖）
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
find_package(Threads REQUIRED)
target_link_libraries(sightline_core PUBLIC Threads::Threads)

# ============ 冒烟测试：验证底座可运行 ============
add_executable(smoke_base tests/smoke_base.cpp)
target_link_libraries(smoke_base PRIVATE sightline_core)

# ============ 第 1 期：服务器本体（三件套 header-only + main，设计图已定稿） ============
add_executable(sightline_server
    main.cpp
    include/net/acceptor.h
    include/net/client_connection.h
    include/net/buffer.h
    include/adapters/frame_codec.h
)
target_link_libraries(sightline_server PRIVATE sightline_core)
