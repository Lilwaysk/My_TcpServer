#include "net/Acceptor.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "base/Logger.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr):
    loop_(loop),
    acceptSocket_(createNonblockingOrDie()),
    acceptChannel_(loop, acceptSocket_.fd()),
    listening_(false),
    idleFd_(::open("/dev/null", O_RDONLY | O_CLOEXEC))
{
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.bindAddress(listenAddr);
}

Acceptor::~Acceptor()
{
    // 先从epoll里摘掉，避免fd关了还有人监听
    acceptChannel_.disableAll();
    acceptChannel_.remove();
    ::close(idleFd_);
    // acceptSocket_析构时自动close监听fd
}

void Acceptor::listen()
{
    loop_->assertInLoopThread();            // 必须在loop线程里面做
    acceptSocket_.listen();
    acceptChannel_.setReadCallback([this](){ handleRead(); });
    acceptChannel_.enableReading();
    listening_ = true;
}

void Acceptor::handleRead()
{
    InetAddress peerAddr;
    for (;;) {
        int connfd = acceptSocket_.accept(&peerAddr);
        if (connfd >= 0)
            if (newConnectionCallback_)
                newConnectionCallback_(connfd, peerAddr);
            else
                ::close(connfd);
        else {
            int savedErrno = errno;
            if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) break;
            if (savedErrno == EINTR) continue;
            if (savedErrno == EMFILE) {
                // fd 耗尽，腾一个位置accept掉再关掉，通知对端“现在忙”
                ::close(idleFd_);
                idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                continue;
            }
            LOG_ERROR << "acept failed: " << ::strerror(savedErrno);
            break;
        }
    }
}

/*
 * ============ Acceptor 实现清单（README 第四节「第 5 步」）============
 *
 * 1) 构造
 *      acceptSocket_(createNonblockingOrDie()),
 *      acceptChannel_(loop, acceptSocket_.fd()),
 *      listening_(false),
 *      idleFd_(::open("/dev/null", O_RDONLY | O_CLOEXEC))
 *    idleFd_ 先打开备着，后面处理 EMFILE 要用（见第 5 点）。
 *
 * 2) 析构
 *      acceptChannel_.disableAll();
 *      acceptChannel_.remove();
 *      ::close(idleFd_);
 *    acceptSocket_ 的关闭由 Socket 析构自动完成。
 *
 * 3) listen()
 *      acceptSocket_.setReuseAddr(true);
 *      acceptSocket_.setReusePort(...);   // 看 TcpServer 传进来的 Option
 *      acceptSocket_.bindAddress(listenAddr);
 *      acceptSocket_.listen();
 *      acceptChannel_.setReadCallback([this]{ handleRead(); });
 *      acceptChannel_.enableReading();
 *      listening_ = true;
 *
 *    顺序不能变：先 bind + listen 再注册可读。反过来的话会出现
 *    「还没 listen 就有连接进来」这种没法复现的诡异状态。
 *
 * 4) handleRead() —— 对应你老代码的 acceptconn，但结构要拆开：
 *
 *      InetAddress peerAddr;
 *      for (;;) {
 *          int connfd = acceptSocket_.accept(&peerAddr);
 *          if (connfd >= 0) {
 *              if (newConnectionCallback_)
 *                  newConnectionCallback_(connfd, peerAddr);
 *              else
 *                  ::close(connfd);        // 没人接就自己关掉，别泄漏
 *          } else {
 *              if (errno == EAGAIN || errno == EWOULDBLOCK)
 *                  break;                  // 队列里的连接都 accept 完了
 *              if (errno == EINTR)
 *                  continue;               // 被信号打断，重试
 *              LOG_ERROR << "accept failed: " << std::strerror(errno);
 *              break;
 *          }
 *      }
 *
 *    「循环 accept 到 EAGAIN」这一层是老代码里最值得保留的东西：
 *    水平触发（LT）下一次可读事件可能对应多个待接受的连接，
 *    只 accept 一次会让剩下的连接一直等到下次事件才被处理。
 *
 * 5) EMFILE 的处理 —— idleFd_ 的用法，建议主线跑通后再补
 *      ::close(idleFd_);                    // 腾出一个 fd 名额
 *      idleFd_ = ::accept(acceptSocket_.fd(), nullptr, nullptr);
 *      ::close(idleFd_);                    // 立刻关掉，等于通知对端「现在忙」
 *      idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);   // 重新备一个
 *
 *    不做这件事的后果：fd 一耗尽就会进入
 *    「accept 失败 → 连接还留在队列里 → 继续触发可读 → 又失败」的死循环，
 *    进程 CPU 直接跑满，而且日志刷屏。
 */

// Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr) { /* ... */ }
// Acceptor::~Acceptor() { /* ... */ }
// void Acceptor::listen() { /* ... */ }
// void Acceptor::handleRead() { /* ... */ }
