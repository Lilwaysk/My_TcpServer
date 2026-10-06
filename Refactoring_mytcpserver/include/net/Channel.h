#ifndef NET_CHANNEL_H
#define NET_CHANNEL_H

#include <functional>
#include <memory>

#include "base/noncopyable.h"

class EventLoop;

/*
 * 一个 fd + 关注的事件 + 事件回调 打成的包。
 *
 * 高级myevent_s
 *
 * 它对应你老代码里的 struct myevent_s，区别是去掉了 buf / len / sendpos ——
 * 缓冲区那部分归 Buffer 管。Channel 只管「事件来了，转给谁」，
 * 自己不含任何业务逻辑，像总机接线员，不参与谈话内容。
 *
 * 归属：Channel 不拥有 fd，EventLoop 也不拥有 Channel。
 * 谁创建谁持有 —— Acceptor 持有监听 Channel，TcpConnection 持有连接 Channel。
 * 唯一例外是 EventLoop 自己那个用于唤醒的 Channel。
 */
class Channel : noncopyable {
public:
    typedef std::function<void()> EventCallback;        // 定义回调函数

    Channel(EventLoop* loop, int fd);
    ~Channel();

    /* EventLoop 的 poll 返回后调用，根据 Poller 填入的实际事件分发回调。 */
    void handleEvent();

    // 注册对应事件的回调；只有发生该类事件且回调非空时才会调用。
    void setReadCallback(const EventCallback& cb) { readCallback_ = cb; }
    void setWriteCallback(const EventCallback& cb) { writeCallback_ = cb; }
    void setCloseCallback(const EventCallback& cb) { closeCallback_ = cb; }
    void setErrorCallback(const EventCallback& cb) { errorCallback_ = cb; }

    // 返回关联的 fd；Channel 只记录 fd，不负责关闭它。
    int fd() const { return fd_; }
    // 返回当前关注的事件掩码，不是本轮实际发生的事件。
    int events() const { return events_; }

    // 判断当前是否没有关注任何事件；不表示 fd 本轮没有事件到达。
    bool isNoneEvent() const { return events_ == kNoneEvent; }
    // 判断当前是否关注写事件。
    bool isWriting() const { return (events_ & kWriteEvent) != 0; }
    // 判断当前是否关注读事件。
    bool isReading() const { return (events_ & kReadEvent) != 0; }

    /* Poller 填入本轮实际发生的事件；与 events_ 中的关注掩码相区别。 */
    void set_revents(int revt) { revents_ = revt; }
    // 返回 Poller 填入的本轮实际事件掩码。
    int revents() const { return revents_; }

    /* 修改关注的事件掩码，并通知 EventLoop/Poller 更新 fd 的监听状态。 */
    // 添加读事件关注；重复调用不会重复添加同一标志，表示事件可读
    void enableReading() { events_ |= kReadEvent; update(); }
    // 取消读事件关注；若仍关注写事件，fd 仍会留在 Poller 中，表示事件不可读
    void disableReading() { events_ &= ~kReadEvent; update(); }
    // 添加写事件关注，表示事件可写；重复调用不会重复添加同一标志
    void enableWriting() { events_ |= kWriteEvent; update(); }
    // 取消写事件关注；若仍关注读事件，fd 仍会留在 Poller 中，表示事件不可写
    void disableWriting() { events_ &= ~kWriteEvent; update(); }
    // 清除所有事件关注；Poller 会将该 fd 从 epoll 监听集合中移除，表示事件不可读不可写
    void disableAll() { events_ = kNoneEvent; update(); }

    /*
    * 三态标记，是 Channel 和 Poller 之间的契约：
    * kNew 表示尚未加入 Poller；kAdded 表示已在 epoll 中监听；
    * kDeleted 表示仍由 Poller 管理，但已从 epoll 监听集合移除。
    * Poller 据此选择 ADD、MOD 或 DEL；重新启用 kDeleted 的 Channel 时会重新 ADD。
     */
    static const int kNew = -1;
    static const int kAdded = 1;
    static const int kDeleted = 2;

    // 返回当前在 Poller 中的注册状态。
    int index() const { return index_; }
    // 由 Poller 更新注册状态；一般不应由业务代码直接调用。
    void set_index(int idx) { index_ = idx; }

    // 返回所属 EventLoop；该指针不转移所有权。
    EventLoop* ownerLoop() { return loop_; }

    /* 从 Poller 注销该 Channel；不会关闭 fd，通常在 Channel 析构前调用。 */
    void remove();

    /*
     * 防悬垂：回调里可能把 Channel 的宿主（比如 TcpConnection）析构掉。
     * tie() 传一个 weak_ptr 进来，handleEvent 前先尝试提升成 shared_ptr，
     * 提不上去就说明对象已经没了，直接返回。
     *
     * 注意：第 3 步（只写 Channel + Poller）用不到这个，等 TcpConnection
     * 出现、真的会「回调里自杀」时再加。现在先把接口留在这儿。
     */
    void tie(const std::shared_ptr<void>& obj);

private:
    void update();
    /* 根据 revents_ 分发事件，调用对应回调；由 handleEvent() 在派发事件时调用。 */
    void handleEventWithGuard();

    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;

    EventLoop* loop_;
    const int fd_;
    int events_;        /* 我关心什么 */
    int revents_;       /* 实际发生了什么 */
    int index_;         /* kNew / kAdded / kDeleted 是这三个事件中的哪一个*/

    bool tied_;
    bool eventHandling_;        /* 正在派发事件，此时不许析构 */
    bool addedToLoop_;
    std::weak_ptr<void> tie_;

    EventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};

#endif
