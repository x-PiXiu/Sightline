//
// Created by 29108 on 2025/6/30.
//

#ifndef EPOLL_H
#define EPOLL_H

#include <vector>
#include <map>
#include <unordered_map>
#include <sys/epoll.h>

namespace common {
    namespace network {

        class Channel;
        class EventLoop;

        class Epoll {
        public:
            typedef std::vector<struct epoll_event> EventList;
            typedef std::vector<Channel*> ChannelList;
            typedef std::map<int, Channel*> ChannelMap;

            /**
            * @brief Epoll统计信息结构体
            */
            struct EpollStats {
                size_t total_channels = 0;
                size_t reading_channels = 0;
                size_t writing_channels = 0;
                size_t current_events_size = 0;
                int epoll_fd = -1;
                int next_resize_threshold = 0;
            };

            /**
             * @brief 获取Epoll统计信息
             * @return 当前Epoll的统计信息
             */
            EpollStats getEpollStats() const;

            /**
            * @brief 构造函数 - 使用参数注入方式
            * @param loop 所属的EventLoop
            * @param init_event_size 初始事件列表大小
            * @param max_events 最大事件数
            * @param enable_resize_opt 是否启用扩容优化
            * @param resize_factor 扩容因子
            * @details 通过参数接收配置值，而非持有配置对象
            */
            Epoll(EventLoop* loop, int init_event_size = 48, int max_events = 1024,
                  bool enable_resize_opt = true, double resize_factor = 1.5);
            // Epoll(EventLoop* loop);
            ~Epoll();

            //更新Channel
            void updateChannel(Channel* channel);
            void removeChannel(Channel* channel);

            //检测是否存在指导Channel
            bool hasChannel(Channel* channel) const;

            //等待事件
            int poll(int timeoutMs, std::vector<Channel*>* activeChannels);

            /**
            * @brief 获取所有注册的Channel及其事件状态
            * @return Channel指针和事件状态的配对列表
            * @details 用于配置热更新时保存和恢复Channel状态
            */
            std::vector<std::pair<Channel*, int>> getAllChannelStates() const;

            /**
             * @brief 获取当前Epoll的配置信息
             * @return 当前Epoll配置的结构体
             */
            struct EpollConfig {
                int init_event_list_size;
                int max_events;
                bool enable_resize_optimization;
                double resize_factor;
            };
            EpollConfig getCurrentConfig() const;

        private:
            static const int KInitEventListSize = 48;

            // Channel在Epoll中的状态常量
            static const int kNew = -1;        // 新Channel，未添加到epoll
            static const int kAdded = 1;       // 已添加到epoll
            static const int kDeleted = 2;     // 已从epoll中删除

            //填充活跃的Channel
            void fillActiveChannels(int numsEvents, ChannelList* activeChannels) const;
            //更新事件
            void update(int operation, Channel* channel);

            // 成员变量按构造函数初始化顺序排列
            EventLoop* ownerLoop_;
            const int init_event_list_size_;    // 初始事件列表大小
            const int max_events_;              // 最大事件数
            const bool enable_resize_optimization_; // 是否启用扩容优化
            const double resize_factor_;        // 扩容因子
            int epollfd_;
            EventList events_;
            int nextResize_;
            ChannelMap channels_;
        };
    }
}
#endif //EPOLL_H
