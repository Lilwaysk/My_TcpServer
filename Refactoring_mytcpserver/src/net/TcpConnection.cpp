#include "net/TcpConnection.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <unistd.h>

#include "base/Logger.h"
#include "net/Channel.h"
#include "net/EventLoop.h"
#include "net/Socket.h"

// 构造 TcpConnection，并绑定到指定 socket。
TcpConnection::TcpConnection(EventLoop* loop, const std::string& name,
                             int sockfd, const InetAddress& localAddr,
                             const InetAddress& peerAddr)
    : loop_(loop),
      name_(name),
      state_(kConnecting),
      socket_(new Socket(sockfd)),
      channel_(new Channel(loop, sockfd)),
      localAddr_(localAddr),
      peerAddr_(peerAddr),
      highWaterMark_(64 * 1024 * 1024)
{
    socket_->setTcpNoDelay(true);
    channel_->setReadCallback([this]() { handleRead(); });
    channel_->setWriteCallback([this]() { handleWrite(); });
    channel_->setCloseCallback([this]() { handleClose(); });
    channel_->setErrorCallback([this]() { handleError(); });
}

// 析构函数：取消所有事件监听，并从 poller 中移除 channel。
TcpConnection::~TcpConnection()
{
    if (channel_) {
        channel_->disableAll();
        channel_->remove();
    }
}

// 当连接建立后，开始监听 socket 的读事件。
void TcpConnection::connectEstablished()
{
    loop_->assertInLoopThread();
    assert(state_ == kConnecting);
    setState(kConnected);
    channel_->tie(shared_from_this());
    channel_->enableReading();

    if (connectionCallback_) {
        connectionCallback_(shared_from_this());
    }
}

// 当连接即将销毁时，统一做状态和 Channel 清理。
void TcpConnection::connectDestroyed()
{
    loop_->assertInLoopThread();
    if (state_ != kDisconnected) {
        setState(kDisconnected);
    }
    channel_->disableAll();
    channel_->remove();
}

// 读事件回调：尽量读出所有数据，并交给消息回调处理。
void TcpConnection::handleRead()
{
    loop_->assertInLoopThread();

    if (state_ == kDisconnected) return;                                            // 还没连接上的直接返回

    int savedErrno = 0;
    ssize_t n = inputBuffer_.readfd(channel_->fd(), &savedErrno);                   // 统计读到的总字节数

    if (n > 0) {
        if (messageCallback_)                                                       // 如果注册了回调函数
            messageCallback_(shared_from_this(), &inputBuffer_, Timestamp::now());  // 调用消息处理回调函数
        return;
    }

    if (n == 0) {                                                                   // n == 0表示对端正常关闭了连接
        handleClose();                                                              // 触发连接关闭流程
        return;
    }

    errno = savedErrno;
    if (savedErrno != EAGAIN && savedErrno != EWOULDBLOCK) handleError();           // 过滤非致命错误，EAGAIN和EWOULDBLOCK算阻塞状态不能算错误
}

void TcpConnection::handleWrite()
{
    loop_->assertInLoopThread();

    if (state_ == kDisconnected) return ;

    if (outputBuffer_.readableBytes() == 0) {
        channel_->disableWriting();

        if (state_ == kDisconnecting) shutdownInLoop();

        return ;
    }

    ssize_t n = ::write(channel_->fd(), outputBuffer_.peek(), outputBuffer_.readableBytes());

    if (n > 0) {
        outputBuffer_.retrieve(static_cast<size_t>(n));

        if (outputBuffer_.readableBytes() == 0) {
            channel_->disableWriting();

            if (writeCompleteCallback_) writeCompleteCallback_(shared_from_this());
            if (state == kDisconnecting) shutdownInLoop();
            return ;
        }
    } else if (n < 0 && errnno != EAGAIN && errno != EWOULDBLOCK) {
        handleError();
        return ;
    }

    if (outputBuffer_.readableBytes() > 0) channel_->enableWriting();
}

void TcpConnection::handleClose()
{
    loop_->assertInLoopThread();

    if (state_ == kDisconnected) return;

    setState(kDisconnected);
    channel_->disableAll();

    if (connectionCallback_) connectionCallback_(shared_from_this());
    if (closeCallback_) closeCallback_(shared_from_this());
}

void TcpConnection::handleError()
{
    loop_->assertInLoopThread();

    int savedErrno = errno;
    LOG_ERROR << "TcpConnection::handleError() - fd = " << channel_->fd()
              << ", errno = " << savedErrno << ": " << ::strerror(savedErrno);

    handleClose();
}

void TcpConnection::send(const std::string& message)
{
    if (state_ == kDisconnected) return ;

    if (loop_->isInLoopThread()) sendInLoop(message);
    else loop_->runInLoop([this, message](){ sendInLoop(message); });
}

void TcpConnection::send(const void* data, size_t len)
{
    if (data == nullptr || len == 0) return ;
    send(std::string(static_cast<const char*>(data), len));
}

void TcpConnection::sendInLoop(const std::string& message)
{
    loop_->assertInLoopThread();

    if (state_ == kDisconnected || state_ == kDisconnecting) return ;                       // 确保已连上

    if (message.empty()) return ;                                                           // 确保消息不为空
    // 当前没有未发完的数据，而且写事件也没开着，所以可以直接走一次 write()，尽量把这次消息一次性发出去
    if (outputBuffer_.readableBytes() == 0 && !channel_->isWriting()) {
        ssize_t nw = ::write(channel_->fd(), message().data(), message.size());

        if (nw >= 0) {
            size_t written = static_cast<size_t>(nw);
            if (written < message.size()) {                                                 // 没写完
                outputBuffer_.append(message.data() + written, message.size());             // 追加没写完的数据
                channel_->enableWriting();                                                  // 设置可写
            } else if (writeCompleteCallback_)                                              // 写回调
                writeCompleteCallback_(shared_from_this());

            /*
             * 当 outputBuffer_ 里积压的数据超过了阈值 highWaterMark_, 就触发一次 highWaterMarkCallback_
             * 告诉上层：当前连接的发送缓冲区已经比较满了，业务层可以考虑减速或者暂停发送
             */
            if (outputBuffer_.readableBytes() > highWaterMark_ && highWaterMarkCallback_)
                highWaterMarkCallback_(shared_from_this(), outputBuffer_.readableBytes());

            return ;
        }
        /*
         * 当前 write() 触发了非阻塞 socket 的正常暂时性阻塞，这说明“现在 socket 可写状态还没到，不能立刻继续写”=
         * 所以把这次消息放进 outputBuffer_，并打开 channel_->enableWriting()，等后面 POLLOUT 事件来时，再继续写
         */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            outputBuffer_.append(message.data(), message.size());
            channel_->enableWriting();
        } else {
            handleError();
            return ;
        }
    } else {
            // 处理未发完的数据
            outputBuffer_.append(message.data(), message.size());
            channel_->enableWriting();
    }

    if (outputBuffer_.readableBytes() > highWaterMark_ && highWaterMarkCallback_)
        highWaterMarkCallback_(shared_from_this(), outputBuffer_.readableBytes());
}

void TcpConnection::sendInLoop(const void* data, size_t len)
{
    if (data == nullptr || len == 0) return;
    sendInLoop(std::string(static_cast<const char*>(data)), len);
}

void TcpConnection::shutdown()
{
    if (loop_->isInLoopThread())
        shutdownInLoop();
    else
        loop_->runInLoop([this]{
            shutdownInLoop();
        });
}

void TcpConnection::shutdownInLoop()
{
    loop_->assertInLoopThread();

    if (state_ == kDisconnected || state_ == kConnecting) return ;

    if (state_ != kDisconnecting) setState(kDisconnecting);

    // 没有残余数据了和没有channel正在触发写事件
    if (outputBuffer_.readableBytes() == 0 && !channel_->isWriting()) {
        socket_->shutdownWrite();
        return ;
    }

    if (!channel_->isWriting() && outputBuffer_.readableBytes() > 0) channel_->enableWriting();
}

void TcpConnection::forceClose()
{
    if (loop_->isInLoopThread()) forceCloseInLoop();
    else loop_->runInLoop([this]{ forceCloseInLoop(); });
}

void TcpConnection::forceCloseInLoop()
{
    loop_->assertInLoopThread();

    if (state_ == kDisconnected) return;

    setState(kDisconnected);
    channel_->disableAll();

    if (connectionCallback_) connectionCallback_(shared_from_this());
    if (closeCallback_) closeCallback_(shared_from_this());
}

void TcpConnection::setHighWaterMarkCallback(const HighWaterMarkCallback& cb, size_t hwm)
{
    highWaterMarkCallback_ = cb;
    highWaterMark_ = hwm;
}

/*
 * ============ TcpConnection 实现清单（README 第四节「第 6 步」）============
 * 整个重构的分水岭。跑通之后你就有了自己的网络库，后面加协议、加线程池
 * 都只在 examples/ 层动手。
 *
 * 构造：socket_ 接管 sockfd，channel_ 绑同一个 fd，
 *       四个回调分别接到 handleRead / handleWrite / handleClose / handleError，
 *       socket_->setTcpNoDelay(true)，state_ = kConnecting。
 *
 * connectEstablished：setState(kConnected) → channel_->tie(shared_from_this())
 *       → enableReading() → connectionCallback_(shared_from_this())。
 *       tie() 是关键，它保证「回调里连接被析构」时对象能活到派发结束。
 *
 * handleRead：readFd 返回 >0 走 messageCallback_；==0 说明对端正常关闭，
 *       走 handleClose；<0 才是 handleError。这三种必须分开判，
 *       老代码里只判 <=0 会把 EAGAIN 也当成断开。
 *
 * handleWrite：发出多少 retrieve 多少；可读字节清零时必须 disableWriting()，
 *       否则水平触发下 EPOLLOUT 会让 CPU 空转。若 state_ 已经是
 *       kDisconnecting，说明之前的 shutdown 在等这里收尾。
 *
 * send / sendInLoop：send 只负责 runInLoop 投递。sendInLoop 里先在
 *       outputBuffer_ 为空时直接 ::write 试一次（省一次拷贝，echo 场景的主路径）；
 *       写不完的追加进 outputBuffer_ 并 enableWriting()；
 *       积压超过 highWaterMark_ 就调 highWaterMarkCallback_ 做背压。
 *       state_ 为 kDisconnecting / kDisconnected 时直接丢弃，不要写。
 *
 * shutdown / forceClose：两者都要 runInLoop 回到本线程再执行。
 *
 * handleClose / handleError：disableAll → setState(kDisconnected)
 *       → connectionCallback_ 通知业务层 → closeCallback_ 通知 TcpServer 摘表。
 *
 * connectDestroyed：只由 TcpServer 调。最后一定要 channel_->remove()，
 *       从 Poller 注销之后对象才能真正析构。
 *
 * 验收：用 TcpServer 重写 echo，行为不回退；用 netstat 看关掉客户端后
 *       fd 有没有被回收。
 */


