#ifndef NET_TCPCONNECTION_H
#define NET_TCPCONNECTION_H

#include <memory>
#include <string>

#include "base/noncopyable.h"
#include "net/Buffer.h"
#include "net/Callbacks.h"
#include "net/InetAddress.h"

class Channel;
class EventLoop;
class Socket;

/*
 * 一条 TCP 连接，等于你老代码里 recvdata + senddata + conn_close 三块合体，
 * 再加上它自己的全部状态。
 *
 * 用 shared_ptr 管理生命周期（TcpConnectionPtr 已在 Callbacks.h 里定义好）：
 * 一个连接可能同时被「服务器连接表」「正在执行的回调栈」持有，
 * 引用计数归零才真正析构 —— 这样就不会出现「函数执行到一半 fd 被关了」。
 *
 * 继承 enable_shared_from_this 是为了在成员函数里拿到指向自己的 shared_ptr。
 * 典型场景是把「关闭连接」这个动作投递到 loop 线程时，
 * 得保证对象活到那时候。
 */
class TcpConnection : noncopyable,
                      public std::enable_shared_from_this<TcpConnection> {
public:
    TcpConnection(EventLoop* loop,
                  const std::string& name,
                  int sockfd,
                  const InetAddress& localAddr,
                  const InetAddress& peerAddr);
    ~TcpConnection();

    EventLoop* getLoop() const { return loop_; }
    const std::string& name() const { return name_; }
    const InetAddress& localAddress() const { return localAddr_; }
    const InetAddress& peerAddress() const { return peerAddr_; }
    bool connected() const { return state_ == kConnected; }

    /* 线程安全：内部会转到 loop 线程再真正发送 */
    void send(const std::string& message);
    void send(const void* data, size_t len);

    /* 只关写端，还能继续读（半关闭）*/
    void shutdown();

    /* 立刻关，不等待缓冲区发完 */
    void forceClose();

    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }
    void setCloseCallback(const CloseCallback& cb) { closeCallback_ = cb; }
    void setHighWaterMarkCallback(const HighWaterMarkCallback& cb, size_t highWaterMark);

    /* 只有 TcpServer 会调这两个 */
    void connectEstablished();
    void connectDestroyed();

private:
    enum StateE { kDisconnected, kConnecting, kConnected, kDisconnecting };

    void setState(StateE s) { state_ = s; }

    /* Channel 的四个回调落点 */
    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();

    void sendInLoop(const std::string& message);
    void sendInLoop(const void* data, size_t len);
    void shutdownInLoop();
    void forceCloseInLoop();

    EventLoop* loop_;
    const std::string name_;
    StateE state_;
    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;
    const InetAddress localAddr_;
    const InetAddress peerAddr_;

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    CloseCallback closeCallback_;
    size_t highWaterMark_;

    /*
     * 输入缓冲：readFd 往这里读，粘包的数据在这里攒着等上层切。
     * 输出缓冲：send 时内核写不下就留在这里，等 EPOLLOUT 再续。
     */
    Buffer inputBuffer_;
    Buffer outputBuffer_;
};

#endif
