#include "net/Socket.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "base/Logger.h"
#include "net/InetAddress.h"

int createNonblockingOrDie()
{
    int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sockfd < 0)
    {
        int savedErrno = errno;
        LOG_FATAL << "socket() failed: " << ::strerror(savedErrno)
                  << " (errno = " << savedErrno << ")";
        ::abort();
    }
    return sockfd;
}

Socket::~Socket()
{
    ::close(sockfd_);
}

void Socket::bindAddress(const InetAddress& localaddr)
{   // reinterpret_cast<>强制类型转换
    if (::bind(sockfd_, reinterpret_cast<const struct sockaddr*>(&localaddr.getSockAddrInet()), sizeof(struct sockaddr_in)) < 0) {
        int savedErrno = errno;
        LOG_FATAL << "bind() failed on " << localaddr.toIpPort() << ": " << ::strerror(savedErrno) << "(errno = " << savedErrno << ")";
        ::abort();
    }
}

void Socket::listen()
{
    if (::listen(sockfd_, SOMAXCONN) < 0) {
        int savedErrno = errno;
        LOG_FATAL << "listen() failed: " << ::strerror(savedErrno)
                  << " (errno=" << savedErrno << ")";
        ::abort();
    }
}

int Socket::accept(InetAddress* peeraddr)
{
    struct sockaddr_in addr;
    ::memset(&addr, 0, sizeof(addr));
    socklen_t len = sizeof(addr);

    // accept4多了第四个参数可以直接设置非阻塞和关闭旧fd，老版本accept要分步骤设置
    int connfd = ::accept4(sockfd_, reinterpret_cast<struct sockaddr*>(&addr), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connfd >= 0) peeraddr->setSockAddrInet(addr);           //  把对端地址写回调用方

    return connfd;
}

void Socket::shutdownWrite()
{
    // SHUT_WR: 只关闭写不关闭读
    if (::shutdown(sockfd_, SHUT_WR) < 0) {
        int savedErrno = errno;
        LOG_ERROR << "shutdown(SHUT_WR) failed: " << ::strerror(savedErrno) << " (errno= " << savedErrno << ")";
    }
}

void Socket::setTcpNoDelay(bool on)
{
    int optval = on ? 1 : 0;
    if (::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) < 0) {
        int savedErrno = errno;;
        LOG_ERROR << "setsockopt(TCP_NODELAY) failed: " << ::strerror(savedErrno)
                  << " (errno=" << savedErrno << ")";
    }
}

void Socket::setReuseAddr(bool on)
{
    int optval = on ? 1 : 0;
    if (::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval) < 0)
    {
        int savedErrno = errno;
        LOG_ERROR << "setsockopt(SO_REUSEADDR) failed: " << ::strerror(savedErrno);
    }
}

void Socket::setReusePort(bool on)
{
    int optval = on ? 1 : 0;
    if (::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof optval) < 0)
    {
        int savedErrno = errno;
        LOG_ERROR << "setsockopt(SO_REUSEPORT) failed: " << ::strerror(savedErrno);
    }
}

void Socket::setKeepAlive(bool on)
{
    int optval = on ? 1 : 0;
    if (::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof optval) < 0)
    {
        int savedErrno = errno;
        LOG_ERROR << "setsockopt(SO_KEEPALIVE) failed: " << ::strerror(savedErrno);
    }
}

/*
 * ============ Socket 实现清单（README 第四节「第 5 步」）============
 *
 * 1) createNonblockingOrDie()
 *      int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
 *      失败 LOG_FATAL + abort。
 *      SOCK_NONBLOCK：建出来就是非阻塞，省掉一次 fcntl 来回。
 *      SOCK_CLOEXEC：防止 fork + exec 时把这个 fd 泄漏给子进程。
 *
 *    非阻塞必须从第一步就做对。老代码是 accept 之后才 fcntl 设非阻塞，
 *    中间那一小段窗口里 read 可能阻塞住整个线程。
 *
 * 2) 析构 —— ::close(sockfd_)，一行。这就是 RAII 的全部价值。
 *
 * 3) bindAddress(addr) —— 失败的 LOG_FATAL 里要带上 InetAddress::toIpPort()，
 *    否则只剩一个 errno，看不出是哪个地址绑失败了。
 *
 * 4) listen() —— ::listen(sockfd_, SOMAXCONN)
 *
 * 5) accept(peeraddr)
 *      struct sockaddr_in addr;
 *      socklen_t len = sizeof addr;
 *      int connfd = ::accept4(sockfd_, (struct sockaddr*)&addr, &len,
 *                             SOCK_NONBLOCK | SOCK_CLOEXEC);
 *      if (connfd >= 0) peeraddr->setSockAddrInet(addr);
 *      return connfd;
 *    用 accept4 而不是 accept：一次系统调用就把非阻塞和 CLOEXEC 都设好，
 *    省掉两次 fcntl，也避免中间窗口。
 *
 *    EAGAIN / EMFILE 等错误不在这一层处理 —— Socket 不知道当前上下文，
 *    交给调用方（Acceptor）决定怎么应对。
 *
 * 6) shutdownWrite() —— ::shutdown(sockfd_, SHUT_WR)，只关写端。
 *    客户端先关写端、等服务端把剩余数据发完再关，是 TCP 的礼貌做法。
 *
 * 7) 四个 setsockopt 小工具：
 *      setTcpNoDelay —— TCP_NODELAY。echo / IM 这种小包场景一定要开，
 *                       否则 Nagle 算法会攒包，延迟肉眼可见地变大。
 *      setReuseAddr  —— SO_REUSEADDR。重启服务能立刻重绑，不用等 TIME_WAIT。
 *      setReusePort  —— SO_REUSEPORT。多进程同时 listen 同一端口。
 *      setKeepAlive  —— SO_KEEPALIVE。注意 TCP 自带 keepalive 默认要 2 小时
 *                       才开始探测，做 IM 心跳根本不够，真正的保活得在应用层做。
 *
 * 8) 每个 setsockopt 都要检查返回值并 LOG_ERROR。
 *    「参数静默没生效」是隐形杀手，出问题时会查很久。
 */

// int createNonblockingOrDie() { /* ... */ }
// Socket::~Socket() { /* ... */ }
// void Socket::bindAddress(const InetAddress& localaddr) { /* ... */ }
// void Socket::listen() { /* ... */ }
// int Socket::accept(InetAddress* peeraddr) { /* ... */ }
// void Socket::shutdownWrite() { /* ... */ }
// void Socket::setTcpNoDelay(bool on) { /* ... */ }
// void Socket::setReuseAddr(bool on) { /* ... */ }
// void Socket::setReusePort(bool on) { /* ... */ }
// void Socket::setKeepAlive(bool on) { /* ... */ }
