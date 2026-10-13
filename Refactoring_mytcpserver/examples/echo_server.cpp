/*
 * ============ echo_server：网络库的使用者（README 第四节「第 6 步」）============
 *
 * 这个文件是验收标准：用 TcpServer 复刻你现在那个 epoll echo 服务器，
 * 行为一致就算重构成功。
 *
 * 关键点在于 —— 整个 echo 逻辑只剩下面这几行回调，
 * 没有 epoll、没有 accept、没有 close、没有缓冲区管理。
 * 那些全都被库吃掉了，这正是重构的价值。
 *
 * 注意这个文件故意保持在 include/ 和 src/ 之外：
 * 依赖方向是 examples/ -> net/ -> base/，库本身不认识 echo_server。
 *
 * 用法：./echo_server [port] [io_threads]
 *   io_threads = 0  单 Reactor（全部 I/O 在 main loop）
 *   io_threads > 0  主从 Reactor（1 个 accept 线程 + N 个 IO 线程）
 */
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "base/Logger.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/TcpConnection.h"
#include "net/TcpServer.h"

using namespace std::placeholders;

class EchoServer {
public:
    EchoServer(EventLoop* loop, const InetAddress& listenAddr)
        : server_(loop, listenAddr, "EchoServer", TcpServer::kReusePort)
    {
        server_.setConnectionCallback(
            std::bind(&EchoServer::onConnection, this, _1));
        server_.setMessageCallback(
            std::bind(&EchoServer::onMessage, this, _1, _2, _3));
    }

    void setThreadNum(int n) { server_.setThreadNum(n); }
    void start() { server_.start(); }

private:
    void onConnection(const TcpConnectionPtr& conn)
    {
        LOG_INFO << conn->peerAddress().toIpPort() << " -> "
                 << conn->localAddress().toIpPort() << " is "
                 << (conn->connected() ? "UP" : "DOWN");
    }

    void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp)
    {
        std::string msg = buf->retrieveAllAsString();
        conn->send(msg);        /* 原样发回去 */
    }

    TcpServer server_;
};

int main(int argc, char* argv[])
{
    LOG_INFO << "pid = " << ::getpid();

    EventLoop loop;
    InetAddress listenAddr(static_cast<uint16_t>(argc > 1 ? atoi(argv[1]) : 8888));
    EchoServer server(&loop, listenAddr);

    /* 第 7 步：验证主从 Reactor */
    server.setThreadNum(argc > 2 ? atoi(argv[2]) : 0);

    server.start();
    loop.loop();
}
