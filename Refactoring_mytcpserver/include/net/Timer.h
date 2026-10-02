#ifndef NET_TIMER_H
#define NET_TIMER_H

#include <atomic>
#include <cstdint>

#include "base/Timestamp.h"
#include "base/noncopyable.h"
#include "net/Callbacks.h"

/*
 * 一个定时器句柄。对象很小，全部 inline，所以没有 Timer.cpp。
 *
 * 老代码里的定时器只会「到期关连接」，这里改成通用的「到期执行回调」——
 * 后面心跳、空闲超时、重连退避都能复用它。
 */
class Timer : noncopyable {
public:
    Timer(const TimerCallback& cb, Timestamp when, double interval)
        : callback_(cb),
          expiration_(when),
          interval_(interval),
          repeat_(interval > 0.0),      /* interval > 0 表示周期性定时器 */
          sequence_(nextSequence()) {}

    void run() const
    {
        if (callback_)
            callback_();
    }

    Timestamp expiration() const { return expiration_; }
    bool repeat() const { return repeat_; }

    /* 加入顺序的序号，用来给同一时刻到期的定时器排序（见 TimerQueue::Entry）*/
    int64_t sequence() const { return sequence_; }

    /* 周期性定时器触发后，重新计算下次到期时间 */
    void restart(Timestamp now)
    {
        if (repeat_)
            expiration_ = addTime(now, interval_);
        else
            expiration_ = Timestamp::invalid();
    }

private:
    /*
     * 全局自增序号。用函数内 static 而不是类内 static 成员，
     * 是为了让 Timer 保持 header-only（C++11 没有 inline 变量）。
     */
    static int64_t nextSequence()
    {
        static std::atomic<int64_t> counter(0);
        return counter.fetch_add(1) + 1;
    }

    const TimerCallback callback_;
    Timestamp expiration_;
    const double interval_;     /* 秒；0 表示一次性 */
    const bool repeat_;
    const int64_t sequence_;
};

#endif
