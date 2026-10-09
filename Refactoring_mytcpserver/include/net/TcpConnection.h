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
    // 构造一条 TcpConnection。
    // socket 的所有权转交给 TcpConnection。
    TcpConnection(EventLoop* loop,
                  const std::string& name,
                  int sockfd,
                  const InetAddress& localAddr,
                  const InetAddress& peerAddr);
    ~TcpConnection();

    // 返回该连接所属的 EventLoop。
    EventLoop* getLoop() const { return loop_; }
    // 返回连接名。
    const std::string& name() const { return name_; }
    // 返回本地地址。
    const InetAddress& localAddress() const { return localAddr_; }
    // 返回对端地址。
    const InetAddress& peerAddress() const { return peerAddr_; }
    // 返回连接是否已建立。
    bool connected() const { return state_ == kConnected; }

    // 线程安全：内部会转到 loop 线程真正执行发送。
    void send(const std::string& message);
    void send(const void* data, size_t len);

    // 只关闭写端，仍可继续读取。
    void shutdown();

    // 立刻关闭连接，不等待缓冲区中的数据发送完。
    void forceClose();

    // 设置连接建立时的回调。
    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    // 设置消息到达时的回调。
    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    // 设置写完成时的回调。
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }
    // 设置连接关闭时的回调。
    void setCloseCallback(const CloseCallback& cb) { closeCallback_ = cb; }
    // 设置高水位回调和阈值。
    void setHighWaterMarkCallback(const HighWaterMarkCallback& cb, size_t highWaterMark);

    // 由 TcpServer 在连接建立时调用。
    void connectEstablished();
    // 由 TcpServer 在连接销毁时调用。
    void connectDestroyed();

private:
    // 连接的状态。
    enum StateE { kDisconnected, kConnecting, kConnected, kDisconnecting };

    // 设置连接状态。
    void setState(StateE s) { state_ = s; }

    // Channel 的四类事件回调入口。
    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();

    // 线程安全的发送/关闭操作最终都转到 loop 线程里执行。
    void sendInLoop(const std::string& message);
    void sendInLoop(const void* data, size_t len);
    void shutdownInLoop();
    void forceCloseInLoop();

    // 该连接所属的事件循环。
    EventLoop* loop_;
    // 连接名称，常见形式如 "server#1"。
    const std::string name_;
    // 连接状态。
    StateE state_;
    // 封装 socket 的 RAII 对象。
    std::unique_ptr<Socket> socket_;
    // 用于监听 socket 事件的 Channel。
    std::unique_ptr<Channel> channel_;
    // 本地地址和对端地址。
    const InetAddress localAddr_;
    const InetAddress peerAddr_;

    // 用户注册的回调。
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;
    CloseCallback closeCallback_;
    // 输出缓冲区高水位线。
    size_t highWaterMark_;

    // 输入缓冲区：存放从 socket 读到的数据。
    // 输出缓冲区：存放尚未完全写入 socket 的数据。
    Buffer inputBuffer_;
    Buffer outputBuffer_;
};

#endif
