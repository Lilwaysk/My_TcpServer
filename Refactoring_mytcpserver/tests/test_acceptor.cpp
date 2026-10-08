#include "net/Acceptor.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

// Acceptor 的测试（README「第 5 步」的验收）。
//
// Acceptor 没有「查端口」的公开接口，所以这里先用一个临时 socket 绑到
// 端口 0 向内核要一个空闲端口，关掉它，再让 Acceptor 绑同一个端口。
// 因为 Acceptor 开了 SO_REUSEADDR，这样是安全的。
//
// 重点验收两条：
//   1. 真的 accept 到新连接，并且对端地址被正确传进回调；
//   2. 「循环 accept 到 EAGAIN」——一次可读事件要把队列里的多个连接
//      全部 accept 出来，而不是只 accept 一个。

static int g_checks = 0;
static int g_failed = 0;

#define EXPECT_TRUE(expr)                                                   \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(expr)) {                                                      \
            ++g_failed;                                                     \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);        \
        }                                                                   \
    } while (0)

static uint16_t findFreePort()
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    struct sockaddr_in addr;
    ::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len);
    uint16_t port = ::ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

static int connectTo(uint16_t port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    ::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = ::htons(port);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static void testAcceptsNewConnection()
{
    printf("testAcceptsNewConnection\n");

    uint16_t port = findFreePort();
    EXPECT_TRUE(port != 0);

    EventLoop loop;
    Acceptor acceptor(&loop, InetAddress(port, true));
    int accepted = 0;
    std::string peerIp;
    uint16_t peerPort = 0;
    std::vector<int> connfds;

    acceptor.setNewConnectionCallback([&](int fd, const InetAddress& peer) {
        ++accepted;
        connfds.push_back(fd);
        peerIp = peer.toIp();
        peerPort = peer.port();
        if (accepted == 3) loop.quit();
    });
    acceptor.listen();
    EXPECT_TRUE(acceptor.listening());

    // 连 3 个客户端，然后才跑 loop：一次可读事件应该把 3 个全 accept 出来
    int c1 = connectTo(port);
    int c2 = connectTo(port);
    int c3 = connectTo(port);
    EXPECT_TRUE(c1 > 0 && c2 > 0 && c3 > 0);

    loop.runAfter(2.0, [&]() { loop.quit(); });   // 安全阀
    loop.loop();

    EXPECT_TRUE(accepted == 3);                   // 循环 accept 到 EAGAIN
    EXPECT_TRUE(peerIp == "127.0.0.1");
    EXPECT_TRUE(peerPort > 0);

    for (int fd : connfds) ::close(fd);
    ::close(c1);
    ::close(c2);
    ::close(c3);
}

static void testNoConnectionNoCallback()
{
    printf("testNoConnectionNoCallback\n");

    uint16_t port = findFreePort();
    EXPECT_TRUE(port != 0);

    EventLoop loop;
    Acceptor acceptor(&loop, InetAddress(port, true));
    int accepted = 0;

    acceptor.setNewConnectionCallback([&](int fd, const InetAddress&) {
        ++accepted;
        ::close(fd);              // 没人接就自己关，别泄漏
    });
    acceptor.listen();

    loop.runAfter(0.05, [&]() { loop.quit(); });
    loop.loop();

    EXPECT_TRUE(accepted == 0);   // 没有连接就不该有回调
}

int main()
{
    testAcceptsNewConnection();
    testNoConnectionNoCallback();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
