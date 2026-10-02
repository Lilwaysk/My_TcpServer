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

    void setThreadInitCallback(const ThreadInitCallback& cb) { threadInitCallback_ = cb; }

    /* 开始监听 */
    void start();

    void setConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void setMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    void setWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }

private:
    void newConnection(int sockfd, const InetAddress& peerAddr);
    void removeConnection(const TcpConnectionPtr& conn);
    void removeConnectionInLoop(const TcpConnectionPtr& conn);

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
