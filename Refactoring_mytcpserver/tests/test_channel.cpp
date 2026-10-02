#include "net/Channel.h"
#include "net/EventLoop.h"
#include "net/Poller.h"

#include <cstdio>
#include <sys/eventfd.h>
#include <unistd.h>

/*
 * Channel / Poller 的驱动测试（README 第四节「第 3 步」的验收）。
 *
 * 一个前置说明：README 里写的是「先不引入 EventLoop」，但真正看代码会发现
 * Channel::update() 的实现就是 loop_->updateChannel(this) —— 它绕不开
 * EventLoop 这个对象。
 *
 * 实际可行的拆法是：第 3 步只实现 EventLoop 里转发用的那几个函数
 * （updateChannel / removeChannel / isInLoopThread / assertInLoopThread），
 * 几十行就够；loop() / wakeup / 定时器留到第 4 步再补。
 * 这样第 3 步的验证范围仍然被限制在「事件分发」这一段。
 *
 * 测试思路：用 eventfd 造一个可写的 fd 挂到 Channel 上，
 * 往里面写 8 字节，看 readCallback_ 有没有被调到。
 * 不用 socketpair 是因为 eventfd 更简单，不经过协议栈，
 * 一旦失败，问题范围就只可能在 Channel / Poller 里。
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

static void testEnableReadingTriggersCallback()
{
    printf("testEnableReadingTriggersCallback\n");

    /*
     * TODO:
     *   int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
     *   EventLoop loop;
     *   Channel ch(&loop, efd);
     *   bool called = false;
     *   ch.setReadCallback([&]{ called = true; uint64_t v; ::read(efd, &v, sizeof v); });
     *   ch.enableReading();
     *   ::write(efd, ...);              // 让它变可读
     *   loop.poller_->poll(0, &channels);  // 或者跑一次 loop 的 poll 阶段
     *   逐个 ch->handleEvent();
     *   EXPECT_TRUE(called);
     */
}

static void testDisableReadingStopCallback()
{
    printf("testDisableReadingStopCallback\n");
    /* TODO: disableReading() 之后再写 eventfd，回调不应该被调到 */
}

static void testRemoveUnregistersFromPoller()
{
    printf("testRemoveUnregistersFromPoller\n");
    /* TODO: ch.remove() 之后 Poller 内部的 channels_ 应为空 */
}

static void testPeerCloseStillCallsReadCallback()
{
    printf("testPeerCloseStillCallsReadCallback\n");

    /*
     * TODO: 用 socketpair 造一对 fd，把其中一端挂到 Channel 上，
     *      另一端直接 close()，然后 poll 一次。
     *
     *      这个用例专门守 handleEventWithGuard 里的坑一：
     *      对端关闭时可能只拿到 POLLHUP 而没有 POLLIN，
     *      如果实现里写成「只有 POLLIN 才调 readCallback_」，
     *      这个用例会挂 —— 而那个 bug 在真实连接里表现为「连接泄漏」。
     */
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
