//
// Created by 29108 on 2025/6/30.
//

#ifndef CHANNEL_H
#define CHANNEL_H

#include <functional>
#include <memory>
#include <sys/epoll.h>

#include "common/thread_pool/thread_pool.h"
#include "common/logger/logger.h"

namespace common {
    namespace network {
        class EventLoop;

        class Channel {
        public:
            typedef std::function<void()> EventCallback;
            typedef std::function<void()> ReadEventCallback;

            Channel(EventLoop* loop, int fd);
            ~Channel();

            //处理事件
            void handleEvent();
            
            // 🔧 添加可配置的事件处理方法
            void handleEventWithConfig(bool use_thread_pool = false);

            //设置回调函数
            void setReadCallback(const ReadEventCallback& cb) { readCallback_ = cb; }
            void setWriteCallback(const EventCallback& cb) { writeCallback_ = cb; }
            void setCloseCallback(const EventCallback& cb) { closeCallback_ = cb; }
            void setErrorCallback(const EventCallback& cb) { errorCallback_ = cb; }

            //获取文件描述符
            int fd() const { return fd_; }

            //获取和设置事件
            int events() const {
                return events_;
            }
            void setRevents(int revt) { revents_ = revt; }

            //判断是否没有事件
            bool isNoneEvent() const {
                return events_ == kNoneEvent;
            }

            //启动/禁用读写事件
            void enableReading() {
                events_ |= kReadEvent;
                update();
            }
            void disableReading() {
                events_ &= ~kReadEvent;
                update();
            }
            void enableWriting() {
                events_ |= kWriteEvent;
                update();
                // 临时调试日志
                LOG_DEBUG("Channel enableWriting called for fd: " + std::to_string(fd_));
            }
            void disableWriting() {
                events_ &= ~kWriteEvent;
                update();
            }
            void disableAll() {
                events_ = kNoneEvent;
                update();
            }

            //判断是否启用了写事件
            bool isWriting() const {
                return events_ & kWriteEvent;
            }
            bool isReading() const {
                return events_ & kReadEvent;
            }

            // 获取revents状态（用于调试）
            int revents() const { return revents_; }

            // 获取在Epoll中的状态
            int index() { return index_; }
            void setIndex(int idx) { index_ = idx; }

            // 获取所属的EventLoop
            EventLoop* ownerLoop() { return loop_; }

            // 移除自己
            void remove();

        private:
            //更新到epoll
            void update();

            //事件常量
            static const int kNoneEvent;
            static const int kReadEvent;
            static const int kWriteEvent;

            EventLoop* loop_;   //所属的EventLoop
            const int fd_;      //文件描述符
            int events_;        //关注的事
            int revents_;       //实际发生的事
            int index_;         //在Epoll中的状态

            //事件回调函数
            ReadEventCallback readCallback_;
            EventCallback writeCallback_;
            EventCallback closeCallback_;
            EventCallback errorCallback_;

            bool eventHandling_; //是否正在处理事件
            bool addedToLoop_;  //是否已经添加到EventLoop
        };
    }
}
#endif //CHANNEL_H
