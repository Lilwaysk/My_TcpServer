#ifndef BASE_TIMESTAMP_H
#define BASE_TIMESTAMP_H

#include <cstdint>
#include <ctime>
#include <string>

/*
 * 时间戳，内部统一用「微秒」存。
 *
 * 替换掉老代码里的 time(NULL) 和 now_ms()：秒级精度做不了定时器，
 * 你原来的超时检测最小粒度就是 1 秒。
 */
class Timestamp {
public:
    static const int kMicroSecondsPerSecond = 1000 * 1000;      // static: 属于整个类共用一份的变量; const: 不可修改

    Timestamp() : microSecondsSinceEpoch_(0) {}
    explicit Timestamp(int64_t microSecondsSinceEpoch)          // explict: 禁止隐式类型转换
        : microSecondsSinceEpoch_(microSecondsSinceEpoch) {}

    static Timestamp now();                                     // 静态成员函数: 不用对象也可直接调用，是类级别的函数;但本身无this指针，所以不能直接操作对象自己的成员
    static Timestamp invalid() { return Timestamp(); }

    bool valid() const { return microSecondsSinceEpoch_ > 0; }  // 函数名后面加const: 表示该函数不会修改当前对象

    int64_t microSecondsSinceEpoch() const { return microSecondsSinceEpoch_; }

    time_t secondsSinceEpoch() const {
        return static_cast<time_t>(microSecondsSinceEpoch_ / kMicroSecondsPerSecond);
    }

    std::string toString() const;

private:
    int64_t microSecondsSinceEpoch_;
};

inline bool operator<(Timestamp lhs, Timestamp rhs)
{
    return lhs.microSecondsSinceEpoch() < rhs.microSecondsSinceEpoch();
}

inline bool operator==(Timestamp lhs, Timestamp rhs)
{
    return lhs.microSecondsSinceEpoch() == rhs.microSecondsSinceEpoch();
}

/* 定时器要用的：timestamp + seconds */
inline Timestamp addTime(Timestamp timestamp, double seconds)
{
    int64_t delta = static_cast<int64_t>(seconds * Timestamp::kMicroSecondsPerSecond);
    return Timestamp(timestamp.microSecondsSinceEpoch() + delta);
}

#endif
