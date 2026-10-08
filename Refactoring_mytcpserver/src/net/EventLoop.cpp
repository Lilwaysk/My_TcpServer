#include "net/EventLoop.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sys/eventfd.h>
#include <unistd.h>

#include "base/Logger.h"
#include "net/Channel.h"
#include "net/TimerQueue.h"
#include <assert.h>

namespace {
    const int kPollTimeMs = 10000;
}

bool EventLoop::isInLoopThread() const { return threadId_ == std::this_thread::get_id(); }
void EventLoop::assertInLoopThread() { if (!isInLoopThread()) abortNotInLoopThread(); }

EventLoop::EventLoop():
    looping_(false),
    quit_(false),
    eventHandling_(false),
    callingPendingFunctors_(false)
{
    threadId_ = std::this_thread::get_id();
    poller_.reset(new Poller(this));                                    // .reset() 会先 delete 再 new，确保 poller_ 先析构
    wakeupFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);               // 创建一个 eventfd，用于线程间的唤醒，设置为非阻塞和启动新程序时自动关闭
    wakeupChannel_.reset(new Channel(this, wakeupFd_));                 // 创建一个 Channel 对象，注册到 wakeupFd_ 上，用于处理唤醒事件
    wakeupChannel_->setReadCallback([this](){ handleWakeupRead(); });   // 设置 wakeupChannel_ 的读事件回调函数，当有唤醒事件时调用 handleWakeupRead() 处理
    wakeupChannel_->enableReading();                                    // 启用 wakeupChannel_ 的读事件监听，使其可以接收唤醒事件
    timerQueue_.reset(new TimerQueue(this));                            // 创建一个 TimerQueue 对象，用于管理定时器事件
}

EventLoop::~EventLoop()
{
    assert(!looping_);                                                   // 确保在析构时事件循环没有在运行
    wakeupChannel_->disableAll();                                        // 禁用 wakeupChannel_ 的所有事件监听
    wakeupChannel_->remove();                                            // 从 Poller 中移除 wakeupChannel_
    ::close(wakeupFd_);                                                  // 关闭 wakeupFd_，释放资源
}

void EventLoop::loop()
{
    assertInLoopThread();
    looping_ = true;                                                     // 设置事件循环正在运行的标志
    quit_ = false;                                                       // 重置退出标志
    while (!quit_) {
        activeChannels_.clear();
        poller_->poll(kPollTimeMs, &activeChannels_);
        eventHandling_ = true;
        for (Channel* channel : activeChannels_) {
            channel->handleEvent();                                      // 处理每个活跃的 Channel 的事件
        }
        eventHandling_ = false;
        doPendingFunctors();                                             // 执行所有排队的回调函数
    }
    looping_ = false;                                                    // 循环结束
    quit_ = true;
}

void EventLoop::quit()
{
    quit_ = true;
    /*
     * 如果是在 loop 线程里调的，当前这一轮事件处理完、回到 while 顶部
     * 就会看到 quit_ 退出，不需要（也不能依赖）wakeup。
     * 如果是在别的线程里调的，loop 此刻很可能正阻塞在 epoll_wait 上，
     * 必须踹它一脚，否则要等到下一个事件到来才会发现 quit_。
     */
    if (!isInLoopThread()) wakeup();
}

void EventLoop::runInLoop(const Functor& cb)
{
    if (isInLoopThread()) cb();  // 如果当前线程是事件循环线程，直接执行回调函数
    else queueInLoop(cb);        // 否则将回调函数加入队列，等待事件循环线程执行
}

void EventLoop::queueInLoop(const Functor& cb)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);   // 加锁保护 alreadyFunctors_ 队列，这是共享队列，可能被别的线程访问
        alreadyFunctors_.push_back(cb);             // 将回调函数加入队列，入队完就释放锁
    }
    /*
        第一个条件是相当于 现在线程空闲阻塞在wait等事件，这个wakeup是把它唤醒来处理
        第二个条件 就相当于让处理完第一批的回头重新回来处理第二批
    */
    if (!isInLoopThread() || callingPendingFunctors_) wakeup();  // 如果当前线程不是事件循环线程，或者正在执行回调函数，则唤醒事件循环线程
}

void EventLoop::doPendingFunctors()
{
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;  // 设置正在执行回调函数的标志
    {
        std::lock_guard<std::mutex> lock(mutex_);  // 加锁保护 alreadyFunctors_ 队列
        functors.swap(alreadyFunctors_);           // 交换队列，将待执行的回调函数取出，避免在执行过程中持锁
    }

    for (const Functor& functor : functors) functor();  // 执行每个回调函数

    callingPendingFunctors_ = false;  // 执行完毕，重置
}

void EventLoop::wakeup()
{
    uint64_t one = 1;  // 写入 eventfd 的值，表示唤醒事件
    ssize_t n = ::write(wakeupFd_, &one, sizeof(one));  // 向 wakeupFd_ 写入数据，触发唤醒事件，write后内核计数器加一，epoll_wait就会返回
    if (n != sizeof(one)) {
        LOG_ERROR << "EventLoop::wakeup() writes " << n << " bytes instead of 8";  // 写入失败，记录错误日志
    }
}

void EventLoop::handleWakeupRead()
{
    uint64_t one = 1;  // 读取 eventfd 的值，表示唤醒事件已处理
    ssize_t n = ::read(wakeupFd_, &one, sizeof(one));  // 从 wakeupFd_ 读取数据，清除唤醒事件， read后内核计数器减一，epoll_wait就不会再返回
    if (n != sizeof(one)) {
        LOG_ERROR << "EventLoop::handleWakeupRead() reads " << n << " bytes instead of 8";  // 读取失败，记录错误日志
    }
}

void EventLoop::updateChannel(Channel* channel)
{
    assert(channel->ownerLoop() == this);  // 确保 Channel 属于当前 EventLoop
    assertInLoopThread();                  // 确保在事件循环线程中调用
    poller_->updateChannel(channel);       // 调用 Poller 的 updateChannel 方法，更新 Channel 的注册状态
}

void EventLoop::removeChannel(Channel* channel)
{
    assert(channel->ownerLoop() == this);  // 确保 Channel 属于当前 EventLoop
    assertInLoopThread();                  // 确保在事件循环线程中调用
    poller_->removeChannel(channel);       // 调用 Poller 的 removeChannel 方法，移除 Channel 的注册状态
}

void EventLoop::abortNotInLoopThread()
{
    LOG_FATAL << "EventLoop::abortNotInLoopThread - EventLoop " << this
              << " was created in threadId_ = " << threadId_
              << ", current thread id = " << std::this_thread::get_id();
    abort();  // 打印日志后直接终止程序，防止多线程错误使用 EventLoop
}

void EventLoop::runAt(Timestamp time, const TimerCallback& cb)
{
    timerQueue_->addTimer(cb, time, 0.0);  // 将定时器事件添加到 TimerQueue 中，指定时间点执行回调函数
}

void EventLoop::runAfter(double delay, const TimerCallback& cb)
{
    Timestamp time(Timestamp::now().microSecondsSinceEpoch() + static_cast<int64_t>(delay * Timestamp::kMicroSecondsPerSecond));  // 计算延迟时间对应的时间戳
    timerQueue_->addTimer(cb, time, 0.0);  // 将定时器事件添加到 TimerQueue 中，指定延迟时间后执行回调函数
}

void EventLoop::runEvery(double interval, const TimerCallback& cb)
{
    Timestamp time(Timestamp::now().microSecondsSinceEpoch() + static_cast<int64_t>(interval * Timestamp::kMicroSecondsPerSecond));  // 计算间隔时间对应的时间戳
    timerQueue_->addTimer(cb, time, interval);  // 将定时器事件添加到 TimerQueue 中，指定间隔时间周期性执行回调函数
}

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
 *            alreadyFunctors_.push_back(cb); }
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
 *        functors.swap(alreadyFunctors_); }     // 先换出来，再执行
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
