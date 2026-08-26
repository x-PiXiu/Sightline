//
// Created by 29108 on 2025/6/29.
//

#include "net/socket.h"
#include <cstring>
#include <stdexcept>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/tcp.h>  // 用于TCP_NODELAY选项
#include <cstdio>         // 用于perror和fprintf
#include "logger/logger.h"

namespace common {
    namespace network {
        Socket::Socket(int sockfd): sockfd_(sockfd) {
            if (sockfd_ == -1) {
                LOG_ERROR("Socket creation failed: " + std::string(strerror(errno)));
                throw std::runtime_error("Socket creation failed: " + std::string(strerror(errno)));
            }
        }

        Socket::Socket() {
            sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
            if (sockfd_ == -1) {
                LOG_ERROR("Socket creation failed: " + std::string(strerror(errno)));
                throw std::runtime_error("Socket creation failed: " + std::string(strerror(errno)));
            }
        }

        Socket::~Socket() {
            if (sockfd_ != -1) {
                // 强制关闭socket，确保端口立即释放
                // 设置 SO_LINGER 为 0，强制立即关闭连接
                struct linger linger_opt;
                linger_opt.l_onoff = 1;   // 启用 linger
                linger_opt.l_linger = 0;  // 立即关闭，不等待数据发送完成

                if (::setsockopt(sockfd_, SOL_SOCKET, SO_LINGER,
                                &linger_opt, sizeof(linger_opt)) < 0) {
                    LOG_WARNING("Failed to set SO_LINGER for socket fd: " + std::to_string(sockfd_) +
                               ", error: " + std::string(strerror(errno)));
                }

                // 关闭socket
                if (::close(sockfd_) < 0) {
                    LOG_WARNING("Failed to close socket fd: " + std::to_string(sockfd_) +
                               ", error: " + std::string(strerror(errno)));
                } else {
                }
                sockfd_ = -1;
            }
        }

        void Socket::applySocketOptions(bool reuse_addr, bool reuse_port, bool tcp_no_delay, bool keep_alive) {
            if (sockfd_ < 0) {
                LOG_WARNING("Cannot apply socket options on invalid socket");
                return;
            }

            // 应用各项Socket选项
            setReuseAddr(reuse_addr);
            setReusePort(reuse_port);
            setTcpNoDelay(tcp_no_delay);
            setKeepAlive(keep_alive);

            LOG_INFO("Socket options applied for fd " + std::to_string(sockfd_) +
                     ": reuse_addr=" + (reuse_addr ? "true" : "false") +
                     ", reuse_port=" + (reuse_port ? "true" : "false") +
                     ", tcp_no_delay=" + (tcp_no_delay ? "true" : "false") +
                     ", keep_alive=" + (keep_alive ? "true" : "false"));
        }

        void Socket::configureKeepAliveParameters(int idle_time, int interval, int probes) {
            if (sockfd_ < 0) {
                LOG_WARNING("Cannot configure keep-alive on invalid socket");
                return;
            }

            // 参数合法性检查
            if (idle_time <= 0 || interval <= 0 || probes <= 0) {
                LOG_ERROR("Invalid keep-alive parameters: idle_time=" + std::to_string(idle_time) +
                          ", interval=" + std::to_string(interval) + ", probes=" + std::to_string(probes));
                return;
            }

            // 使用传入的参数值而非配置对象
            const int keepIdle = idle_time;
            const int keepInterval = interval;
            const int keepCount = probes;

#ifdef TCP_KEEPIDLE
            if (::setsockopt(sockfd_, IPPROTO_TCP, TCP_KEEPIDLE,&keepIdle, sizeof(keepIdle)) < 0) {
                LOG_WARNING("Failed to set TCP_KEEPIDLE: " + std::string(strerror(errno)));
            } else {
            }
#endif

#ifdef TCP_KEEPINTVL
            if (::setsockopt(sockfd_, IPPROTO_TCP, TCP_KEEPINTVL,&keepInterval, sizeof(keepInterval)) < 0) {
                LOG_WARNING("Failed to set TCP_KEEPINTVL: " + std::string(strerror(errno)));
            } else {
            }
#endif

#ifdef TCP_KEEPCNT
            if (::setsockopt(sockfd_, IPPROTO_TCP, TCP_KEEPCNT,&keepCount, sizeof(keepCount)) < 0) {
                LOG_WARNING("Failed to set TCP_KEEPCNT: " + std::string(strerror(errno)));
            } else {
            }
#endif

            LOG_INFO("Keep-alive parameters configured from config: idle=" + std::to_string(keepIdle) +
                     "s, interval=" + std::to_string(keepInterval) +
                     "s, count=" + std::to_string(keepCount));
        }

        int Socket::fd() const { return sockfd_; }

        void Socket::bindAddress(const InetAddress &localaddr) {
            if (sockfd_ == -1) {
                LOG_ERROR("Socket is not valid");
                throw std::runtime_error("Socket is not valid");
            }
            if (bind(sockfd_, localaddr.getConstSockAddr(), localaddr.getSockAddrSize()) == -1) {
                LOG_ERROR("bind failed: " + std::string(strerror(errno)));
                throw std::runtime_error("bind failed: " + std::string(strerror(errno)));
            }
        }

        void Socket::listen() {
            if (sockfd_ == -1) {
                LOG_ERROR("Socket is not valid");
                throw std::runtime_error("Socket is not valid");
            }
            
            // ⭐⭐⭐ 优化：将监听队列从1024增加到65535
            // backlog参数定义等待accept()的连接队列长度
            // 实际值会受限于 net.core.somaxconn 系统参数
            // 建议：代码设置大值 + 系统参数优化，两者取小值生效
            const int backlog = 65535;  // 从1024增加到65535
            
            if (::listen(sockfd_, backlog) == -1) {
                LOG_ERROR("listen failed: " + std::string(strerror(errno)));
                throw std::runtime_error("listen failed: " + std::string(strerror(errno)));
            }
            
            LOG_INFO("Socket listening with backlog=" + std::to_string(backlog) + 
                     " (actual may be limited by net.core.somaxconn)");
        }

        int Socket::accept(InetAddress *peeraddr) {
            if (sockfd_ == -1) {
                LOG_ERROR("Socket is not valid");
                throw std::runtime_error("Socket is not valid");
            }
            socklen_t addrlen = peeraddr->getSockAddrSize();
            // accept4 直接产出非阻塞 fd：ET 读循环的 readv 依赖 EAGAIN 而非阻塞
            int connfd = ::accept4(sockfd_, peeraddr->getSockAddr(), &addrlen,
                                    SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (connfd == -1 && errno == ENOSYS) {
                // 老内核兜底：accept + 手动设非阻塞
                connfd = ::accept(sockfd_, peeraddr->getSockAddr(), &addrlen);
                if (connfd >= 0) {
                    int flags = ::fcntl(connfd, F_GETFL, 0);
                    ::fcntl(connfd, F_SETFL, flags | O_NONBLOCK);
                }
            }
            if (connfd == -1) {
                int saved_errno = errno;
                
                // 对于非阻塞socket，EAGAIN/EWOULDBLOCK是正常情况
                if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
                    return -1;
                }
                
                // ⭐⭐⭐ 修复：EMFILE/ENFILE 文件描述符用尽，记录警告但不抛异常
                // 这样服务器可以继续运行，等待现有连接关闭释放fd后再接受新连接
                if (saved_errno == EMFILE || saved_errno == ENFILE) {
                    LOG_WARNING("⚠️ 文件描述符用尽: " + std::string(strerror(saved_errno)) + 
                               " - 服务器将等待现有连接关闭后再接受新连接");
                    LOG_WARNING("建议执行: ulimit -n 65535 或修改 /etc/security/limits.conf");
                    return -1;  // 返回-1而不是抛异常
                }
                
                // 其他错误情况记录并抛异常
                LOG_ERROR("accept failed: " + std::string(strerror(saved_errno)));
                throw std::runtime_error("accept failed: " + std::string(strerror(saved_errno)));
            }
            return connfd;
        }

        /**
 * @brief 关闭socket的写端
 *
 * 功能说明：
 * - 调用shutdown(SHUT_WR)关闭写端，但保持读端开放
 * - 向对端发送FIN包，表示不再发送数据
 * - 仍可以接收对端发送的数据
 * - 常用于优雅关闭连接的第一步
 *
 * 使用场景：
 * - HTTP服务器发送完响应后关闭写端
 * - 客户端发送完请求后关闭写端
 * - 实现半关闭连接
 */
        void Socket::shutdownWrite() {
            if (sockfd_ >= 0) {
                // 关闭写端，SHUT_WR表示关闭写方向
                if (::shutdown(sockfd_, SHUT_WR) < 0) {
                    // 获取错误码进行详细的错误处理
                    int error = errno;
                    std::string errorMsg = "Socket::shutdownWrite failed: " + std::string(strerror(error));


                    // 对于某些错误码，这是正常情况（如socket未连接）
                    if (error == ENOTCONN) {
                        // socket未连接，这在测试中是正常的
                    } else if (error == EBADF) {
                        // socket已经关闭
                    } else {
                        // 其他错误情况
                        LOG_ERROR(errorMsg);
                    }
                } else {
                    // 成功关闭写端，记录调试信息
                }
            } else {
                LOG_WARNING("Attempted to shutdown write on invalid socket fd: " + std::to_string(sockfd_));
            }
        }

        /**
         * @brief 设置TCP_NODELAY选项
         * @param on true启用，false禁用
         *
         * 功能说明：
         * - 控制Nagle算法的启用/禁用
         * - Nagle算法会延迟发送小数据包以提高网络效率
         * - 启用TCP_NODELAY会禁用Nagle算法，立即发送数据
         * - 适用于需要低延迟的应用（如游戏、实时通信）
         *
         * 性能影响：
         * - 启用：降低延迟，但可能增加网络包数量
         * - 禁用：可能增加延迟，但减少网络包数量
         */
        void Socket::setTcpNoDelay(bool on)
        {
            if (sockfd_ >= 0) {
                int optval = on ? 1 : 0;
                if (::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY,
                                &optval, static_cast<socklen_t>(sizeof(optval))) < 0) {
                    // 企业级错误处理：记录详细错误信息
                    int error = errno;
                    std::string errorMsg = "Socket::setTcpNoDelay(" + std::string(on ? "true" : "false") +
                                         ") failed: " + std::string(strerror(error));
                    LOG_ERROR(errorMsg);

                    // 某些情况下TCP_NODELAY可能不被支持，记录警告而不是错误
                    if (error == ENOPROTOOPT || error == EOPNOTSUPP) {
                        LOG_WARNING("TCP_NODELAY not supported on this socket");
                    }

                } else {
                    // 记录成功设置的调试信息
                    //                           " for socket fd: " + std::to_string(sockfd_));
                }
            } else {
                LOG_WARNING("Attempted to set TCP_NODELAY on invalid socket fd: " + std::to_string(sockfd_));
            }
        }

        /**
         * @brief 设置SO_REUSEADDR选项
         * @param on true启用，false禁用
         *
         * 功能说明：
         * - 允许重用处于TIME_WAIT状态的地址
         * - 解决服务器重启时"Address already in use"错误
         * - 允许多个socket绑定到同一地址（在某些条件下）
         *
         * 使用场景：
         * - 服务器程序重启时快速重新绑定端口
         * - 避免等待TIME_WAIT超时（通常2分钟）
         * - 开发调试时频繁重启程序
         *
         * 注意事项：
         * - 必须在bind()之前调用
         * - 可能存在安全风险，生产环境需谨慎使用
         */
        void Socket::setReuseAddr(bool on) {
            if (sockfd_ >= 0) {
                int optval = on ? 1 : 0;
                if (::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR,
                                &optval, static_cast<socklen_t>(sizeof(optval))) < 0) {
                    // 企业级错误处理：记录详细错误信息
                    int error = errno;
                    std::string errorMsg = "Socket::setReuseAddr(" + std::string(on ? "true" : "false") +
                                         ") failed: " + std::string(strerror(error));
                    LOG_ERROR(errorMsg);
                    // 对于 SO_REUSEADDR 设置失败，这是一个严重问题，应该抛出异常
                    throw std::runtime_error(errorMsg);
                }

                // 注意：不再自动设置 SO_REUSEPORT
                // SO_REUSEPORT 应该通过 setReusePort() 方法显式设置
                // 这样可以避免意外的多进程端口共享
            } else {
                LOG_WARNING("Attempted to set SO_REUSEADDR on invalid socket fd: " + std::to_string(sockfd_));
                throw std::runtime_error("Cannot set SO_REUSEADDR on invalid socket");
            }
        }

        void Socket::forceClose() {
            if (sockfd_ != -1) {

                // 设置 SO_LINGER 为 0，强制立即关闭连接
                struct linger linger_opt;
                linger_opt.l_onoff = 1;   // 启用 linger
                linger_opt.l_linger = 0;  // 立即关闭，不等待数据发送完成

                if (::setsockopt(sockfd_, SOL_SOCKET, SO_LINGER,
                                &linger_opt, sizeof(linger_opt)) < 0) {
                    LOG_WARNING("Failed to set SO_LINGER for socket fd: " + std::to_string(sockfd_) +
                               ", error: " + std::string(strerror(errno)));
                }

                // 关闭socket
                if (::close(sockfd_) < 0) {
                    LOG_ERROR("Failed to force close socket fd: " + std::to_string(sockfd_) +
                             ", error: " + std::string(strerror(errno)));
                }
                sockfd_ = -1;
            } else {
                LOG_WARNING("Attempted to force close invalid socket fd: " + std::to_string(sockfd_));
            }
        }

        /**
         * @brief 设置SO_REUSEPORT选项
         * @param on true启用，false禁用
         *
         * 功能说明：
         * - 允许多个socket绑定到完全相同的地址和端口
         * - 内核会在多个socket间负载均衡传入连接
         * - 支持多进程/多线程服务器架构
         *
         * 使用场景：
         * - 多进程服务器模型（如Nginx worker进程）
         * - 提高服务器并发处理能力
         * - 实现无锁的负载均衡
         *
         * 系统要求：
         * - Linux 3.9+内核支持
         * - 某些BSD系统也支持
         *
         * 注意事项：
         * - 必须在bind()之前调用
         * - 所有绑定相同地址的socket都必须设置此选项
         */
        void Socket::setReusePort(bool on) {
            if (sockfd_ >= 0) {
                int optval = on ? 1 : 0;
                // SO_REUSEPORT在某些系统上可能不可用
#ifdef SO_REUSEPORT
                if (::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT,
                                &optval, static_cast<socklen_t>(sizeof(optval))) < 0) {
                    // 企业级错误处理：记录详细错误信息
                    int error = errno;
                    std::string errorMsg = "Socket::setReusePort(" + std::string(on ? "true" : "false") +
                                         ") failed: " + std::string(strerror(error));
                    LOG_ERROR(errorMsg);

                    // 某些内核版本可能不支持SO_REUSEPORT
                    if (error == ENOPROTOOPT || error == EOPNOTSUPP) {
                        LOG_WARNING("SO_REUSEPORT not supported on this kernel version");
                    }
                }
#else
                // 如果系统不支持SO_REUSEPORT，记录警告
                if (on) {
                    LOG_WARNING("SO_REUSEPORT not supported on this system");
                }
#endif
            } else {
                LOG_WARNING("Attempted to set SO_REUSEPORT on invalid socket fd: " + std::to_string(sockfd_));
            }
        }

        /**
         * @brief 设置SO_KEEPALIVE选项
         * @param on true启用，false禁用
         *
         * 功能说明：
         * - 启用TCP层的保活机制
         * - 定期发送保活探测包检测连接状态
         * - 自动检测并清理死连接
         *
         * 工作原理：
         * - 在连接空闲一定时间后开始发送保活包
         * - 如果连续多次探测失败，则认为连接已断开
         * - 系统会自动关闭socket并通知应用程序
         *
         * 使用场景：
         * - 长连接服务（如聊天服务器）
         * - 检测客户端异常断开
         * - 穿越NAT和防火墙保持连接
         *
         * 配置参数（可通过系统调用进一步配置）：
         * - tcp_keepalive_time: 开始探测前的空闲时间
         * - tcp_keepalive_intvl: 探测包发送间隔
         * - tcp_keepalive_probes: 最大探测次数
         */
        void Socket::setKeepAlive(bool on) {
            if (sockfd_ >= 0) {
                int optval = on ? 1 : 0;
                if (::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE,
                                &optval, static_cast<socklen_t>(sizeof(optval))) < 0) {
                    int error = errno;
                    std::string errorMsg = "Socket::setKeepAlive(" + std::string(on ? "true" : "false") +
                                         ") failed: " + std::string(strerror(error));
                    LOG_ERROR(errorMsg);
                } else {
                    // 如果启用了keepalive，应用默认参数（原无参重载已随热更新清理移除）
                    if (on) {
                        configureKeepAliveParameters(600, 60, 3);
                    }
                }
            } else {
                LOG_WARNING("Attempted to set SO_KEEPALIVE on invalid socket fd: " + std::to_string(sockfd_));
            }
        }

        void Socket::setNonBlocking(bool on) {
            if (sockfd_ < 0) {
                LOG_WARNING("Attempted to set non-blocking on invalid socket fd");
                return;
            }
            int flags = ::fcntl(sockfd_, F_GETFL, 0);
            if (flags < 0) {
                LOG_ERROR("fcntl(F_GETFL) failed: " + std::string(strerror(errno)));
                return;
            }
            int new_flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
            if (::fcntl(sockfd_, F_SETFL, new_flags) < 0) {
                LOG_ERROR("fcntl(F_SETFL) failed: " + std::string(strerror(errno)));
            }
        }

    } // namespace network
} // namespace common
