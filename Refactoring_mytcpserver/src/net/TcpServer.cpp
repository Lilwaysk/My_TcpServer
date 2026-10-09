#include "net/TcpServer.h"

#include <cassert>
#include <cstdio>

#include "base/Logger.h"
#include "net/Acceptor.h"
#include "net/EventLoop.h"
#include "net/EventLoopThreadPool.h"
#include "net/TcpConnection.h"

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listenAddr,
                     const std::string& nameArg, Option option)
                    :loop_(loop),
                     ipPort_(listenAddr.toIpPort()),
                     name_(nameArg),
                     started_(false),
                     nextConnId_(1)
{
    assert(loop_ != nullptr);

    acceptor_.reset(new Acceptor(loop_, listenAddr, option == kReusePort));
    acceptor_->setNewConnectionCallback([this](int sockfd, const InetAddress& peerAddr){
        newConnection(sockfd, peerAddr)
    });

    threadPool_->reset(new EventLoopThreadPool(loop_, name_));
}

TcpServer::~TcpServer()
{

}

void TcpServer::setThreadNum(int numThreads)
{

}

void TcpServer::start()
{

}

void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr)
{

}

void TcpServer::removeConnection(const TcpConnectionPtr& conn)
{

}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn)
{

}

/*
 * ============ TcpServer 实现清单（README 第四节「第 6 步」）============
 *
 * 构造：acceptor_.reset(new Acceptor(loop, listenAddr));
 *       acceptor_->setNewConnectionCallback(
 *           std::bind(&TcpServer::newConnection, this, _1, _2));
 *       threadPool_.reset(new EventLoopThreadPool(loop, name_));
 *
 * setThreadNum：转发给 threadPool_->setThreadNum(n)。
 *
 * start()：threadPool_->start(threadInitCallback_) → acceptor_->listen()。
 *       顺序不能反。必须先把 IO 线程都起好，否则第一个连接可能落到
 *       一个还没跑起来的 loop 上。用 started_ 断言防止重复 start。
 *
 * newConnection(sockfd, peerAddr) —— 跑在 baseLoop 线程里：
 *       1. EventLoop* ioLoop = threadPool_->getNextLoop();
 *       2. connName = name_ + "#" + std::to_string(nextConnId_++)，
 *          同时日志记下 peerAddr.toIpPort()。
 *       3. conn.reset(new TcpConnection(ioLoop, connName, sockfd, localAddr, peerAddr));
 *       4. 把 connectionCallback_ / messageCallback_ / writeCompleteCallback_
 *          透传给它。
 *       5. conn->setCloseCallback(std::bind(&TcpServer::removeConnection, this, _1));
 *       6. connections_[connName] = conn;
 *       7. ioLoop->runInLoop(std::bind(&TcpConnection::connectEstablished, conn));
 *
 *       第 7 条是主从 Reactor 的核心：连接对象是在 baseLoop 里创建的，
 *       但 Channel 的注册和之后的所有事件必须发生在 ioLoop 线程里。
 *       掉这一步就会出现「注册在一个线程、事件在另一个线程」的竞态。
 *
 * removeConnection(conn) —— 可能在任何线程被调到：
 *       loop_->runInLoop(std::bind(&TcpServer::removeConnectionInLoop, this, conn));
 *       分两段的理由：connections_ 这个 map 只允许 baseLoop 线程碰，
 *       跨线程改 map 是要出事的。
 *
 * removeConnectionInLoop(conn)：
 *       connections_.erase(conn->name());
 *       EventLoop* ioLoop = conn->getLoop();
 *       ioLoop->queueInLoop(std::bind(&TcpConnection::connectDestroyed, conn));
 *
 *       这里用 queueInLoop 而不是 runInLoop 是刻意的：此刻很可能
 *       就站在这条连接的 IO 回调栈里，同步执行会把当前这层栈提前拆掉。
 *       queueInLoop 保证等当前回调返回之后再执行。
 *
 * 验收：examples/echo_server.cpp 跑起来，行为要和现在的 epoll 版本一致。
 */
