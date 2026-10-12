#include "net/EventLoopThread.h"

#include <stdexcept>

#include "net/EventLoop.h"

EventLoopThread::EventLoopThread(const ThreadInitCallback& cb,
                                 const std::string&)
    : loop_(nullptr),
      exiting_(false),
      started_(false),
      callback_(cb)
{
}

EventLoopThread::~EventLoopThread()
{
    {
        // 上锁，避免和工作线程同时修改共享状态
        std::lock_guard<std::mutex> lock(mutex_);
        // 设置退出标志，通知工作线程停止事件循环
        exiting_ = true;
        // 如果事件循环已经创建，通知其退出
        if (loop_ != nullptr) loop_->quit();
        // 唤醒等待中的线程，让它感知到退出条件已成立
        cond_.notify_one();
    }

    if (thread_.joinable()) thread_.join();
}

EventLoop* EventLoopThread::startLoop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    // 只允许启动一次线程，避免重复创建事件循环线程
    if (thread_.joinable()) {
        throw std::logic_error("EventLoopThread::startLoop called more than once");
    }

    // 创建工作线程，并让它执行 threadFunc
    thread_ = std::thread(&EventLoopThread::threadFunc, this);
    // 等待子线程创建好 EventLoop，并且在未退出时继续等待
    while (loop_ == nullptr && !exiting_) {
        cond_.wait(lock);
    }
    // 此时 loop_ 已经创建成功，或者对象已经进入退出状态
    EventLoop* loop = loop_;
    started_ = true;
    // 唤醒等待中的线程，通知启动流程已完成
    cond_.notify_one();
    return loop;
}

void EventLoopThread::threadFunc()
{
    // EventLoop 在当前工作线程中创建，threadId 也属于当前线程
    EventLoop loop;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 如果对象已经进入退出状态，直接通知等待线程并返回
        if (exiting_) {
            cond_.notify_one();
            return;
        }
        // 否则把新创建的 EventLoop 暴露给外部线程
        loop_ = &loop;
        // 通知 startLoop() 线程，loop 已经准备好
        cond_.notify_one();
        // 在未启动完成且未退出前，等待外部线程确认启动
        while (!started_ && !exiting_) {
            cond_.wait(lock);
        }
        // 如果在未启动前就收到了退出信号，清理状态并返回
        if (exiting_ && !started_) {
            loop_ = nullptr;
            cond_.notify_one();
            return;
        }
    }

    if (callback_) callback_(&loop);
    // 开始事件循环，直到 loop 被退出为止
    loop.loop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = nullptr;
        exiting_ = true;
        cond_.notify_one();
    }
}
