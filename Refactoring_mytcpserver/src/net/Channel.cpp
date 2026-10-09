#include "net/Channel.h"

#include <poll.h>       /* 用 POLL* 常量，避免把 <sys/epoll.h> 引进 Channel.cpp */
#include <cassert>

#include "base/Logger.h"
#include "net/EventLoop.h"

const int Channel::kNoneEvent = 0;
const int Channel::kReadEvent = POLLIN | POLLPRI;
const int Channel::kWriteEvent = POLLOUT;

Channel::Channel(EventLoop *loop, int fd) :
    loop_(loop), fd_(fd), events_(0), revents_(0), index_(kNew), tied_(false), eventHandling_(false), addedToLoop_(false)
{
}

Channel::~Channel()
{
    assert(!eventHandling_);    // 检查是否正在处理事件
}

void Channel::tie(const std::shared_ptr<void>& obj)
{
    tie_ = obj;
    tied_ = true;
}

void Channel::update()
{
    loop_->updateChannel(this);
}

void Channel::remove()
{
    loop_->removeChannel(this);
}

void Channel::handleEventWithGuard()
{
    if (revents_ & (POLLIN | POLLPRI | POLLRDHUP))
        if (readCallback_)
            readCallback_();

    if (revents_ & POLLOUT)
        if (writeCallback_)
            writeCallback_();

    if (revents_ & POLLERR)
        if (errorCallback_)
            errorCallback_();

    if (revents_ & POLLHUP)
        if (closeCallback_)
            closeCallback_();

}

void Channel::handleEvent()
{
    std::shared_ptr<void> guard;
    if (tied_) {
        guard = tie_.lock();
        if (!guard) return;
    }

    eventHandling_ = true;
    handleEventWithGuard();
    eventHandling_ = false;
}

/*
 * ============ Channel 实现清单（README 第四节「第 3 步」）============
 *
 * 建议的落笔顺序：
 *
 * 1) 三个静态常量
 *      kNoneEvent  = 0;
 *      kReadEvent  = POLLIN | POLLPRI;
 *      kWriteEvent = POLLOUT;
 *    为什么用 POLL* 而不是 EPOLL*：数值完全一样（POLLIN == EPOLLIN == 0x001），
 *    但 <poll.h> 不是 epoll 专属头文件，这样 README 第二节那条规矩才守得住。
 *    需要 POLLRDHUP 时记得确认 _GNU_SOURCE —— g++ 默认就带，不用手写。
 *
 * 2) 构造 / 析构
 *      初始化 loop_、fd_、events_ = 0、revents_ = 0、index_ = kNew，
 *      四个 bool 全 false，addedToLoop_ = false。
 *      析构里 assert(!eventHandling_)，并且千万不要 close(fd_) —— fd 不归它管。
 *
 * 3) update()      —— 一行：loop_->updateChannel(this);
 * 4) remove()      —— 一行：loop_->removeChannel(this);
 * 5) tie()         —— tie_ = obj; tied_ = true;
 *                    第 3 步先不写，等第 6 步 TcpConnection 出现再加。
 *
 * 6) handleEvent()
 *      eventHandling_ = true;
 *      handleEventWithGuard();
 *      eventHandling_ = false;
 *
 * 7) handleEventWithGuard() —— 最容易写错的一段，三个坑：
 *
 *    坑一：EPOLLHUP 不一定带 EPOLLIN。对端 close 时你可能只拿到 EPOLLHUP，
 *          所以不能写成「只有 POLLIN 才调 readCallback_」。
 *          muduo 的做法是收到 POLLHUP 时也走 readCallback_，
 *          让上层 read 到 0，自然触发关闭流程。
 *
 *    坑二：必须注册 POLLRDHUP，否则对端 shutdown(SHUT_WR) 这种半关闭
 *          你收不到通知。可读分支写成 (POLLIN | POLLPRI | POLLRDHUP)。
 *
 *    坑三：POLLERR 时 read 和 write 都可能返回错误，两个回调都调一下更稳。
 *
 *    如果加了 tie_ 保护，还要在最前面：
 *      std::shared_ptr<void> guard;
 *      if (tied_) { guard = tie_.lock(); if (guard) handleEventWithGuard(); }
 *      else handleEventWithGuard();
 *    并且 eventHandling_ 为 true 时禁止析构宿主对象 —— 这是 TcpConnection 的活。
 *
 * 验收：配合 tests/test_channel.cpp 跑，用 eventfd 造一个 fd，
 *       往另一头写数据，看 readCallback_ 到底有没有被调到。
 */

// const int Channel::kNoneEvent  = 0;
// const int Channel::kReadEvent  = POLLIN | POLLPRI;
// const int Channel::kWriteEvent = POLLOUT;
//
// Channel::Channel(EventLoop* loop, int fd)
//     : loop_(loop), fd_(fd), events_(0), revents_(0), index_(kNew),
//       tied_(false), eventHandling_(false), addedToLoop_(false)
// {
// }
//
// Channel::~Channel()
// {
//     assert(!eventHandling_);
// }
//
// void Channel::update() { loop_->updateChannel(this); }
// void Channel::remove() { loop_->removeChannel(this); }
// void Channel::tie(const std::shared_ptr<void>& obj) { tie_ = obj; tied_ = true; }
//
// void Channel::handleEvent()
// {
//     eventHandling_ = true;
//     handleEventWithGuard();
//     eventHandling_ = false;
// }
//
// void Channel::handleEventWithGuard()
// {
//     if (revents_ & (POLLIN | POLLPRI | POLLRDHUP)) {
//         if (readCallback_) readCallback_();
//     }
//     if (revents_ & POLLOUT) {
//         if (writeCallback_) writeCallback_();
//     }
//     if (revents_ & POLLERR) {
//         if (errorCallback_) errorCallback_();
//     }
//     if (revents_ & POLLHUP) {
//         if (closeCallback_) closeCallback_();
//     }
// }
