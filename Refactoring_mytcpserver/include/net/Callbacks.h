#ifndef NET_CALLBACKS_H
#define NET_CALLBACKS_H

#include <functional>
#include <memory>

#include "base/Timestamp.h"

/*
 * 回调类型别名集中放这里，解决一个实际问题：
 * TcpConnection.h 需要 MessageCallback，而 MessageCallback 又需要 Buffer 和 Timestamp。
 * 如果每个头文件都去 include 对方，很快就会绕成环。
 *
 * 办法是：这里只做「前向声明 + 类型别名」，谁需要谁 include 本文件。
 */

class Buffer;
class TcpConnection;
class EventLoop;

/* 一条 TcpConnection 的生命周期由 shared_ptr 管理，下面一堆回调都靠它传递 */
typedef std::shared_ptr<TcpConnection> TcpConnectionPtr;

/* 定时器到期时执行 */
typedef std::function<void()> TimerCallback;

/* 连接建立 / 断开 */
typedef std::function<void(const TcpConnectionPtr&)> ConnectionCallback;

/* 连接关闭（此时连接已经从 TcpServer 的连接表里摘掉）*/
typedef std::function<void(const TcpConnectionPtr&)> CloseCallback;

/* 消息到达。Buffer 里可能是半个包，也可能是三个包，由上层协议决定怎么切 */
typedef std::function<void(const TcpConnectionPtr&, Buffer*, Timestamp)> MessageCallback;

/* 内核发送缓冲区里的数据全部发完（包括之前没发完的） */
typedef std::function<void(const TcpConnectionPtr&)> WriteCompleteCallback;

/* 应用层发送缓冲区堆积超过阈值 */
typedef std::function<void(const TcpConnectionPtr&, size_t)> HighWaterMarkCallback;

/* I/O 线程启动时执行一次 */
typedef std::function<void(EventLoop*)> ThreadInitCallback;

#endif
