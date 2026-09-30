// 最小 ET 复现实验：二分定位"动态注册的连接读不到事件"问题
// 用法：./test_et_min [static|dynamic|nested]
//   static  : loop 启动前 enableReading（对应 Acceptor 监听通道）
//   dynamic : loop 运行中（定时器回调里）enableReading（对应 accept 出来的连接通道）
//   nested  : 在另一个 Channel 的读回调内部 enableReading（精确复刻 accept 嵌套注册）
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>
#include "net/event_loop.h"
#include "net/channel.h"

using common::network::EventLoop;
using common::network::Channel;

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "static";
    int sv[2];
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

    EventLoop loop;
    Channel ch(&loop, sv[0]);
    ch.setReadCallback([&] {
        char buf[64] = {0};
        int n = ::read(sv[0], buf, sizeof buf);
        std::printf("[%s-mode] READ %d bytes: %s\n", mode.c_str(), n, buf);
        std::fflush(stdout);
        loop.quit();
    });

    if (mode == "static") {
        ch.enableReading();   // loop 启动前注册
    } else if (mode == "nested") {
        // A 通道先注册；B 通道在 A 的读回调里注册（模拟 listen channel → accept → conn channel）
        int sv2[2];
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv2);
        static Channel* chB = new Channel(&loop, sv2[0]);
        chB->setReadCallback([&] {
            char buf[64] = {0};
            int n = ::read(sv2[0], buf, sizeof buf);
            std::printf("[nested-mode] B READ %d bytes: %s\n", n, buf);
            std::fflush(stdout);
            loop.quit();
        });
        ch.setReadCallback([&] {
            char buf[64] = {0};
            ::read(sv[0], buf, sizeof buf);          // 触发源：A 收到一条消息
            std::printf("[nested-mode] A fired, registering B\n");
            std::fflush(stdout);
            chB->enableReading();                    // 嵌套注册（对应 accept 路径）
        });
        ch.enableReading();
        std::thread trigger([&] {
            ::sleep(1);
            ::write(sv[1], "go", 2);                 // 唤醒 A → 嵌套注册 B
            ::usleep(300 * 1000);
            ssize_t n = ::write(sv2[1], "ping", 4);  // 之后向 B 发数据
            std::printf("client wrote %zd to B\n", n);
            std::fflush(stdout);
        });
        loop.loop();
        trigger.join();
        std::printf("[nested-mode] loop exited\n");
        return 0;
    } else {
        loop.runAfter(300, [&] {      // loop 运行中注册（模拟 accept 路径）
            ch.enableReading();
            std::printf("[dynamic-mode] registered inside loop\n");
            std::fflush(stdout);
        });
    }

    std::thread client([&] {
        ::sleep(1);                    // 数据一定在注册之后到达
        ssize_t n = ::write(sv[1], "ping", 4);
        std::printf("client wrote %zd\n", n);
        std::fflush(stdout);
    });

    loop.loop();
    client.join();
    std::printf("[%s-mode] loop exited\n", mode.c_str());
    return 0;
}
