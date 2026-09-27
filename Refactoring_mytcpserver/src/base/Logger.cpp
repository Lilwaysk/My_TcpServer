#include "base/Logger.h"

#include <cstdio>
#include <mutex>

#include "base/Timestamp.h"

namespace {

LogLevel g_level = LogLevel::INFO;
std::mutex g_mutex;

}  // namespace

Logger::Logger(const char* file, int line, LogLevel level)
    : file_(file), line_(line), level_(level) {}

Logger::~Logger()
{
    /*
     * 一条日志一次性写出去：加锁 + 一次 fprintf。
     * 多线程下如果分多次 printf，日志会互相穿插，排查问题时根本没法看。
     */
    std::string text = stream_.str();
    if (!text.empty() && text.back() == '\n')
        text.pop_back();

    std::lock_guard<std::mutex> lock(g_mutex);
    fprintf(stderr, "%s [%s] %s:%d - %s\n",
            Timestamp::now().toString().c_str(),
            Logger::levelName(level_),
            file_, line_, text.c_str());
    fflush(stderr);
}

void Logger::setLevel(LogLevel level)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = level;
}

LogLevel Logger::level()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_level;
}

const char* Logger::levelName(LogLevel level)
{
    switch (level) {
    case LogLevel::TRACE: return "TRACE";
    case LogLevel::DEBUG: return "DEBUG";
    case LogLevel::INFO:  return "INFO ";
    case LogLevel::WARN:  return "WARN ";
    case LogLevel::ERROR: return "ERROR";
    case LogLevel::FATAL: return "FATAL";
    }
    return "UNKNOWN";
}
