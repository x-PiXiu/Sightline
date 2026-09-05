//
// Created by 29108 on 2025/6/30.
//
#include "net/channel.h"
#include <string>
#include <memory>
#include "logger/logger.h"
#include "net/event_loop.h"
#include <sys/epoll.h>
#include <cassert>

namespace common {
    namespace network {

        const int Channel::kNoneEvent = 0;
        const int Channel::kReadEvent = EPOLLIN | EPOLLPRI | EPOLLET;
        const int Channel::kWriteEvent = EPOLLOUT;


        Channel::Channel(EventLoop *loop, int fd)
            : loop_(loop),
            fd_(fd),
            events_(0),
            revents_(0),
            index_(-1),
            eventHandling_(false),
            addedToLoop_(false){
        }

        Channel::~Channel() {
            try {

                // 1. 检查是否在事件处理中析构（这是危险的）
                eventHandling_ = false;
                if (eventHandling_) {
                    // 使用Logger而不是标准输出
                    LOG_WARNING("[Channel] WARNING: Channel destroyed during event handling fd = " + std::to_string(fd_));
                    LOG_WARNING("[Channel] This may cause segmentation fault. Consider delayed destruction.");
                    // 不要return，继续清理以避免资源泄露
                }

                // 2. 确保从事件循环中移除
                if (addedToLoop_ && loop_) {
                    if (loop_->isInLoopThread()) {
                        // 二次确认是否已移除
                        if (loop_->hasChannel(this)) {
                            loop_->removeChannel(this);
                        }
                    } else {
                        LOG_WARNING("[Channel] Cross-thread removal in destructor for fd = " + std::to_string(fd_)
                                  + " - disabling events only");
                        
                        // 直接设置events为0，避免调用update()触发epoll_ctl
                        events_ = kNoneEvent;
                        
                        // 标记为已从loop移除，避免EventLoop继续处理
                        addedToLoop_ = false;
                        
                        LOG_WARNING("[Channel] Events disabled for fd = " + std::to_string(fd_)
                                  + " - EventLoop will skip processing");
                    }
                }

            } catch (const std::exception& e) {
                LOG_ERROR("[Channel] Exception in destructor for fd " + std::to_string(fd_) + ": " + e.what());
            } catch (...) {
                LOG_ERROR("[Channel] Unknown exception in destructor for fd " + std::to_string(fd_));
            }
        }

        void Channel::handleEvent() {
            if (tied_) {
                // 守卫：本事件处理期间保持 owner（TcpConnection）存活
                std::shared_ptr<void> guard = tie_.lock();
                if (guard) {
                    handleEventWithGuard();
                }
            } else {
                handleEventWithGuard();
            }
        }

        void Channel::handleEventWithGuard() {
            eventHandling_ = true;

            // 处理挂起事件（对端关闭连接）
            if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
                LOG_INFO("[handleEvent] EPOLLHUP without EPOLLIN for fd=" + std::to_string(fd_));
                if (closeCallback_) {
                    closeCallback_();
                }
            }

            // 处理错误事件
            if (revents_ & EPOLLERR) {
                LOG_ERROR("[handleEvent] EPOLLERR for fd=" + std::to_string(fd_));
                if (errorCallback_) {
                    errorCallback_();
                }
            }

            // Handle readable events
            if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
                if (readCallback_) {
                    readCallback_();
                } else {
                    LOG_WARNING("Channel::handleEvent - 读事件触发但没有回调函数 fd: " + std::to_string(fd_));
                }
            }

            // 处理可写事件
            if (revents_ & EPOLLOUT) {
                if (writeCallback_) {
                    writeCallback_();
                }
            }

            eventHandling_ = false;
        }

        void Channel::update() {
            addedToLoop_ = true;
            loop_->updateChannel(this);
        }


        void Channel::remove() {
            // 在移除前确保所有事件都被禁用
            if (!isNoneEvent()) {
                LOG_WARNING("Channel::remove() called with active events. fd=" + std::to_string(fd_) +
                           ", events=" + std::to_string(events_) + ". Auto-disabling...");

                // 强制禁用所有事件
                events_ = kNoneEvent;

                // 立即更新到epoll，确保事件被清除
                if (addedToLoop_ && loop_) {
                    try {
                        update();
                    } catch (const std::exception& e) {
                        LOG_ERROR("Failed to update events during force-disable: " + std::string(e.what()));
                    }
                }
            }

            addedToLoop_ = false;

            // 确保在EventLoop线程中执行removeChannel
            if (loop_->isInLoopThread()) {
                loop_->removeChannel(this);
            } else {
                // 跨线程安全移除
                loop_->runInLoop([this]() {
                    loop_->removeChannel(this);
                });
            }
        }
    }
}