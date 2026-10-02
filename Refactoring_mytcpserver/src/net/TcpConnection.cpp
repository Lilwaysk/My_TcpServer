#include "net/TcpConnection.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

#include "base/Logger.h"
#include "net/Channel.h"
#include "net/EventLoop.h"
#include "net/Socket.h"

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

// TcpConnection::TcpConnection(EventLoop* loop, const std::string& name,
//                              int sockfd, const InetAddress& localAddr,
//                              const InetAddress& peerAddr) { /* ... */ }
// TcpConnection::~TcpConnection() { /* ... */ }
// void TcpConnection::connectEstablished() { /* ... */ }
// void TcpConnection::connectDestroyed() { /* ... */ }
// void TcpConnection::handleRead() { /* ... */ }
// void TcpConnection::handleWrite() { /* ... */ }
// void TcpConnection::handleClose() { /* ... */ }
// void TcpConnection::handleError() { /* ... */ }
// void TcpConnection::send(const std::string& message) { /* ... */ }
// void TcpConnection::send(const void* data, size_t len) { /* ... */ }
// void TcpConnection::sendInLoop(const std::string& message) { /* ... */ }
// void TcpConnection::sendInLoop(const void* data, size_t len) { /* ... */ }
// void TcpConnection::shutdown() { /* ... */ }
// void TcpConnection::shutdownInLoop() { /* ... */ }
// void TcpConnection::forceClose() { /* ... */ }
// void TcpConnection::forceCloseInLoop() { /* ... */ }
// void TcpConnection::setHighWaterMarkCallback(const HighWaterMarkCallback& cb, size_t hwm) { /* ... */ }
