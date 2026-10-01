//
// Created by 29108 on 2025/6/29.
//

#ifndef SOCKET_H
#define SOCKET_H
#include "event_loop.h"
#include "inet_address.h"

namespace common {
    namespace network {
        class Socket {
        public:
            //创建socket
            explicit Socket(int sockfd);

            explicit Socket();

            ~Socket();

            /**
            * @brief 应用Socket配置
            * @param reuse_addr 是否启用SO_REUSEADDR
            * @param reuse_port 是否启用SO_REUSEPORT
            * @param tcp_no_delay 是否启用TCP_NODELAY
            * @param keep_alive 是否启用SO_KEEPALIVE
            * @details 通过参数注入方式接收配置，避免Socket类持有配置对象
            */
            void applySocketOptions(bool reuse_addr, bool reuse_port,
                                   bool tcp_no_delay, bool keep_alive);

            /**
             * @brief 配置Keep-Alive参数
             * @param idle_time 空闲时间（秒）
             * @param interval 探测间隔（秒）
             * @param probes 探测次数
             * @details 接受具体参数值，而非配置对象，保持接口简洁
             */
            void configureKeepAliveParameters(int idle_time, int interval, int probes);

            /**
            * @brief 动态更新Socket配置（热更新支持）
            * @param new_config 新的网络配置
            * @details 在不关闭连接的情况下更新Socket选项
            */
            void updateSocketConfig(const networkConfig& new_config);

            /**
             * @brief 获取当前Socket的配置状态
             * @return 当前Socket配置信息
             */
            struct SocketConfigState {
                bool tcp_no_delay;
                bool keep_alive;
                int keep_alive_idle_time;
                int keep_alive_interval;
                int keep_alive_probes;
                bool is_valid;
            };
            SocketConfigState getCurrentConfigState() const;

            //获取socket文件描述符
            int fd() const;

            //绑定地址
            void bindAddress(const InetAddress &localaddr);

            //监听
            void listen();

            //接收连接
            int accept(InetAddress *peeraddr);

            //关闭写端
            void shutdownWrite();

            //设置TCP选项
            void setTcpNoDelay(bool on);

            void setReuseAddr(bool on);

            void setReusePort(bool on);

            void setKeepAlive(bool on);

            /**
             * @brief 强制关闭socket
             * @details 立即关闭socket并释放端口，不等待数据传输完成
             */
            void forceClose();

        private:
            int sockfd_ = -1;

            /**
             * @brief 配置TCP Keep-Alive详细参数
             *
             * 企业级增强功能：
             * - 设置keep-alive空闲时间
             * - 设置探测包发送间隔
             * - 设置最大探测次数
             * - 提供更精细的连接保活控制
             */
            void configureKeepAliveParameters();
        };
    }
}
#endif //SOCKET_H
