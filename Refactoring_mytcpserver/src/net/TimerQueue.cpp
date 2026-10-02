#include "net/TimerQueue.h"

#include <cstring>
#include <sys/timerfd.h>
#include <unistd.h>

#include "base/Logger.h"
#include "net/Channel.h"
#include "net/EventLoop.h"

/*
 * ============ TimerQueue 实现清单（README 第四节「第 4 步」）============
 *
 * 核心思路：timerfd + set<(到期时间, 序号)>，不再手工算 epoll_wait 超时。
 *
 * 1) 构造
 *      timerfd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
 *      timerfdChannel_.reset(new Channel(loop, timerfd_));
 *      timerfdChannel_->setReadCallback([this]{ handleRead(); });
 *      timerfdChannel_->enableReading();
 *
 *    为什么用 CLOCK_MONOTONIC 而不是 CLOCK_REALTIME：NTP 校时会让墙上时钟跳变。
 *    CLOCK_MONOTONIC 单调递增，算时间间隔才可靠。
 *
 * 2) 析构
 *      timerfdChannel_->disableAll();
 *      timerfdChannel_->remove();
 *      ::close(timerfd_);
 *
 * 3) addTimer(cb, when, interval) —— 线程安全入口
 *      用 loop_->runInLoop(...) 包一层，让真正的活儿在 loop 线程里干。
 *      注意要 bind 住 shared_ptr<Timer>，别传裸指针进去，否则跨线程期间
 *      对象可能已经没了。
 *
 * 4) addTimerInLoop(timer)
 *      insert(timer);
 *      如果新定时器成了 timers_ 里最早的一个，就重新 arm timerfd。
 *
 * 5) insert(timer) —— 返回 bool
 *      把 Entry 塞进 timers_，然后比较它是不是 begin()。
 *      是就说明「最近到期时间提前了」，需要重新 arm。
 *
 * 6) resetTimerfd(timerfd, expiration) —— 建议做成文件内 static 辅助函数
 *      struct itimerspec its;
 *      its.it_value = 换算成 timespec;
 *      its.it_interval = {0, 0};
 *      ::timerfd_settime(timerfd, 0, &its, nullptr);
 *
 *      两个最容易错的地方：
 *        a. it_value 是「相对时间」，不是绝对时间。因为用的是 CLOCK_MONOTONIC，
 *           要算 expiration - Timestamp::now() 的差值。
 *        b. 换算单位：Timestamp 是微秒，timespec 是 (秒, 纳秒)。
 *           别把纳秒当微秒填。
 *
 *      如果差值 <= 0（已经过期了），就把 it_value 设成一个极小值
 *      （比如 1 纳秒），让 timerfd 立刻可读，而不是填 0 —— 填 0 表示「取消定时器」。
 *
 * 7) handleRead() —— timerfd 到期
 *      uint64_t howmany = 0;
 *      ::read(timerfd_, &howmany, sizeof howmany);   // 必须读走，否则一直触发
 *      Timestamp now = Timestamp::now();
 *      std::vector<Entry> expired = getExpired(now);
 *      for (const Entry& e : expired) e.timer->run();
 *      reset(expired, now);
 *
 *      「先 getExpired（从 set 里摘掉）→ 再 run → 最后 reset」这个顺序很关键：
 *      如果某个回调里又加了新定时器，它在 run 期间插进 set，
 *      不会干扰这次已经摘出来的那批。
 *
 * 8) getExpired(now) —— 建议做成 private 成员（本文件里实现）
 *      用 Entry 的 operator< 配合 std::lower_bound({now, 最大序号, nullptr})
 *      就能定位到第一个「晚于 now」的元素，[begin, it) 全是已到期的。
 *      把它们拷贝进 vector 返回，同时从 set 里 erase 掉。
 *      注意模板参数不能写裸 nullptr，要显式构造一个 Entry 当哨兵。
 *
 * 9) reset(expired, now)
 *      周期性定时器：逐个 restart(now) 再插回去；
 *      然后若 timers_ 非空，按新的最早到期时间重新 arm timerfd。
 *
 * 验收：tests/test_timer.cpp 里跑几个一次性 + 周期定时器，
 *       打印实际触发时刻相对预期时刻的偏差。
 */

// namespace {
// void resetTimerfd(int timerfd, Timestamp expiration) { /* ... */ }
// }
//
// TimerQueue::TimerQueue(EventLoop* loop) : loop_(loop), timerfd_(0) { /* ... */ }
// TimerQueue::~TimerQueue() { /* ... */ }
// void TimerQueue::addTimer(const TimerCallback& cb, Timestamp when, double interval) { /* ... */ }
// void TimerQueue::addTimerInLoop(const std::shared_ptr<Timer>& timer) { /* ... */ }
// bool TimerQueue::insert(const std::shared_ptr<Timer>& timer) { /* ... */ }
// void TimerQueue::handleRead() { /* ... */ }
// std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now) { /* ... */ }
// void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp now) { /* ... */ }
