#ifndef NET_EVENTLOOPTHREAD_H
#define NET_EVENTLOOPTHREAD_H

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "base/noncopyable.h"

class EventLoop;

/*
 * 「起一个线程，在里面跑一个 EventLoop」。
 *
 * 这是主从 Reactor 的零件：主线程 accept 到连接后，从中挑一个
 * EventLoopThread 的 loop，把连接绑上去，之后这条连接的所有读写
 * 都在那个线程里发生。
 *
 * startLoop() 一定会等 EventLoop 真正建好才返回 —— 拿到指针就能直接用，
 * 不用在外面 sleep 或者轮询。这就是下面 mutex_ + cond_ 的用途。
 */
class EventLoopThread : noncopyable {
public:
    typedef std::function<void(EventLoop*)> ThreadInitCallback;

    explicit EventLoopThread(const ThreadInitCallback& cb = ThreadInitCallback(),
                             const std::string& name = std::string());
    ~EventLoopThread();

    EventLoop* startLoop();

private:
    void threadFunc();

    EventLoop* loop_;               /* threadFunc 里赋值，startLoop 里读取 */
    bool exiting_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cond_;
    ThreadInitCallback callback_;
};

#endif
