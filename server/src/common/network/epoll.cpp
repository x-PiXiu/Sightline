//
// Created by 29108 on 2025/6/30.
//
#include "common/network/epoll.h"

#include <cassert>
#include <cstring>
#include "common/logger/logger.h"
#include <stdexcept>
#include <unistd.h>
#include <cerrno>

#include "common/network/channel.h"

namespace common {
    namespace network {

        const int MAX_EVENTS = 102400;

        Epoll::EpollStats Epoll::getEpollStats() const {
            EpollStats stats;
            stats.total_channels = channels_.size();
            stats.current_events_size = events_.size();
            stats.epoll_fd = epollfd_;
            stats.next_resize_threshold = nextResize_;

            // 统计各种状态的Channel数量
            for (const auto& [fd, channel] : channels_) {
                if (channel) {
                    if (channel->isReading()) stats.reading_channels++;
                    if (channel->isWriting()) stats.writing_channels++;
                }
            }

            return stats;
        }

        Epoll::Epoll(EventLoop *loop, int init_event_size, int max_events, bool enable_resize_opt,double resize_factor)
            : ownerLoop_(loop),init_event_list_size_(init_event_size),max_events_(max_events),
                enable_resize_optimization_(enable_resize_opt),resize_factor_(resize_factor),events_(init_event_size),nextResize_(0) {

            // 参数验证
            if (init_event_size <= 0 || max_events <= 0 || resize_factor <= 1.0) {
                throw std::invalid_argument("Invalid Epoll parameters");
            }

            epollfd_ = epoll_create1(EPOLL_CLOEXEC);
            if (epollfd_ == -1) {
                LOG_ERROR("epoll_create1 failed: " + std::string(strerror(errno)));
                throw std::runtime_error("epoll_create1 failed: " + std::string(strerror(errno)));
            }


        }

        // Epoll::Epoll(EventLoop* loop)
        //     :ownerLoop_(loop),
        //     init_event_list_size_(KInitEventListSize),  // 使用现有常量
        //     max_events_(1024),                          // 使用合理默认值
        //     enable_resize_optimization_(true),          // 默认启用优化
        //     resize_factor_(1.5),                        // 默认扩容因子
        //     events_(KInitEventListSize),
        //     nextResize_(0) {
        //
        //     epollfd_ = epoll_create1(EPOLL_CLOEXEC);
        //     if (epollfd_ == -1) {
        //         LOG_ERROR("epoll_create1 failed: " + std::string(strerror(errno)));
        //         throw std::runtime_error("epoll_create1 failed: " + std::string(strerror(errno)));
        //     }
        //     LOG_DEBUG("Epoll initialized with default parameters");
        // }

        Epoll::~Epoll() {
            if (epollfd_ != -1) {
                close(epollfd_);
                epollfd_ = -1;
            }
        }

        void Epoll::updateChannel(Channel* channel) {
            int fd = channel->fd();
            int index = channel->index();

            // 缓存查找结果，避免重复查找
            auto it = channels_.find(fd);
            bool exists = (it != channels_.end() && it->second == channel);

            if (index == kNew || index == kDeleted) {
                if (index == kNew) {
                    // 新Channel：必须不存在于 channels_
                    if (exists) {
                        LOG_ERROR("New channel already exists in epoll");
                        return;
                    }
                    channels_[fd] = channel;
                } else { // index == kDeleted
                    // 已删除的Channel：必须存在于 channels_ 且指针匹配
                    if (!exists) {
                        LOG_ERROR("Deleted channel not found in epoll");
                        return;
                    }
                }
                channel->setIndex(kAdded);
                // LOG_DEBUG(" 上 [updateChannel] -> index_ <- " + std::to_string(channel->index()) + " ---- " + std::to_string(fd));
                update(EPOLL_CTL_ADD, channel);
            } else { // 更新已存在的Channel
                // 必须存在于 channels_ 且状态为 kAdded
                if (!exists) {
                    LOG_ERROR("Channel not found in epoll");
                    return;
                }
                if (index != kAdded) {
                    LOG_ERROR("Invalid state for update: expected kAdded");
                    return;
                }

                if (channel->isNoneEvent()) {
                    update(EPOLL_CTL_DEL, channel);
                    channel->setIndex(kDeleted);
                    // LOG_DEBUG("[updateChannel] -> index_ <- " + std::to_string(channel->index()) + " ---- " + std::to_string(fd));
                } else {
                    update(EPOLL_CTL_MOD, channel);
                }
            }
        }

        void Epoll::removeChannel(Channel *channel) {
            int fd = channel->fd();
            // LOG_DEBUG("Remove fd=" + std::to_string(fd));

            auto it = channels_.find(fd);
            if (it == channels_.end() || it->second != channel) {
                LOG_ERROR("Channel not found or mismatch");
                return;
            }

            int index = channel->index();
            if (index != kAdded && index != kDeleted) {
                LOG_ERROR("Invalid state for removal");
                return;
            }

            channels_.erase(it);
            if (index == kAdded) {
                update(EPOLL_CTL_DEL, channel);
            }
            channel->setIndex(kNew);
        }

        bool Epoll::hasChannel(Channel *channel) const {
            auto it = channels_.find(channel->fd());
            return it != channels_.end() && it->second == channel;
        }

        int Epoll::poll(int timeoutMs, std::vector<Channel *> *activeChannels) {

            // 使用成员变量控制扩容优化
            if (enable_resize_optimization_ && nextResize_ > static_cast<int>(events_.size())) {
                events_.resize(nextResize_);
                nextResize_ = 0;
            }

            int numEvents = ::epoll_wait(epollfd_,
                                        &*events_.begin(),
                                        static_cast<int>(events_.size()),
                                        timeoutMs);
            int savedErrno = errno;

            if (numEvents > 0) {
                // LOG_DEBUG(std::to_string(numEvents) + " events happened");
                fillActiveChannels(numEvents, activeChannels);

                // 使用成员变量的扩容因子
                if (enable_resize_optimization_ && static_cast<size_t>(numEvents) == events_.size()) {
                    nextResize_ = static_cast<size_t>(events_.size() * resize_factor_);
                }
            } else if (numEvents == 0) {
                // LOG_DEBUG("nothing happened");
            } else {
                if (savedErrno != EINTR) {
                    savedErrno = errno;
                    LOG_ERROR("Epoll::poll()");
                }
            }

            return numEvents;
        }

        std::vector<std::pair<Channel *, int>> Epoll::getAllChannelStates() const {
            std::vector<std::pair<Channel*, int>> channel_states;

            for (const auto& [fd, channel] : channels_) {
                if (channel && channel->fd() >= 0) {
                    // 获取Channel当前的事件状态
                    int events = channel->events();
                    channel_states.emplace_back(channel, events);

                    // LOG_DEBUG("Saved channel state: fd=" + std::to_string(fd) +
                    //          ", events=" + std::to_string(events));
                }
            }

            LOG_INFO("Retrieved " + std::to_string(channel_states.size()) + " channel states from Epoll");
            return channel_states;
        }

        Epoll::EpollConfig Epoll::getCurrentConfig() const {
            EpollConfig current_config;
            current_config.init_event_list_size = init_event_list_size_;
            current_config.max_events = max_events_;
            current_config.enable_resize_optimization = enable_resize_optimization_;
            current_config.resize_factor = resize_factor_;

            // LOG_DEBUG("Current Epoll config: init_size=" + std::to_string(current_config.init_event_list_size) +
            //          ", max_events=" + std::to_string(current_config.max_events) +
            //          ", resize_opt=" + (current_config.enable_resize_optimization ? "true" : "false") +
            //          ", resize_factor=" + std::to_string(current_config.resize_factor));

            return current_config;
        }

        void Epoll::fillActiveChannels(int numsEvents, ChannelList *activeChannels) const {
            // 1. 校验事件数量合法性
            if (static_cast<size_t>(numsEvents) > events_.size()) {
                LOG_ERROR("Invalid numEvents: " + std::to_string(numsEvents));
                return;
            }

            for (int i = 0; i < numsEvents; ++i) {
                // 2. 安全转换 Channel 指针
                Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
                if (!channel) {
                    LOG_ERROR("Event has null Channel pointer");
                    continue;
                }

                int fd = channel->fd();
                // 3. 查找 Channel 并校验一致性
                auto it = channels_.find(fd);
                if (it == channels_.end()) {
                    LOG_ERROR("Channel fd=" + std::to_string(fd) + " not registered");
                    continue;
                }
                if (it->second != channel) {
                    LOG_ERROR("Channel pointer mismatch for fd=" + std::to_string(fd));
                    continue;
                }

                // 4. 回传事件并加入就绪列表
                channel->setRevents(events_[i].events);
                activeChannels->push_back(channel);
            }
        }

        void Epoll::update(int operation, Channel *channel) {
            struct epoll_event event;
            memset(&event, 0, sizeof event);
            event.events = channel->events();
            event.data.ptr = channel;
            int fd = channel->fd();
            
            const char* op_str = (operation == EPOLL_CTL_ADD) ? "ADD" :
                                (operation == EPOLL_CTL_MOD) ? "MOD" : "DEL";

            // LOG_DEBUG("epoll_ctl(" + std::string(op_str) + "): fd=" + std::to_string(fd) +
            //          ", events=" + std::to_string(event.events));

            if (::epoll_ctl(epollfd_, operation, fd, &event) < 0) {
                std::string error_msg = "epoll_ctl(" + std::string(op_str) + ") failed: fd=" + 
                                       std::to_string(fd) + ", errno=" + std::to_string(errno) + 
                                       " (" + strerror(errno) + ")";
                
                if (operation == EPOLL_CTL_DEL) {
                    // Delete operation failure is usually because fd is already closed, normal during shutdown
                    if (errno == EBADF || errno == ENOENT) {
                        // LOG_DEBUG("epoll_ctl DEL ignored (fd already closed): fd=" + std::to_string(fd));
                    } else {
                        LOG_ERROR(error_msg);
                    }
                } else {
                    if (errno == EINVAL || errno == EBADF) {
                        // LOG_DEBUG("epoll_ctl ignored (invalid fd): op=" + std::string(op_str) +
                        //          ", fd=" + std::to_string(fd));
                    } else {
                        LOG_ERROR(error_msg);
                    }
                }
            } else {
                // LOG_DEBUG("epoll_ctl(" + std::string(op_str) + ") success: fd=" + std::to_string(fd));
            }
        }
    }  // namespace network
}  // namespace common
