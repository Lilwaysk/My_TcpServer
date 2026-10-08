#include "net/InetAddress.h"

#include <arpa/inet.h>
#include <csignal>
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>

// InetAddress 的测试（README「第 5 步」的验收）。
//
// 这一层全是「结构体初始化 + 字节序 + 地址解析」的琐事，
// 所以断言的重点是：写进去的数能原样读出来、字节序没搞反、
// 非法 IP 不会像老代码那样静默变成 255.255.255.255。

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

static void testAnyAddress()
{
    printf("testAnyAddress\n");

    InetAddress addr(8080);                 // loopbackOnly = false -> 0.0.0.0

    EXPECT_TRUE(addr.port() == 8080);
    EXPECT_TRUE(addr.toIp() == "0.0.0.0");
    EXPECT_TRUE(addr.toIpPort() == "0.0.0.0:8080");
    EXPECT_TRUE(addr.getSockAddrInet().sin_family == AF_INET);
    EXPECT_TRUE(addr.getSockAddrInet().sin_port == ::htons(8080));  // 网络字节序
}

static void testLoopbackAddress()
{
    printf("testLoopbackAddress\n");

    InetAddress addr(0, true);              // port 0 = 让内核挑，loopbackOnly = true

    EXPECT_TRUE(addr.toIp() == "127.0.0.1");
    EXPECT_TRUE(addr.port() == 0);
    EXPECT_TRUE(addr.getSockAddrInet().sin_addr.s_addr == ::htonl(INADDR_LOOPBACK));
}

static void testFromIpString()
{
    printf("testFromIpString\n");

    InetAddress addr("192.168.1.10", 65535);

    EXPECT_TRUE(addr.toIp() == "192.168.1.10");
    EXPECT_TRUE(addr.port() == 65535);
    EXPECT_TRUE(addr.toIpPort() == "192.168.1.10:65535");
}

static void testRoundTripThroughSockAddr()
{
    printf("testRoundTripThroughSockAddr\n");

    InetAddress a("10.0.0.7", 1234);
    InetAddress b(a.getSockAddrInet());     // 从裸 sockaddr_in 再包回来

    EXPECT_TRUE(b.toIp() == "10.0.0.7");
    EXPECT_TRUE(b.port() == 1234);
    EXPECT_TRUE(b.toIp() == a.toIp());
}

// 非法 IP 必须当场炸掉（LOG_FATAL + abort），而不是静默变成某个地址。
// 用 fork 把 abort 关在子进程里，父进程检查它是不是被 SIGABRT 终止。
static void testInvalidIpAborts()
{
    printf("testInvalidIpAborts\n");

    const char* badIps[] = {"999.1.1.1", "not.an.ip", "256.256.256.256"};

    for (const char* ip : badIps) {
        pid_t pid = ::fork();
        if (pid == 0) {
            // 子进程：这里应该 abort，不会走到 _exit(0)
            InetAddress bad(ip, 80);
            (void)bad;
            ::_exit(0);
        }

        int status = 0;
        ::waitpid(pid, &status, 0);
        bool aborted = WIFSIGNALED(status) && (WTERMSIG(status) == SIGABRT);
        EXPECT_TRUE(aborted);
        if (!aborted)
            printf("  ip \"%s\" 没有 abort（status=%d）\n", ip, status);
    }
}

int main()
{
    testAnyAddress();
    testLoopbackAddress();
    testFromIpString();
    testRoundTripThroughSockAddr();
    testInvalidIpAborts();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
