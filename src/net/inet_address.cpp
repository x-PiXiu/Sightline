//
// Created by 29108 on 2025/6/29.
//
#include "net/inet_address.h"
#include <string>
#include "logger/logger.h"
#include <arpa/inet.h>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <algorithm>
#include <vector>
#include <chrono>
#include <cstdlib>

namespace common {
    namespace network {
        /**
         * @brief 构造函数 - 使用端口号创建地址对象
         * @param port 端口号，网络字节序
         * @param loopbackOnly 是否只绑定到回环地址(127.0.0.1)
         *
         * 功能说明：
         * - 当loopbackOnly为true时，绑定到127.0.0.1（本地回环）
         * - 当loopbackOnly为false时，绑定到0.0.0.0（所有可用接口）
         * - 自动将端口号转换为网络字节序
         */
        InetAddress::InetAddress(uint16_t port, bool loopbackOnly) {
            // 清零地址结构体，确保所有字段都被正确初始化
            std::memset(&addr_, 0, sizeof(addr_));

            // 设置地址族为IPv4
            addr_.sin_family = AF_INET;

            // 根据loopbackOnly参数设置IP地址
            if (loopbackOnly) {
                // 绑定到本地回环地址 127.0.0.1
                addr_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            } else {
                // 绑定到所有可用网络接口 0.0.0.0
                addr_.sin_addr.s_addr = htonl(INADDR_ANY);
            }

            // 设置端口号，转换为网络字节序
            addr_.sin_port = htons(port);
        }

        /**
         * @brief 构造函数 - 使用IP地址和端口号创建地址对象
         * @param ip IP地址字符串，支持点分十进制格式（如"192.168.1.1"）
         * @param port 端口号，主机字节序
         *
         * 功能说明：
         * - 解析IP地址字符串并转换为网络字节序
         * - 自动将端口号转换为网络字节序
         * - 如果IP地址格式无效，会抛出异常
         */
        InetAddress::InetAddress(const std::string &ip, uint16_t port) {
            // 清零地址结构体
            std::memset(&addr_, 0, sizeof(addr_));

            // 设置地址族为IPv4
            addr_.sin_family = AF_INET;

            // 将IP地址字符串转换为网络字节序的二进制格式
            int result = inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr);
            if (result <= 0) {
                if (result == 0) {
                    // IP地址格式无效
                    LOG_WARNING("Invalid IP address format: " + ip);
                    throw std::invalid_argument("Invalid IP address format: " + ip);
                } else {
                    // 系统调用失败
                    LOG_ERROR("Invalid IP address format: " + ip);
                    throw std::runtime_error("inet_pton failed for IP: " + ip);
                }
            }

            // 设置端口号，转换为网络字节序
            addr_.sin_port = htons(port);
        }

        /**
         * @brief 获取IP地址字符串
         * @return IP地址的点分十进制字符串表示
         *
         * 功能说明：
         * - 将网络字节序的IP地址转换为可读的字符串格式
         * - 返回格式如"192.168.1.1"
         * - 使用inet_ntop进行安全的地址转换
         */
        std::string InetAddress::toIp() const {
            char buf[INET_ADDRSTRLEN];

            // 将网络字节序的IP地址转换为字符串
            const char* result = inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));
            if (result == nullptr) {
                // 转换失败，返回空字符串或抛出异常
                LOG_ERROR("inet_ntop failed");
                throw std::runtime_error("inet_ntop failed");
            }

            return std::string(buf);
        }

        std::vector<std::string> InetAddress::getLocalIPs() {
            std::vector<std::string> ips;
            struct ifaddrs *ifaddrs_list = nullptr;

            // 获取所有网络接口的地址信息
            if (getifaddrs(&ifaddrs_list) != 0) {
                LOG_ERROR("[getLocalIPs] getifaddrs 失败");
                return ips;
            }

            // 遍历所有接口
            for (struct ifaddrs *ifa = ifaddrs_list; ifa != nullptr; ifa = ifa->ifa_next) {
                if (ifa->ifa_addr == nullptr) continue;

                // 只处理 IPv4 地址
                if (ifa->ifa_addr->sa_family == AF_INET) {
                    struct sockaddr_in *addr_in = (struct sockaddr_in *)ifa->ifa_addr;
                    char ip_buffer[INET_ADDRSTRLEN];

                    // 将二进制地址转换为字符串
                    if (inet_ntop(AF_INET, &(addr_in->sin_addr), ip_buffer, INET_ADDRSTRLEN)) {
                        std::string ip(ip_buffer);

                        ips.push_back(ip);
                        // 过滤掉回环地址 (127.0.0.1)
                        // if (ip.substr(0, 4) != "127.") {
                        //     ips.push_back(ip);
                        // }
                    }
                }
            }

            // 释放内存
            freeifaddrs(ifaddrs_list);
            return ips;
        }

        /**
         * @brief 获取IP地址和端口的组合字符串
         * @return "IP:端口" 格式的字符串，如"192.168.1.1:8080"
         *
         * 功能说明：
         * - 组合IP地址和端口号为统一的字符串格式
         * - 便于日志记录和调试输出
         * - 端口号自动转换为主机字节序
         */
        std::string InetAddress::toIpPort() const {
            // 获取IP地址字符串
            std::string ip = toIp();

            // 获取端口号并转换为主机字节序
            uint16_t portNum = ntohs(addr_.sin_port);

            // 组合为 "IP:端口" 格式
            return ip + ":" + std::to_string(portNum);
        }

        /**
         * @brief 获取端口号
         * @return 端口号，主机字节序
         *
         * 功能说明：
         * - 从sockaddr_in结构中提取端口号
         * - 自动转换为主机字节序，便于应用程序使用
         */
        uint16_t InetAddress::port() const {
            // 将网络字节序的端口号转换为主机字节序
            return ntohs(addr_.sin_port);
        }



        /**
         * @brief 设置sockaddr_in结构
         * @param addr sockaddr_in结构体引用
         *
         * 功能说明：
         * - 直接设置内部的sockaddr_in结构
         * - 用于从外部socket操作中获取地址信息
         * - 常用于accept()等系统调用后设置客户端地址
         */
        void InetAddress::setSockAddr(const struct sockaddr_in &addr) {
            // 直接复制整个结构体
            addr_ = addr;
        }

        /**
         * @brief 获取sockaddr结构指针
         * @return 指向内部sockaddr_in结构的sockaddr指针
         *
         * 功能说明：
         * - 返回可用于socket系统调用的地址结构指针
         * - 类型转换为通用的sockaddr指针
         * - 用于bind()、connect()、accept()等系统调用
         */
        const struct sockaddr * InetAddress::getConstSockAddr() const {
            // 将sockaddr_in指针转换为sockaddr指针
            return reinterpret_cast<const struct sockaddr*>(&addr_);
        }

        struct sockaddr * InetAddress::getSockAddr(){
            return reinterpret_cast<struct sockaddr*>(&addr_);
        }

        /**
         * @brief 检查是否为回环地址
         * @return 如果是127.0.0.1则返回true
         *
         * 功能说明：
         * - 检查当前地址是否为本地回环地址
         * - 回环地址用于本地进程间通信
         */
        bool InetAddress::isLoopback() const {
            // 获取网络字节序的IP地址并转换为主机字节序进行比较
            uint32_t ip = ntohl(addr_.sin_addr.s_addr);
            return ip == INADDR_LOOPBACK;  // 127.0.0.1
        }

        /**
         * @brief 检查是否为任意地址
         * @return 如果是0.0.0.0则返回true
         *
         * 功能说明：
         * - 检查当前地址是否为通配符地址
         * - 0.0.0.0表示绑定到所有可用的网络接口
         */
        bool InetAddress::isAnyAddress() const {
            // 获取网络字节序的IP地址并转换为主机字节序进行比较
            uint32_t ip = ntohl(addr_.sin_addr.s_addr);
            return ip == INADDR_ANY;  // 0.0.0.0
        }

        /**
         * @brief 检查是否为有效的地址
         * @return 如果地址有效则返回true
         *
         * 功能说明：
         * - 检查地址族是否为AF_INET
         * - 检查端口号是否在有效范围内
         */
        bool InetAddress::isValid() const {
            // 1. 检查地址族有效性
            if (addr_.sin_family != AF_INET)
                return false;

            // 2. 获取网络字节序IP并转换为主机序
            uint32_t ip = ntohl(addr_.sin_addr.s_addr);

            // 3. 检查保留地址段（Class E实验地址）
            if ((ip & 0xF0000000) == 0xF0000000) { // 240.0.0.0/4
                // 排除广播地址 255.255.255.255
                return (ip == 0xFFFFFFFF); // 仅广播地址有效[1](@ref)
            }

            // 4. 检查无效多播地址范围
            if ((ip & 0xF0000000) == 0xE0000000) { // 224.0.0.0/4
                // 排除本地网络控制块 (224.0.0.0-224.0.0.255)
                return ((ip & 0xFFFFFF00) != 0xE0000000);
            }

            // 5. 其他保留地址（如文档专用、链路本地）视为格式有效
            // 无需额外检查，直接通过[1,8](@ref)

            return true;
        }

        /**
         * @brief 相等比较操作符
         * @param other 另一个InetAddress对象
         * @return 如果IP和端口都相同则返回true
         *
         * 功能说明：
         * - 比较两个地址对象的IP地址和端口号
         * - 使用网络字节序进行比较，避免字节序转换
         */
        bool InetAddress::operator==(const InetAddress& other) const {
            return (addr_.sin_family == other.addr_.sin_family) &&
                   (addr_.sin_addr.s_addr == other.addr_.sin_addr.s_addr) &&
                   (addr_.sin_port == other.addr_.sin_port);
        }

        /**
         * @brief 不等比较操作符
         * @param other 另一个InetAddress对象
         * @return 如果IP或端口不同则返回true
         */
        bool InetAddress::operator!=(const InetAddress& other) const {
            return !(*this == other);
        }

        /**
         * @brief 解析IP地址字符串
         * @param ipStr IP地址字符串
         * @return 如果格式有效则返回true
         *
         * 功能说明：
         * - 使用inet_pton验证IP地址格式
         * - 不修改任何状态，仅进行格式验证
         */
        bool InetAddress::isValidIpAddress(const std::string& ipStr) {
            struct in_addr addr;
            // 尝试将字符串转换为网络地址
            int result = inet_pton(AF_INET, ipStr.c_str(), &addr);
            return result == 1;  // 1表示成功转换
        }
    }
}