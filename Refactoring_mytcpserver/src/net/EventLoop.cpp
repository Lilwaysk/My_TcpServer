#include "net/EventLoop.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sys/eventfd.h>
#include <unistd.h>

#include "base/Logger.h"
#include "net/Channel.h"
#include "net/TimerQueue.h"

/*
 * ============ EventLoop 实现清单（README 第四节「第 4 步」）============
 *
 * 建议的落笔顺序：
 *
 * 1) 构造
 *      threadId_ = std::this_thread::get_id();
 *      poller_.reset(new Poller(this));
 *      wakeupFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
 *      wakeupChannel_.reset(new Channel(this, wakeupFd_));
 *      wakeupChannel_->setReadCallback([this]{ handleWakeupRead(); });
 *      wakeupChannel_->enableReading();
 *      timerQueue_.reset(new TimerQueue(this));
 *    poller_ 必须比 wakeupChannel_ 先建：Channel 构造里就会用到 loop_。
 *
 * 2) 析构
 *      assert(!looping_);
 *      wakeupChannel_->disableAll();
 *      wakeupChannel_->remove();
 *      ::close(wakeupFd_);
 *
 * 3) loop() —— 主循环
 *      assertInLoopThread();
 *      looping_ = true;  quit_ = false;
 *      while (!quit_) {
 *          activeChannels_.clear();
 *          poller_->poll(kPollTimeMs, &activeChannels_);   // 阻塞点
 *          eventHandling_ = true;
 *          for (Channel* ch : activeChannels_) ch->handleEvent();
 *          eventHandling_ = false;
 *          doPendingFunctors();
 *      }
 *      looping_ = false;
 *
 *    kPollTimeMs 给 10000 就行（muduo 就是这么给的）。因为定时器走 timerfd，
 *    不需要靠 epoll_wait 超时来驱动，超时值给大一点没坏处。
 *
 *    quit() 里必须调 wakeup()：loop 可能正阻塞在 epoll_wait 里，
 *    不踹它一脚，quit_ 要等到下一个事件才被看见。
 *
 * 4) runInLoop / queueInLoop —— 整个库线程模型的基石
 *
 *      void EventLoop::runInLoop(const Functor& cb) {
 *          if (isInLoopThread()) cb();     // 已经在本线程，直接同步执行
 *          else queueInLoop(cb);           // 否则投递进去
 *      }
 *
 *      void EventLoop::queueInLoop(const Functor& cb) {
 *          { std::lock_guard<std::mutex> lock(mutex_);
 *            pendingFunctors_.push_back(cb); }
 *          if (!isInLoopThread() || callingPendingFunctors_)
 *              wakeup();
 *      }
 *
 *    第二个条件 callingPendingFunctors_ 是面试高频点，务必想明白：
 *    doPendingFunctors 用 swap 把队列整个换出来执行，执行期间新投递的任务
 *    落在新的空队列里、下一轮才跑。此刻不 wakeup 的话，下一轮 poll()
 *    会直接阻塞在那儿，那个任务就被卡住了。
 *
 * 5) doPendingFunctors()
 *      std::vector<Functor> functors;
 *      callingPendingFunctors_ = true;
 *      { std::lock_guard<std::mutex> lock(mutex_);
 *        functors.swap(pendingFunctors_); }     // 先换出来，再执行
 *      for (auto& f : functors) f();
 *      callingPendingFunctors_ = false;
 *
 *    用 swap 的两个理由：不持锁执行用户回调；回调里再投递不会让迭代器失效。
 *
 * 6) wakeup / handleWakeupRead
 *      wakeup：uint64_t one = 1; ::write(wakeupFd_, &one, sizeof one);
 *              非阻塞 fd，写失败（EAGAIN）忽略即可。
 *      handleWakeupRead：把计数读走。水平触发下不读走会一直触发。
 *
 * 7) updateChannel / removeChannel
 *      assert(channel->ownerLoop() == this);
 *      assertInLoopThread();
 *      poller_->updateChannel(channel);
 *
 * 8) assertInLoopThread / abortNotInLoopThread
 *      误用的日志里一定要打出「当前线程 id」和「loop 线程 id」，
 *      否则多线程 bug 根本没法查。abortNotInLoopThread 打印完直接 abort。
 *
 * 9) runAt / runAfter / runEvery —— 三行转发给 timerQueue_->addTimer(...)
 *
 * 验收：把老代码的主循环搬进来，行为和以前一致。
 */

// namespace {
// const int kPollTimeMs = 10000;
// }
//
// EventLoop::EventLoop()
//     : looping_(false), quit_(false), eventHandling_(false),
//       callingPendingFunctors_(false), threadId_(std::this_thread::get_id()),
//       poller_(new Poller(this)),
//       wakeupFd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
// {
//     /* wakeupChannel_ / timerQueue_ 照着上面的顺序补 */
// }
//
// EventLoop::~EventLoop() { /* ... */ }
// void EventLoop::loop() { /* ... */ }
// void EventLoop::quit() { /* ... */ }
// void EventLoop::runInLoop(const Functor& cb) { /* ... */ }
// void EventLoop::queueInLoop(const Functor& cb) { /* ... */ }
// void EventLoop::wakeup() { /* ... */ }
// void EventLoop::handleWakeupRead() { /* ... */ }
// void EventLoop::doPendingFunctors() { /* ... */ }
// void EventLoop::updateChannel(Channel* channel) { /* ... */ }
// void EventLoop::removeChannel(Channel* channel) { /* ... */ }
// bool EventLoop::isInLoopThread() const { return threadId_ == std::this_thread::get_id(); }
// void EventLoop::assertInLoopThread() { if (!isInLoopThread()) abortNotInLoopThread(); }
// void EventLoop::abortNotInLoopThread() { /* 打日志 + abort */ }
// void EventLoop::runAt(Timestamp time, const TimerCallback& cb) { /* ... */ }
// void EventLoop::runAfter(double delay, const TimerCallback& cb) { /* ... */ }
// void EventLoop::runEvery(double interval, const TimerCallback& cb) { /* ... */ }
