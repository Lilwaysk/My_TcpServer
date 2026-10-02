#ifndef NET_EVENTLOOPTHREADPOOL_H
#define NET_EVENTLOOPTHREADPOOL_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/noncopyable.h"

class EventLoop;
class EventLoopThread;

/*
 * IO 线程池。注意它和「业务线程池」是两回事：
 *
 *   - EventLoopThreadPool：每个线程跑一个 EventLoop，负责收发字节。
 *     线程数通常等于 CPU 核数，因为它是 IO 密集的。
 *   - 业务线程池（threadpool.cpp 那套）：负责耗时的业务计算，
 *     避免阻塞 IO 线程。等 echo 改成有耗时逻辑时再加。
 *
 * 分配策略是轮询（round-robin），和 muduo 一致。比「谁空闲给谁」简单，
 * 而且连接建立后一直绑在同一个 loop 上，不需要迁移。
 */
class EventLoopThreadPool : noncopyable {
public:
    typedef std::function<void(EventLoop*)> ThreadInitCallback;

    EventLoopThreadPool(EventLoop* baseLoop, const std::string& nameArg);
    ~EventLoopThreadPool();

    void setThreadNum(int numThreads) { numThreads_ = numThreads; }

    void start(const ThreadInitCallback& cb = ThreadInitCallback());

    /* 轮询取下一个 IO 线程的 EventLoop；没起线程时返回 baseLoop_ */
    EventLoop* getNextLoop();

    std::vector<EventLoop*> getAllLoops();

    bool started() const { return started_; }
    const std::string& name() const { return name_; }

private:
    EventLoop* baseLoop_;           /* 主线程的 loop */
    std::string name_;
    bool started_;
    int numThreads_;
    int next_;                      /* 轮询下标 */
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};

#endif
