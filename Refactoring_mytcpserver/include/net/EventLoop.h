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
    typedef std::function<void()> Functor;      // 回调函数
    typedef std::vector<Channel*> ChannelList;  // 事件循环里有事件的 Channel 指针数组

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
    void runInLoop(const Functor& cb);      // 在事件循环线程中执行回调函数

    void queueInLoop(const Functor& cb);    // 在事件循环线程中投递排队回调函数，等到下一轮循环再执行

    /* 唤醒阻塞在 epoll_wait 的 loop */
    void wakeup();                          // 唤醒事件循环等待的线程，通常是为了让它处理新加入的任务

    /* 必须在 loop 线程里调用 */
    void updateChannel(Channel* channel);   // 更新 Channel 的 epoll 注册状态
    void removeChannel(Channel* channel);   // 移除 Channel 的 epoll 注册状态

    void assertInLoopThread();              // 确保 EventLoop 只能在它所属的线程中使用
    bool isInLoopThread() const;            // 判断当前线程是否是 EventLoop 所属的线程
    bool eventHandling() const { return eventHandling_; }   // 判断当前是否正在处理事件

    void runAt(Timestamp time, const TimerCallback& cb);        // 在指定时间点执行回调函数
    void runAfter(double delay, const TimerCallback& cb);       // 在指定延迟时间后执行回调函数
    void runEvery(double interval, const TimerCallback& cb);    // 每隔指定时间间隔执行回调函数

private:
    void handleWakeupRead();                // 处理唤醒事件，通常是读取 eventfd 的计数
    void doPendingFunctors();               // 执行所有排队的回调函数，通常在事件循环中调用
    void abortNotInLoopThread();            // 如果当前线程不是 EventLoop 所属的线程，打印日志并终止程序

    bool looping_;                          // 事件循环是否正在运行
    std::atomic<bool> quit_;                // 是否退出事件循环(原子类型)
    bool eventHandling_;                    // 当前是否正在处理事件
    bool callingPendingFunctors_;           // 当前是否正在执行排队的回调函数
    std::thread::id threadId_;              /* 谁创建我，我就是谁的事件循环 */

    std::unique_ptr<Poller> poller_;        // Poller 对象，用于封装 epoll 的操作
    int wakeupFd_;                              /* eventfd */
    std::unique_ptr<Channel> wakeupChannel_;    /* 唯一被 EventLoop 自己持有的 Channel */
    ChannelList activeChannels_;                // 当前有事件的 Channel 数组，Poller 填入本轮实际发生事件的 Channel 指针
    std::unique_ptr<TimerQueue> timerQueue_;    // 定时器队列，管理定时器事件

    /*
     * 这里是整个库唯一必须加锁的地方：跨线程投递任务。
     */
    mutable std::mutex mutex_;              // 保护 alreadyFunctors_ 的互斥锁
    std::vector<Functor> alreadyFunctors_;  // 待执行的回调函数数组，由 queueInLoop() 投递进来，EventLoop 在下一轮循环中执行
};

#endif
