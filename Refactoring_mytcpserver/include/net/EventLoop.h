#ifndef NET_EVENTLOOP_H
#define NET_EVENTLOOP_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "base/Timestamp.h"
#include "base/noncopyable.h"
#include "net/Callbacks.h"
#include "net/Poller.h"     /* 只是普通头文件，不含 epoll，所以放心 include */

class Channel;
class TimerQueue;

/*
 * 每线程一个的事件循环，对应你老代码里那个 while(1) 主循环。
 *
 * 线程模型一句话：主线程跑一个 EventLoop 只负责 accept，
 * 每个 IO 线程跑自己的 EventLoop 负责收发。所有跨线程操作
 * 都通过 runInLoop / queueInLoop 挪到目标线程去执行 —— 于是
 * 业务回调里永远不需要加锁。
 */
class EventLoop : noncopyable {
public:
    typedef std::function<void()> Functor;
    typedef std::vector<Channel*> ChannelList;

    EventLoop();
    ~EventLoop();

    /* 当前线程必须就是创建它的那个线程 */
    void loop();

    /* 线程安全：可以从别的线程调，会唤醒 loop 让它退出 */
    void quit();

    /*
     * 线程安全。如果已经在 loop 线程里就同步执行，
     * 否则排队进去、由 loop 线程执行。
     * TcpConnection 的所有写操作都靠它回到本线程。
     */
    void runInLoop(const Functor& cb);

    /* 线程安全。总是排队，不立刻执行 */
    void queueInLoop(const Functor& cb);

    /* 唤醒阻塞在 epoll_wait 的 loop */
    void wakeup();

    /* 必须在 loop 线程里调用 */
    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);

    void assertInLoopThread();
    bool isInLoopThread() const;
    bool eventHandling() const { return eventHandling_; }

    /* 定时器三件套，只是转发给 TimerQueue */
    void runAt(Timestamp time, const TimerCallback& cb);
    void runAfter(double delay, const TimerCallback& cb);
    void runEvery(double interval, const TimerCallback& cb);

private:
    void handleWakeupRead();
    void doPendingFunctors();
    void abortNotInLoopThread();

    bool looping_;
    std::atomic<bool> quit_;
    bool eventHandling_;
    bool callingPendingFunctors_;
    std::thread::id threadId_;      /* 谁创建我，我就是谁的事件循环 */

    std::unique_ptr<Poller> poller_;
    int wakeupFd_;                              /* eventfd */
    std::unique_ptr<Channel> wakeupChannel_;    /* 唯一被 EventLoop 自己持有的 Channel */
    ChannelList activeChannels_;
    std::unique_ptr<TimerQueue> timerQueue_;

    /*
     * 这里是整个库唯一必须加锁的地方：跨线程投递任务。
     */
    mutable std::mutex mutex_;
    std::vector<Functor> pendingFunctors_;
};

#endif
