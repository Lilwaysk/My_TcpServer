//
// ============ protocol_echo_server：M1 的服务端验收程序 ============
//
// 和 echo_server.cpp 只差一件事：它不把收到的字节原样扔回去，
// 而是先让 Codec 按 10 字节头拆帧，拆出来再回一帧。
//
// 所以它证明的是「服务端会拆包」—— 单测只能证明 Codec 自己自洽，
// 只有走真实 socket 才能证明「内核把一帧切成 5 段送来，服务端还能拼回来」。
//
// 用法：./protocol_echo_server [port] [io_threads]
//
//   io_threads = 0  单 Reactor（全部 I/O 在 main loop）
//   io_threads > 0  主从 Reactor（1 个 accept 线程 + N 个 IO 线程）
//
// 命令行验收（整帧 / 逐字节两种发法，后者才真正压到拆包逻辑）：
//   整帧：printf '\xEC\x03\x02\x00\x00\x00\x01\x00\x00\x00{}' | nc 127.0.0.1 8888 | xxd
//   逐字节：见 tests/test_codec.cpp 顶部注释里的 python3 一行命令
//
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

#include "base/Logger.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/TcpConnection.h"
#include "net/TcpServer.h"
#include "protocol/Codec.h"
#include "protocol/Protocol.h"

using namespace std::placeholders;

class ProtocolEchoServer {
public:
    ProtocolEchoServer(EventLoop* loop, const InetAddress& listenAddr)
        : server_(loop, listenAddr, "ProtocolEchoServer", TcpServer::kReusePort)
    {
        server_.setConnectionCallback(
            std::bind(&ProtocolEchoServer::onConnection, this, _1));
        server_.setMessageCallback(
            std::bind(&ProtocolEchoServer::onMessage, this, _1, _2, _3));
    }

    void setThreadNum(int n) { server_.setThreadNum(n); }
    void start() { server_.start(); }

private:
    void onConnection(const TcpConnectionPtr& conn)
    {
        LOG_INFO << conn->peerAddress().toIpPort() << " -> "
                 << conn->localAddress().toIpPort() << " is "
                 << (conn->connected() ? "UP" : "DOWN");
    }

    void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp)
    {
        bool fatal = false;
        const std::vector<proto::Frame> frames = proto::Codec::tryDecode(buf, &fatal);

        for (size_t i = 0; i < frames.size(); ++i) {
            const proto::Frame& f = frames[i];

            // R9：每条消息收发都打一行带 msgid / seq 的日志，先能查再谈性能
            LOG_INFO << "recv frame from " << conn->peerAddress().toIpPort()
                     << " msgid=" << f.msgId
                     << " seq=" << f.seq
                     << " bodyLen=" << f.body.size();

            // 原样回一帧：msgid / seq / 正文都不变，方便命令行直接比对
            conn->send(proto::Codec::encode(f.msgId, f.seq, f.body));
        }

        if (fatal) {
            // 帧头不可信 = 这个流永远对不齐了，只能断开，等客户端重连
            LOG_ERROR << "bad frame header from " << conn->peerAddress().toIpPort()
                      << ", closing connection";
            conn->forceClose();
        }
    }

    TcpServer server_;
};

int main(int argc, char* argv[])
{
    LOG_INFO << "pid = " << ::getpid();

    EventLoop loop;
    InetAddress listenAddr(static_cast<uint16_t>(argc > 1 ? atoi(argv[1]) : 8888));
    ProtocolEchoServer server(&loop, listenAddr);

    server.setThreadNum(argc > 2 ? atoi(argv[2]) : 0);

    server.start();
    loop.loop();
}
