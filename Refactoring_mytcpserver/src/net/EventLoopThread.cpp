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
        std::lock_guard<std::mutex> lock(mutex_);
        exiting_ = true;
        if (loop_ != nullptr) loop_->quit();
        cond_.notify_one();
    }

    if (thread_.joinable()) thread_.join();
}

EventLoop* EventLoopThread::startLoop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (thread_.joinable()) {
        throw std::logic_error("EventLoopThread::startLoop called more than once");
    }

    thread_ = std::thread(&EventLoopThread::threadFunc, this);
    while (loop_ == nullptr && !exiting_) {
        cond_.wait(lock);
    }
    EventLoop* loop = loop_;
    started_ = true;
    cond_.notify_one();
    return loop;
}

void EventLoopThread::threadFunc()
{
    EventLoop loop;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (exiting_) {
            cond_.notify_one();
            return;
        }
        loop_ = &loop;
        cond_.notify_one();
        while (!started_ && !exiting_) {
            cond_.wait(lock);
        }
        if (exiting_ && !started_) {
            loop_ = nullptr;
            cond_.notify_one();
            return;
        }
    }

    if (callback_) callback_(&loop);
    loop.loop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        loop_ = nullptr;
        exiting_ = true;
        cond_.notify_one();
    }
}
