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

    // 返回sockfd
    int fd() const { return sockfd_; }

    void bindAddress(const InetAddress& localaddr);     // 传入本地地址，地址绑定
    void listen();                                      // 设置监听范围

    /*
     * accept 一个新连接，把对端地址写进 peeraddr。
     * 老代码里「循环 accept 到 EAGAIN」的那层循环归 Acceptor，这里只 accept 一次。
     */
    int accept(InetAddress* peeraddr);

    void shutdownWrite();                               // 结束写

    void setTcpNoDelay(bool on);                        // 关掉Nagle算法，不攒小包等包满再发，小包也立即发送
    void setReuseAddr(bool on);                         // 设置地址复用
    void setReusePort(bool on);                         // 设置端口复用
    void setKeepAlive(bool on);                         // 设置存活探测

private:
    const int sockfd_;
};

/*
 * 建一个非阻塞 socket，失败直接 abort。
 * 启动阶段的错误早点炸掉，比返回 -1 再层层判断更好定位。
 */
int createNonblockingOrDie();   // 创建一个tcpsockfd + 设置非阻塞和设置exec时把这个旧的fd的释放，创建失败就abort
// SOCK_CLOEXEC: 给这个 fd 打上 CLOEXEC,就是告诉内核:"本进程 exec 变成别的程序时,把这个 fd 关掉。"

#endif
