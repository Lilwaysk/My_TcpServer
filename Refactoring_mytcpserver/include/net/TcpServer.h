#ifndef NET_TCPSERVER_H
#define NET_TCPSERVER_H

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "base/noncopyable.h"
#include "net/Callbacks.h"
#include "net/InetAddress.h"

class EventLoop;
class Acceptor;
class EventLoopThreadPool;

/*
 * 智能指针: unique_ptr shared__ptr weak_ptr
 *
 * unique_ptr: 独占所有权，一个对象只能由一个unique_ptr拥有，该对象不能被多个unique_ptr指向，unique_ptr不能被复制，但可以用move()转移所有权给另一个对象
 * shared_ptr: 共享所有权，多个shared_ptr可以指向同一个对象，它们共享引用计数，最后一个拥有者销毁时，对象才会被销毁
 *   weak_ptr: 观察，不拥有。weak_ptr 可以观察由 shared_ptr 管理的对象，但不增加拥有者计数
 *             因此不会让对象延长生命。使用前要尝试通过 lock() 获取一个临时的 shared_ptr，
 *             常用来打破 shared_ptr 的循环引用，或表示“对象可能已经不存在”。
 *
 *
 *
 * 服务器门面。用户（examples/ 里的代码）只碰这一个类，
 * 不直接接触 Acceptor / TcpConnection / Channel。
 *
 * 它替掉老代码里的 g_events[MAX_EVENTS+1] ——
 * 「手工找空槽位」换成 std::map<std::string, TcpConnectionPtr>，
 * 连接数不再有上限，也不用遍历数组找 NULL。
 */
class TcpServer : noncopyable {
public:
    typedef std::function<void(EventLoop*)> ThreadInitCallback;

    // 是否设置端口复用
    enum Option {
        kNoReusePort,
        kReusePort,
    };

    TcpServer(EventLoop* loop,
              const InetAddress& listenAddr,
              const std::string& nameArg,
              Option option = kNoReusePort);
    ~TcpServer();

    const std::string& name() const { return name_; }
    const std::string& ipPort() const { return ipPort_; }
    EventLoop* getLoop() const { return loop_; }

    /*
     * numThreads == 0：所有连接都在主线程处理（单 Reactor）
     * numThreads >  0：主线程只 accept，IO 分给线程池（主从 Reactor）
     */
    void setThreadNum(int numThreads);

    // 设置线程初始化的回调函数
    void setThreadInitCallback(const ThreadInitCallback& cb) { threadInitCallback_ = cb; }

    /* 开始监听 */
    void start();

    // 设置处理连接的回调
    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    // 设置处理消息的回调
    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    // 设置处理写完成时的回调
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }

private:
    // 新链接
    void newConnection(int sockfd, const InetAddress& peerAddr);
    // 断开连接
    void removeConnection(const TcpConnectionPtr& conn);
    // 从事件循环里移除连接
    void removeConnectionInLoop(const TcpConnectionPtr& conn);

    // 连接表：一个名字 对应 一个Tcp连接指针
    typedef std::map<std::string, TcpConnectionPtr> ConnectionMap;

    EventLoop* loop_;                   /* 主线程的 loop，只管 accept */
    const std::string ipPort_;
    const std::string name_;
    std::unique_ptr<Acceptor> acceptor_;
    std::shared_ptr<EventLoopThreadPool> threadPool_;

    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    ThreadInitCallback threadInitCallback_;

    bool started_;
    int nextConnId_;                    /* 用来拼连接名 conn1 / conn2 ... */
    ConnectionMap connections_;
};

#endif
