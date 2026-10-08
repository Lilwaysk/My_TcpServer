#include "base/Timestamp.h"
#include "net/EventLoop.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

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

/* a 到 b 之间经过了多少微秒 */
static int64_t usBetween(Timestamp a, Timestamp b)
{
    return b.microSecondsSinceEpoch() - a.microSecondsSinceEpoch();
}

static void testRunAfterFiresOnce()
{
    printf("testRunAfterFiresOnce\n");

    EventLoop loop;
    int count = 0;
    Timestamp scheduled = Timestamp::now();
    Timestamp fired;

    loop.runAfter(0.1, [&]() {
        ++count;
        fired = Timestamp::now();
        loop.quit();
    });
    /* 安全阀：定时器真出问题时别让测试挂死，1 秒后强制退出 */
    loop.runAfter(1.0, [&]() { loop.quit(); });

    loop.loop();

    EXPECT_TRUE(count == 1);            /* 一次性定时器只触发一次 */
    EXPECT_TRUE(fired.valid());
    if (fired.valid()) {
        int64_t actual = usBetween(scheduled, fired);
        int64_t dev = actual - 100000;  /* 预期 100ms */
        printf("  fired after %.3f ms, deviation %.3f ms\n",
               actual / 1000.0, dev / 1000.0);
        EXPECT_TRUE(std::llabs(dev) < 30000);   /* 偏差 < 30ms */
    }
}

static void testRunEveryFiresRepeatedly()
{
    printf("testRunEveryFiresRepeatedly\n");

    EventLoop loop;
    int count = 0;

    loop.runEvery(0.05, [&]() { ++count; });
    loop.runAfter(0.31, [&]() { loop.quit(); });

    loop.loop();

    /* 0.31 秒里，50ms 周期大概触发 6 次，放宽到 5~7 次 */
    printf("  fired %d times in ~0.31s\n", count);
    EXPECT_TRUE(count >= 5 && count <= 7);
}

static void testSameDeadlinePreservesOrder()
{
    printf("testSameDeadlinePreservesOrder\n");

    EventLoop loop;
    std::vector<int> order;
    Timestamp when = addTime(Timestamp::now(), 0.05);

    for (int i = 0; i < 3; ++i) {
        loop.runAt(when, [&order, i]() { order.push_back(i); });
    }
    loop.runAfter(0.3, [&]() { loop.quit(); });

    loop.loop();

    EXPECT_TRUE(order.size() == 3);
    bool inOrder = (order.size() == 3 && order[0] == 0 && order[1] == 1 && order[2] == 2);
    EXPECT_TRUE(inOrder);
    if (!inOrder)
        printf("  order = [%zu]: %d %d %d\n", order.size(),
               order.size() > 0 ? order[0] : -1,
               order.size() > 1 ? order[1] : -1,
               order.size() > 2 ? order[2] : -1);
}

static void testTimerAddedInsideCallback()
{
    printf("testTimerAddedInsideCallback\n");

    EventLoop loop;
    bool secondFired = false;

    /*
     * 在回调里再加一个定时器。这条守的是 handleRead 里
     * 「先 getExpired 摘掉 → 再 run → 最后 reset」的执行顺序：
     * 顺序写错的话，回调里新加的定时器会被这次的 getExpired 结果覆盖掉，
     * 表现为「偶尔漏触发一次」。
     */
    loop.runAfter(0.05, [&]() {
        loop.runAfter(0.05, [&]() {
            secondFired = true;
            loop.quit();
        });
    });
    loop.runAfter(1.0, [&]() { loop.quit(); });  /* 安全阀 */

    loop.loop();

    EXPECT_TRUE(secondFired);
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
