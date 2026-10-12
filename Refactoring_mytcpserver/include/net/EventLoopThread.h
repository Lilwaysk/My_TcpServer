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
    // 定义void(EventLoop*)类型的线程初始化回调函数
    typedef std::function<void(EventLoop*)> ThreadInitCallback;
    // 构造函数：线程初始化回调，线程名
    explicit EventLoopThread(const ThreadInitCallback& cb = ThreadInitCallback(),
                             const std::string& name = std::string());
    ~EventLoopThread();
    // 该线程开始进入事件循环
    EventLoop* startLoop();

private:
    // EventLoopThread 里真正启动新线程后执行的函数
    void threadFunc();

    EventLoop* loop_;               /* threadFunc 里创建并赋值，startLoop 里读取，用于拿到新线程中的 EventLoop */
    bool exiting_;                  // 线程退出标志；析构或停止事件循环时会用到
    bool started_;                  // startLoop() 拿到 loop 后置位，threadFunc 等它才真正进入 loop.loop()
    std::thread thread_;            // 真正运行 threadFunc() 的后台线程对象
    std::mutex mutex_;              // 保护 loop_ 等共享状态，和 cond_ 配合实现线程同步
    std::condition_variable cond_;  // 用于让 startLoop() 等待新线程创建完 EventLoop 并通知已就绪
    ThreadInitCallback callback_;   // 线程启动后执行的初始化回调，可在 loop 运行前做额外设置
};

#endif
