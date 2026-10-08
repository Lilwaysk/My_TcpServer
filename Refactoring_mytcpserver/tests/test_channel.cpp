#include "net/Channel.h"
#include "net/EventLoop.h"

#include <cstdio>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * Channel / Poller 的驱动测试（README「第 3 步」的验收）。
 *
 * Channel 绕不开 EventLoop（Channel::update() 就是 loop_->updateChannel(this)），
 * 所以这里直接起一个真 EventLoop 来驱动事件分发，而不是去访问 EventLoop
 * 私有的 poller_ —— 对外接口能验证的东西，就不该靠 friend 打开内部。
 *
 * 用例思路：用 eventfd / socketpair 造一个可读的 fd 挂到 Channel 上，
 * 触发后看 readCallback_ 有没有被调到，问题范围就锁在 Channel / Poller 里。
 *
 * 注意：Channel 不拥有 fd，也不负责从 Poller 注销自己。
 * 每个用例结束前必须 ch.remove()，否则 Poller 析构时的
 * assert(channels_.empty()) 会直接 abort。
 */

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

static void writeEventfd(int fd)
{
    uint64_t one = 1;
    ssize_t n = ::write(fd, &one, sizeof(one));
    (void)n;
}

static void testEnableReadingTriggersCallback()
{
    printf("testEnableReadingTriggersCallback\n");

    int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    EventLoop loop;
    Channel ch(&loop, efd);
    bool called = false;

    ch.setReadCallback([&]() {
        called = true;
        uint64_t v = 0;
        ssize_t n = ::read(efd, &v, sizeof(v));   /* 读走计数，否则水平触发会一直报 */
        (void)n;
        loop.quit();
    });
    ch.enableReading();

    writeEventfd(efd);                            /* 让它变可读 */
    loop.runAfter(1.0, [&]() { loop.quit(); });   /* 安全阀 */
    loop.loop();

    EXPECT_TRUE(called);
    EXPECT_TRUE(ch.index() == Channel::kAdded);

    ch.remove();
    ::close(efd);
}

static void testDisableReadingStopCallback()
{
    printf("testDisableReadingStopCallback\n");

    int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    EventLoop loop;
    Channel ch(&loop, efd);
    bool called = false;

    ch.setReadCallback([&]() {
        called = true;
        uint64_t v = 0;
        ssize_t n = ::read(efd, &v, sizeof(v));
        (void)n;
    });
    ch.enableReading();
    ch.disableReading();

    writeEventfd(efd);                       /* 再写也不会回调了 */
    loop.runAfter(0.05, [&]() { loop.quit(); });
    loop.loop();

    EXPECT_TRUE(!called);

    ch.remove();
    ::close(efd);
}

static void testRemoveUnregistersFromPoller()
{
    printf("testRemoveUnregistersFromPoller\n");

    int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    EventLoop loop;
    Channel ch(&loop, efd);
    bool called = false;

    ch.setReadCallback([&]() {
        called = true;
        uint64_t v = 0;
        ssize_t n = ::read(efd, &v, sizeof(v));
        (void)n;
    });
    ch.enableReading();
    EXPECT_TRUE(ch.index() == Channel::kAdded);

    ch.remove();
    /* remove() 之后 index 应回到 kNew（不是 kDeleted）——Poller 里也没了这条记录 */
    EXPECT_TRUE(ch.index() == Channel::kNew);

    writeEventfd(efd);                       /* 已注销，写进去也不该有回调 */
    loop.runAfter(0.05, [&]() { loop.quit(); });
    loop.loop();

    EXPECT_TRUE(!called);

    ::close(efd);
}

static void testPeerCloseStillCallsReadCallback()
{
    printf("testPeerCloseStillCallsReadCallback\n");

    int sv[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        printf("  socketpair failed, skip\n");
        return;
    }

    EventLoop loop;
    Channel ch(&loop, sv[0]);
    bool called = false;
    ssize_t nread = -2;

    ch.setReadCallback([&]() {
        char buf;
        nread = ::read(sv[0], &buf, 1);      /* 对端已关，应该读到 0（EOF） */
        called = true;
        loop.quit();
    });
    ch.enableReading();

    ::close(sv[1]);                               /* 对端直接关闭 */
    loop.runAfter(1.0, [&]() { loop.quit(); });   /* 安全阀 */
    loop.loop();

    /*
     * 守的是 handleEventWithGuard 里的坑一：对端关闭时可能只拿到 POLLHUP
     * 而没有 POLLIN，如果实现里写成「只有 POLLIN 才调 readCallback_」，
     * 这个用例会挂 —— 而那个 bug 在真实连接里表现为「连接泄漏」。
     */
    EXPECT_TRUE(called);
    EXPECT_TRUE(nread == 0);

    ch.remove();
    ::close(sv[0]);
}

int main()
{
    testEnableReadingTriggersCallback();
    testDisableReadingStopCallback();
    testRemoveUnregistersFromPoller();
    testPeerCloseStillCallsReadCallback();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
