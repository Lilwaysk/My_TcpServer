#ifndef NET_ACCEPTOR_H
#define NET_ACCEPTOR_H

#include <functional>

#include "base/noncopyable.h"
#include "net/Channel.h"
#include "net/Socket.h"

class EventLoop;
class InetAddress;

/*
 * 只管 accept，对应你老代码里的 acceptconn。
 *
 * 区别在于：老代码在一个大循环里既 accept 又顺手初始化连接的一切；
 * 这里 Acceptor 只负责「有连接来了」这一件事，剩下的交给 TcpServer 决定
 * 把新连接分给哪个 IO 线程。
 */
class Acceptor : noncopyable {
public:
    // 处理新连接的回调函数
    typedef std::function<void(int sockfd, const InetAddress&)> NewConnectionCallback;

    Acceptor(EventLoop* loop, const InetAddress& listenAddr);
    ~Acceptor();

    void setNewConnectionCallback(const NewConnectionCallback& cb)
    {
        newConnectionCallback_ = cb;
    }

    bool listening() const { return listening_; }
    void listen();

private:
    void handleRead();

    EventLoop* loop_;
    Socket acceptSocket_;
    Channel acceptChannel_;
    NewConnectionCallback newConnectionCallback_;
    bool listening_;

    /*
     * 预留一个 fd（/dev/null），用来对付 fd 耗尽的情况：
     * accept 返回 EMFILE 时先 close 掉它，腾出一个位置 accept 新连接，
     * 立刻再 close 掉（相当于通知对端「我现在忙」），然后重新打开 /dev/null。
     * 不这么做的话，连接一多就会进入 accept 失败 -> 继续触发可读事件 -> 死循环。
     *
     * 这是第 5 步之后再补的细节，先把主线跑通。
     */
    int idleFd_;
};

#endif
