#include "net/TimerQueue.h"

#include <cerrno>
#include <cstring>
#include <sys/timerfd.h>
#include <unistd.h>

#include "base/Logger.h"
#include "net/Channel.h"
#include "net/EventLoop.h"

namespace {
// 重新设置timefd，重新设置到期时间
void resetTimerfd(int timerfd, Timestamp expiration)
{
    /*linux 用来设置定时器到期时间和间隔时间的结构体*/
    struct itimerspec its;
    std::memset(&its, 0, sizeof(its));

    // 计算定时器到期时间与当前时间的差值，单位为微秒
    int64_t diff = expiration.microSecondsSinceEpoch()
                 - Timestamp::now().microSecondsSinceEpoch();

    // 如果差值小于等于 0，表示定时器已经过期，设置为最小值 1 纳秒，确保定时器立即触发
    if (diff <= 0) {
        its.it_value.tv_nsec = 1;  // 立刻触发，不要设置为 0
    } else {
        int64_t sec = diff / Timestamp::kMicroSecondsPerSecond;     // 计算秒数
        int64_t usec = diff % Timestamp::kMicroSecondsPerSecond;    // 计算微秒数
        its.it_value.tv_sec = static_cast<time_t>(sec);             // 设置秒数
        its.it_value.tv_nsec = static_cast<long>(usec * 1000);      // 设置纳秒数（微秒 * 1000 = 纳秒）
    }

    ::timerfd_settime(timerfd, 0, &its, nullptr);                   // 更新定时器的时间
}
}

TimerQueue::TimerQueue(EventLoop* loop) :
    loop_(loop),
    timerfd_(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)),
    timerfdChannel_(new Channel(loop, timerfd_))
{
    timerfdChannel_->setReadCallback([this](){ handleRead(); });
    timerfdChannel_->enableReading();
}

TimerQueue::~TimerQueue()
{
    timerfdChannel_->disableAll();
    timerfdChannel_->remove();
    ::close(timerfd_);
}

void TimerQueue::addTimer(const TimerCallback& cb, Timestamp when, double interval)
{
    // 线程安全入口：内部会转到 loop 线程执行。
    std::shared_ptr<Timer> timer(new Timer(cb, when, interval));    // 创建一个指向 Timer 的 shared_ptr，确保跨线程期间对象不会被销毁
    loop_->runInLoop([this, timer](){ addTimerInLoop(timer); });    // 将添加定时器的操作封装为一个 lambda，并在 loop 线程中执行
}

void TimerQueue::addTimerInLoop(const std::shared_ptr<Timer>& timer)
{
    /*
       insert返回ture表示新定时器比队列里原来最早的定时器到期更早（或者它是队列中唯一的定时器）
       所以要重新设置 timerfd，让它在这个新定时器到期时通知事件循环
       insert返回false表示队列里已有更早的定时器，timerfd 已经会在那个时间触发，不需要重新设置。

       比如原定时器 5 秒后到期，后来加了一个 2 秒后到期的定时器，就必须把 timerfd 改成 2 秒后触发
       如果新加的是 8 秒后到期，则不必改，先让 5 秒后的定时器正常触发即可。

       这里传入的 timer->expiration() 是新定时器的到期时间；resetTimerfd 会据此设置 timerfd
    */
    bool earliestChanged = insert(timer);
    if (earliestChanged) {
        resetTimerfd(timerfd_, timer->expiration());
    }
}

bool TimerQueue::insert(const std::shared_ptr<Timer>& timer)
{
    // 如果集合为空，直接插入集合
    if (timers_.empty()) {
        timers_.insert(Entry{timer->expiration(), timer->sequence(), timer});
        return true;
    }
    // set会自动排序，Entry在内部自定义了排序规则，begin()返回的是剩余时间最少的
    const auto& earliest = *timers_.begin();

    // 我比你早超时，或者时间一样但序号比你早的，则返回true表示要更新了
    bool ealiestChanged = timer->expiration() < earliest.when ||
                        ( timer->expiration() == earliest.when && timer->sequence() < earliest.sequence);

    timers_.insert(Entry{timer->expiration(), timer->sequence(), timer});

    return ealiestChanged;
}

void TimerQueue::handleRead()
{
    // expirations充当到期计时器，read会把timefd到期了几次写进expirations，n返回的是读取的字节数
    uint64_t expirations = 0;
    const ssize_t n = ::read(timerfd_, &expirations, sizeof(expirations));

    // static_cast强制类型转换
    if (n != static_cast<ssize_t>(sizeof(expirations))) {
        if (n < 0) {
            LOG_ERROR << "TimerQueue::handleRead read failed: "
                      << std::strerror(errno);
        } else {
            LOG_ERROR << "TimerQueue::handleRead read " << n
                      << " bytes instead of " << sizeof(expirations);
        }
        return;
    }

    const Timestamp now = Timestamp::now();
    const std::vector<Entry> expired = getExpired(now);

    for (auto entry : expired) entry.timer->run();

    reset(expired, now);
}

std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now)
{
    std::vector<Entry> expired;

    auto it = timers_.begin();
    while (it != timers_.end() && !(now < it->when)) {
        expired.push_back(*it);
        it = timers_.erase(it);
    }

    return expired;
}

void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp now)
{
    for (auto entry : expired) {
        // repeat判断是周期定时器还是一次性定时器
        if (entry.timer->repeat()) {
            entry.timer->restart(now);
            insert(entry.timer);
        }
    }

    // 定时机队列非空时，就取 timers_.begin()，也就是最早到期的定时器——重新设置 timerfd，让它在那个时间触发。
    if (!timers_.empty()) resetTimerfd(timerfd_, timers_.begin()->when);
}

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
