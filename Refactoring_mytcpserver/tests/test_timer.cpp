#include "base/Timestamp.h"
#include "net/EventLoop.h"

#include <cstdio>

/*
 * TimerQueue 的测试（README 第四节「第 4 步」的验收）。
 *
 * 和 test_buffer 不一样，这个测试必须真跑一个 EventLoop ——
 * 定时器是「时间」和「事件循环」协作的结果，没法纯逻辑测。
 *
 * 断言重点不是「回调被调到了」，而是「什么时候被调到的」：
 * 偏差应该在几毫秒以内。如果偏差是几十甚至几百毫秒，
 * 说明 timerfd 的 arm 时机或者 timespec 换算有问题。
 *
 * 另外一定要有「同一时刻到期的多个定时器，按加入顺序触发」这个用例 ——
 * 它守的是 TimerQueue.h 里 Entry::operator< 那个 sequence 字段。
 * 少了这一条，同一时刻的定时器就会按指针地址乱序触发，
 * 而且这个 bug 只在压力下才暴露，非常难查。
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

static void testRunAfterFiresOnce()
{
    printf("testRunAfterFiresOnce\n");
    /* TODO: runAfter(0.1, cb)，记下触发时刻，断言只触发一次且偏差 < 30ms */
}

static void testRunEveryFiresRepeatedly()
{
    printf("testRunEveryFiresRepeatedly\n");
    /* TODO: runEvery(0.05, cb)，跑 0.3 秒，断言触发次数大约 6 次 */
}

static void testSameDeadlinePreservesOrder()
{
    printf("testSameDeadlinePreservesOrder\n");
    /* TODO: 同一时刻加 3 个定时器，断言回调执行顺序 == 加入顺序 */
}

static void testTimerAddedInsideCallback()
{
    printf("testTimerAddedInsideCallback\n");

    /*
     * TODO: 在回调里再加一个定时器，断言它也能正常触发。
     *       这条守的是 handleRead 里「先 getExpired 摘掉 → 再 run → 最后 reset」
     *       那个执行顺序：如果顺序写错，回调里新加的定时器会被这次
     *       的 getExpired 结果覆盖掉，表现为「偶尔漏触发一次」。
     */
}

int main()
{
    testRunAfterFiresOnce();
    testRunEveryFiresRepeatedly();
    testSameDeadlinePreservesOrder();
    testTimerAddedInsideCallback();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
