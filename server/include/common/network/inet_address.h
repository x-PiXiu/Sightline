//
// Created by 29108 on 2025/6/29.
//

#ifndef INET_ADDRESS_H
#define INET_ADDRESS_H

#include <string>
#include <vector>
#include <netinet/in.h>

namespace common {
    namespace network {

        /**
         * @brief 网络地址封装类
         *
         * 该类封装了IPv4网络地址的操作，提供了便捷的接口来处理IP地址和端口号。
         * 主要功能包括：
         * - IP地址和端口号的存储和转换
         * - 字符串格式和二进制格式之间的转换
         * - 与socket系统调用的兼容性
         * - 网络字节序和主机字节序的自动转换
         */
        class InetAddress {
        public:
            // ==================== 构造函数 ====================

            /**
             * @brief 默认构造函数
             * @param port 端口号，默认为0（系统自动分配）
             * @param loopbackOnly 是否只绑定到回环地址，默认false
             *
             * 当loopbackOnly为true时，绑定到127.0.0.1（仅本地访问）
             * 当loopbackOnly为false时，绑定到0.0.0.0（所有网络接口）
             */
            explicit InetAddress(uint16_t port = 0, bool loopbackOnly = false);

            /**
             * @brief 使用IP地址和端口构造
             * @param ip IP地址字符串，支持点分十进制格式
             * @param port 端口号
             */
            InetAddress(const std::string& ip, uint16_t port);

            /**
             * @brief 拷贝构造函数
             * @param other 另一个InetAddress对象
             */
            InetAddress(const InetAddress& other) = default;

            /**
             * @brief 赋值操作符
             * @param other 另一个InetAddress对象
             * @return 当前对象的引用
             */
            InetAddress& operator=(const InetAddress& other) = default;

            /**
             * @brief 析构函数
             */
            ~InetAddress() = default;

            // ==================== 地址信息获取 ====================

            /**
             * @brief 获取IP地址字符串
             * @return IP地址的点分十进制表示，如"192.168.1.1"
             */
            std::string toIp() const;

            std::vector<std::string> getLocalIPs();

            /**
             * @brief 获取IP地址和端口的组合字符串
             * @return "IP:端口"格式的字符串，如"192.168.1.1:8080"
             */
            std::string toIpPort() const;

            /**
             * @brief 获取端口号
             * @return 端口号（主机字节序）
             */
            uint16_t port() const;

            // ==================== 底层接口 ====================

            /**
             * @brief 获取sockaddr结构指针
             * @return 指向内部sockaddr_in的sockaddr指针，用于socket系统调用
             */
            const struct sockaddr* getConstSockAddr() const;

            struct sockaddr* getSockAddr();

            /**
             * @brief 设置sockaddr_in结构
             * @param addr sockaddr_in结构体
             *
             * 通常用于从accept()等系统调用中获取客户端地址信息
             */
            void setSockAddr(const struct sockaddr_in& addr);

            /**
             * @brief 获取sockaddr结构的大小
             * @return sockaddr_in结构的大小
             */
            static socklen_t getSockAddrSize() {
                return sizeof(struct sockaddr_in);
            }

            // ==================== 实用方法 ====================

            /**
             * @brief 检查是否为回环地址
             * @return 如果是127.0.0.1则返回true
             */
            bool isLoopback() const;

            /**
             * @brief 检查是否为任意地址
             * @return 如果是0.0.0.0则返回true
             */
            bool isAnyAddress() const;

            /**
             * @brief 检查是否为有效的地址
             * @return 如果地址有效则返回true
             */
            bool isValid() const;

            // ==================== 比较操作符 ====================

            /**
             * @brief 相等比较操作符
             * @param other 另一个InetAddress对象
             * @return 如果IP和端口都相同则返回true
             */
            bool operator==(const InetAddress& other) const;

            /**
             * @brief 不等比较操作符
             * @param other 另一个InetAddress对象
             * @return 如果IP或端口不同则返回true
             */
            bool operator!=(const InetAddress& other) const;

            // ==================== 静态工具方法 ====================

            /**
             * @brief 解析IP地址字符串
             * @param ipStr IP地址字符串
             * @return 如果格式有效则返回true
             */
            static bool isValidIpAddress(const std::string& ipStr);

        private:
            struct sockaddr_in addr_;  ///< 底层的sockaddr_in结构体
        };

    } // namespace network
}
#endif //INET_ADDRESS_H
