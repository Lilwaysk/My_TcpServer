#include "net/Socket.h"
#include "net/InetAddress.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

// Socket 的测试（README「第 5 步」的验收）。
//
// 这一层直接碰系统调用，所以每个用例都真开 fd、真 bind/listen/connect，
// 验证的不是「函数返回了」，而是「fd 的属性真的对了」：
// 非阻塞、CLOEXEC、对端地址、setsockopt 是否真的生效。

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

static bool isNonblocking(int fd)
{
    int fl = ::fcntl(fd, F_GETFL, 0);
    return fl != -1 && (fl & O_NONBLOCK);
}

static bool isCloexec(int fd)
{
    int fl = ::fcntl(fd, F_GETFD, 0);
    return fl != -1 && (fl & FD_CLOEXEC);
}

static uint16_t localPort(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    ::memset(&addr, 0, sizeof(addr));
    if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0)
        return 0;
    return ::ntohs(addr.sin_port);
}

static int getIntOption(int fd, int level, int optname)
{
    int val = -1;
    socklen_t len = sizeof(val);
    ::getsockopt(fd, level, optname, &val, &len);
    return val;
}

static void testCreateNonblockingOrDie()
{
    printf("testCreateNonblockingOrDie\n");

    int fd = createNonblockingOrDie();
    EXPECT_TRUE(fd >= 0);
    EXPECT_TRUE(isNonblocking(fd));     // 建出来就是非阻塞
    EXPECT_TRUE(isCloexec(fd));         // fork+exec 不会泄漏给子进程

    {
        Socket s(fd);
        EXPECT_TRUE(s.fd() == fd);
    }
    // Socket 析构后 fd 必须已被 close —— RAII 的核心价值
    EXPECT_TRUE(::fcntl(fd, F_GETFD, 0) == -1);
}

static void testBindListenAcceptAndPeerAddress()
{
    printf("testBindListenAcceptAndPeerAddress\n");

    int listenfd = createNonblockingOrDie();
    Socket listenSock(listenfd);
    listenSock.setReuseAddr(true);
    listenSock.bindAddress(InetAddress(0, true));   // 端口 0：内核挑，绑 127.0.0.1
    listenSock.listen();

    uint16_t port = localPort(listenfd);
    EXPECT_TRUE(port != 0);             // 内核确实分配了端口

    // 用一个阻塞客户端连上来，保证握手完成后 accept 一定能拿到
    int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    ::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    sa.sin_port = ::htons(port);
    EXPECT_TRUE(::connect(cfd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0);

    InetAddress peer;
    int connfd = listenSock.accept(&peer);

    EXPECT_TRUE(connfd >= 0);
    EXPECT_TRUE(isNonblocking(connfd));         // accept4 一次设好非阻塞
    EXPECT_TRUE(isCloexec(connfd));
    EXPECT_TRUE(peer.toIp() == "127.0.0.1");    // 对端地址被正确写回
    EXPECT_TRUE(peer.port() > 0);

    ::close(connfd);
    ::close(cfd);
}

static void testSocketOptionsTakeEffect()
{
    printf("testSocketOptionsTakeEffect\n");

    int fd = createNonblockingOrDie();
    Socket s(fd);

    s.setReuseAddr(true);
    EXPECT_TRUE(getIntOption(fd, SOL_SOCKET, SO_REUSEADDR) == 1);

    s.setReusePort(true);
    EXPECT_TRUE(getIntOption(fd, SOL_SOCKET, SO_REUSEPORT) == 1);

    s.setKeepAlive(true);
    EXPECT_TRUE(getIntOption(fd, SOL_SOCKET, SO_KEEPALIVE) == 1);

    s.setTcpNoDelay(true);
    EXPECT_TRUE(getIntOption(fd, IPPROTO_TCP, TCP_NODELAY) == 1);

    // 关掉也要真的关掉，不能只处理 on=true
    s.setReuseAddr(false);
    EXPECT_TRUE(getIntOption(fd, SOL_SOCKET, SO_REUSEADDR) == 0);
}

static void testShutdownWrite()
{
    printf("testShutdownWrite\n");

    int sv[2] = {-1, -1};
    EXPECT_TRUE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    {
        Socket s(sv[0]);
        s.shutdownWrite();

        char buf = 0;
        ssize_t n = ::read(sv[1], &buf, 1);
        EXPECT_TRUE(n == 0);        // 对端收到 EOF，但连接没整体关闭
    }

    ::close(sv[1]);
}

int main()
{
    testCreateNonblockingOrDie();
    testBindListenAcceptAndPeerAddress();
    testSocketOptionsTakeEffect();
    testShutdownWrite();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
