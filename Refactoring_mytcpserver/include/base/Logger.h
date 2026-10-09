#ifndef BASE_LOGGER_H
#define BASE_LOGGER_H

#include <sstream>
#include <string>

/*
 * 极简分级日志，替换掉散落各处的 printf。
 *
 * 为什么必须换：printf 会锁 stdout，而且没法按级别关掉。
 * 压测时把级别调到 WARN，立刻就能看到 QPS 的差别。
 *
 * 用法： LOG_INFO << "server start, port=" << port;
 *
 * 注意：这是同步日志，写盘会阻塞 IO 线程。异步日志是后面的事。
 */
// 六种日志等级
enum class LogLevel { TRACE, DEBUG, INFO, WARN, ERROR, FATAL };

class Logger {
public:
    Logger(const char* file, int line, LogLevel level);
    ~Logger();      /* 析构时才真正输出，保证一条日志原子写出 */

    std::ostringstream& stream() { return stream_; }

    static void setLevel(LogLevel level);                   // 设置日志等级
    static LogLevel getLevel();                             // 获取当前全局日志等级
    static const char* levelName(LogLevel level);           // 返回level的等级名字

private:
    const char* file_;                                      // 保存的是产生日志的源代码文件名
    int line_;
    LogLevel level_;
    std::ostringstream stream_;                             // Logger析构时，返回的日志输出流
};

/*
 * 先判级别再构造对象：级别不够时连字符串拼接都省了。
 * 这种写法有经典的悬垂 else 问题，所以别把它塞进 if/else 的简写里。
 * __FILE__: 当前源文件的文件名
 * __LINE__: 当前代码所在的行号
 */
#define LOG_TRACE \
    if (Logger::getLevel() <= LogLevel::TRACE) Logger(__FILE__, __LINE__, LogLevel::TRACE).stream()
#define LOG_DEBUG \
    if (Logger::getLevel() <= LogLevel::DEBUG) Logger(__FILE__, __LINE__, LogLevel::DEBUG).stream()
#define LOG_INFO \
    if (Logger::getLevel() <= LogLevel::INFO) Logger(__FILE__, __LINE__, LogLevel::INFO).stream()
#define LOG_WARN \
    if (Logger::getLevel() <= LogLevel::WARN) Logger(__FILE__, __LINE__, LogLevel::WARN).stream()
#define LOG_ERROR \
    if (Logger::getLevel() <= LogLevel::ERROR) Logger(__FILE__, __LINE__, LogLevel::ERROR).stream()
#define LOG_FATAL \
    if (Logger::getLevel() <= LogLevel::FATAL) Logger(__FILE__, __LINE__, LogLevel::FATAL).stream()

#endif
