#include "base/Timestamp.h"

#include <chrono>
#include <cstdio>

Timestamp Timestamp::now()
{
    /*
     * 用 std::chrono 而不是 clock_gettime，两个好处：
     *   1. 不依赖平台差异，Windows / Linux 都能编；
     *   2. 精度直接到微秒。
     *
     * 注意：system_clock 会被系统校时（NTP）影响。严格来说定时器应该用
     * steady_clock 算间隔，这里为了打日志可读先用 system_clock。
     */
    auto now = std::chrono::system_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  now.time_since_epoch()).count();

    return Timestamp(static_cast<int64_t>(us));
}

std::string Timestamp::toString() const
{
    char buf[64] = {0};
    time_t seconds = secondsSinceEpoch();
    int microSeconds = static_cast<int>(microSecondsSinceEpoch_ % kMicroSecondsPerSecond);

    struct tm tm_time;
#ifdef _WIN32
    localtime_s(&tm_time, &seconds);
#else
    localtime_r(&seconds, &tm_time);
#endif

    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06d",
             tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
             tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec, microSeconds);

    return std::string(buf);
}
