#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#include "base/noncopyable.h"

class InetAddress;

/*
 * socket fd 的 RAII 封装：构造时接管一个 fd，析构时 close。
 *
 * 老代码里 close(fd) 散落在 conn_close、acceptconn 的出错分支、main 退出处，
 * 少写一个就是 fd 泄漏。交给析构函数之后，「关闭」和「离开作用域」变成同一件事。
 */
class Socket : noncopyable {
public:
    explicit Socket(int sockfd) : sockfd_(sockfd) {}
    ~Socket();

    int fd() const { return sockfd_; }

    void bindAddress(const InetAddress& localaddr);
    void listen();

    /*
     * accept 一个新连接，把对端地址写进 peeraddr。
     * 老代码里「循环 accept 到 EAGAIN」的那层循环归 Acceptor，这里只 accept 一次。
     */
    int accept(InetAddress* peeraddr);

    void shutdownWrite();

    void setTcpNoDelay(bool on);
    void setReuseAddr(bool on);
    void setReusePort(bool on);
    void setKeepAlive(bool on);

private:
    const int sockfd_;
};

/*
 * 建一个非阻塞 socket，失败直接 abort。
 * 启动阶段的错误早点炸掉，比返回 -1 再层层判断更好定位。
 */
int createNonblockingOrDie();

#endif
