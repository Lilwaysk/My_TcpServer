#ifndef NET_TIMERQUEUE_H
#define NET_TIMERQUEUE_H

#include <cstdint>
#include <memory>
#include <set>
#include <vector>

#include "base/Timestamp.h"
#include "base/noncopyable.h"
#include "net/Callbacks.h"
#include "net/Timer.h"

class EventLoop;
class Channel;

/*
 * 定时器队列，替换掉老代码里的最小堆 + timer_expire_process。
 *
 * 实现方式和以前不一样：不用「算最近到期时间去设 epoll_wait 超时」，
 * 而是拿一个 timerfd 当定时器的载体 —— 把最近到期时间写进 timerfd，
 * 它到期后自己变成「可读事件」，混在同一个 epoll 里通知你。
 *
 * 好处是 epoll_wait 可以给一个很大的超时值，事件分发逻辑保持单一。
 *
 * 定时器按到期时间排序，同一时刻到期的按加入顺序触发。
 * 时间用 Timestamp（微秒），不是老代码里的秒 —— 秒级精度做不了心跳。
 */
class TimerQueue : noncopyable {
public:
    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    /*
     * 线程安全入口：内部会转到 loop 线程执行。
     * interval > 0 表示周期性定时器。
     */
    void addTimer(const TimerCallback& cb, Timestamp when, double interval);

private:
    /*
     * 排序键是 (到期时间, 序号)，不能只用 Timer* 比较 ——
     * 那样同一时刻到期的先后顺序就变成看指针地址了，不可复现。
     */
    struct Entry {
        Timestamp when;
        int64_t sequence;
        std::shared_ptr<Timer> timer;

        bool operator<(const Entry& rhs) const
        {
            if (when < rhs.when)
                return true;
            if (rhs.when < when)
                return false;
            return sequence < rhs.sequence;
        }
    };
    typedef std::set<Entry> TimerList;

    void addTimerInLoop(const std::shared_ptr<Timer>& timer);
    void handleRead();                                  /* timerfd 到期了 */
    std::vector<Entry> getExpired(Timestamp now);
    void reset(const std::vector<Entry>& expired, Timestamp now);
    bool insert(const std::shared_ptr<Timer>& timer);

    EventLoop* loop_;
    const int timerfd_;
    std::unique_ptr<Channel> timerfdChannel_;
    TimerList timers_;
};

#endif
