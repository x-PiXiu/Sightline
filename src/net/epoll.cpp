//
// Created by 29108 on 2025/6/30.
//
#include "net/epoll.h"

#include <cassert>
#include <cstring>
#include "logger/logger.h"
#include <stdexcept>
#include <unistd.h>
#include <cerrno>

#include "net/channel.h"

namespace common {
    namespace network {



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
                } else {
                    update(EPOLL_CTL_MOD, channel);
                }
            }
        }

        void Epoll::removeChannel(Channel *channel) {
            int fd = channel->fd();

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
                fillActiveChannels(numEvents, activeChannels);

                // 使用成员变量的扩容因子
                if (enable_resize_optimization_ && static_cast<size_t>(numEvents) == events_.size()) {
                    nextResize_ = static_cast<size_t>(events_.size() * resize_factor_);
                }
            } else if (numEvents == 0) {
            } else {
                if (savedErrno != EINTR) {
                    savedErrno = errno;
                    LOG_ERROR("Epoll::poll()");
                }
            }

            return numEvents;
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


            if (::epoll_ctl(epollfd_, operation, fd, &event) < 0) {
                std::string error_msg = "epoll_ctl(" + std::string(op_str) + ") failed: fd=" + 
                                       std::to_string(fd) + ", errno=" + std::to_string(errno) + 
                                       " (" + strerror(errno) + ")";
                
                if (operation == EPOLL_CTL_DEL) {
                    // Delete operation failure is usually because fd is already closed, normal during shutdown
                    if (errno == EBADF || errno == ENOENT) {
                    } else {
                        LOG_ERROR(error_msg);
                    }
                } else {
                    if (errno == EINVAL || errno == EBADF) {
                    } else {
                        LOG_ERROR(error_msg);
                    }
                }
            } else {
            }
        }
    }  // namespace network
}  // namespace common
