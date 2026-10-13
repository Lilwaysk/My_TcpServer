#include "net/EventLoopThreadPool.h"

#include <cassert>

#include "base/Logger.h"
#include "net/EventLoop.h"
#include "net/EventLoopThread.h"

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, const std::string& nameArg)
    : baseLoop_(baseLoop),
      name_(nameArg),
      started_(false),
      numThreads_(0),
      next_(0)
{
}

EventLoopThreadPool::~EventLoopThreadPool()
{
    // 不需要手动写，unique_ptr 会自动销毁线程对象
    // EventLoopThread 的析构函数会负责 quit + join
}

void EventLoopThreadPool::start(const ThreadInitCallback& cb)
{
    started_ = true;

    for (int i = 0; i < numThreads_; ++i) {
        EventLoopThread* thread = new EventLoopThread(cb, name_ + std::to_string(i));
        threads_.emplace_back(thread);          // 在容器末尾直接构造一个新元素，不用先创建临时对象再拷贝/移动
        loops_.push_back(thread->startLoop());
    }

    if (numThreads_ == 0 && cb) cb(baseLoop_);
}

EventLoop* EventLoopThreadPool::getNextLoop()
{
    if (loops_.empty()) return baseLoop_;

    EventLoop* loop = loops_[static_cast<size_t>(next_)];
    next_ = (next_ + 1) % static_cast<int>(loops_.size());
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops()
{
    return loops_;
}


/*
 * ============ EventLoopThreadPool 实现清单（README 第四节「第 7 步」）============
 *
 * 构造：baseLoop_(baseLoop), name_(nameArg), started_(false),
 *       numThreads_(0), next_(0)。
 *       numThreads_ 默认 0，表示「不额外起线程」，所有连接都归 baseLoop_。
 *
 * start(cb)：
 *      started_ = true;
 *      for (int i = 0; i < numThreads_; ++i) {
 *          EventLoopThread* t = new EventLoopThread(cb, name_ + std::to_string(i));
 *          threads_.push_back(std::unique_ptr<EventLoopThread>(t));
 *          loops_.push_back(t->startLoop());
 *      }
 *      if (numThreads_ == 0 && cb)
 *          cb(baseLoop_);          // 没起线程也要回调一次，保持语义一致
 *
 * getNextLoop()：
 *      EventLoop* loop = baseLoop_;
 *      if (!loops_.empty()) {
 *          loop = loops_[next_];
 *          next_ = (next_ + 1) % loops_.size();
 *      }
 *      return loop;
 *
 *    轮询（round-robin）而不是「谁空闲给谁」。理由：连接一建好就永久绑在
 *    同一个 loop 上，中途不迁移，所以「空闲」这个概念本身就不成立；
 *    而且轮询天然均匀，不用额外统计。
 *
 * getAllLoops()：返回 loops_。写了 TcpClient 之后，
 *       客户端也需要遍历所有 loop 去发起连接。
 *
 * 析构：threads_ 全是 unique_ptr，自动析构。
 *       每个 EventLoopThread 的析构会负责 quit + join，
 *       所以这里什么都不用写。
 *
 * 验收：echo_server 里调 setThreadNum(n)，用压测工具打，
 *       QPS 应该随核数上升；用 top -H 看确实有 n+1 个线程在跑。
 */
