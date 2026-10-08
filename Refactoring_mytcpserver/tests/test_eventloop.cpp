#include "net/EventLoop.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

/*
 * EventLoop 的测试（README 第四节「第 4 步」的验收）。
 *
 * 重点在「线程模型」这一段，也就是这个库最核心也最容易写错的几行：
 *   - runInLoop 在本线程里是同步执行；跨线程时必须挪到 loop 线程执行；
 *   - queueInLoop 从别的线程投递任务时，必须 wakeup 把阻塞在 epoll_wait
 *     的 loop 踹醒 —— 少这一脚，投进去的任务会被卡到下一个事件才跑；
 *   - quit() 从别的线程调用时同样要能唤醒 loop 并退出。
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

static void testRunInLoopSameThreadIsSynchronous()
{
    printf("testRunInLoopSameThreadIsSynchronous\n");

    EventLoop loop;
    bool ran = false;

    loop.runInLoop([&]() { ran = true; });

    /* 已经在 loop 线程里，runInLoop 必须当场同步执行，不经过队列 */
    EXPECT_TRUE(ran);
    EXPECT_TRUE(loop.isInLoopThread());
}

static void testIsInLoopThreadFromOtherThread()
{
    printf("testIsInLoopThreadFromOtherThread\n");

    EventLoop loop;
    std::atomic<bool> otherSaysInLoop(true);

    std::thread t([&]() { otherSaysInLoop = loop.isInLoopThread(); });
    t.join();

    /* 其它线程必须被识别为「不在 loop 线程」 */
    EXPECT_TRUE(!otherSaysInLoop);
}

static void testRunInLoopFromOtherThreadIsDeferred()
{
    printf("testRunInLoopFromOtherThreadIsDeferred\n");

    EventLoop loop;
    std::atomic<bool> ran(false);
    std::thread::id execThread;
    const std::thread::id loopThread = std::this_thread::get_id();

    std::thread t([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        /* 此刻 main 正阻塞在 epoll_wait，靠 wakeup 才能把任务跑起来 */
        loop.runInLoop([&]() {
            ran = true;
            execThread = std::this_thread::get_id();
            loop.quit();
        });
    });

    loop.loop();
    t.join();

    EXPECT_TRUE(ran);
    EXPECT_TRUE(execThread == loopThread);   /* 回调必须在 loop 线程执行 */
}

static void testQueueInLoopPreservesOrderAndWakesUp()
{
    printf("testQueueInLoopPreservesOrderAndWakesUp\n");

    EventLoop loop;
    std::vector<int> order;

    std::thread t([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (int i = 0; i < 5; ++i) {
            loop.queueInLoop([&order, i]() { order.push_back(i); });
        }
        loop.queueInLoop([&]() { loop.quit(); });
    });

    loop.loop();
    t.join();

    EXPECT_TRUE(order.size() == 5);
    bool inOrder = (order.size() == 5);
    for (size_t i = 0; inOrder && i < order.size(); ++i) {
        if (order[i] != static_cast<int>(i)) inOrder = false;
    }
    EXPECT_TRUE(inOrder);   /* 同一线程投递的任务按 FIFO 执行 */
}

static void testQuitFromOtherThreadWhileBlocked()
{
    printf("testQuitFromOtherThreadWhileBlocked\n");

    EventLoop loop;
    std::atomic<bool> tDone(false);

    std::thread t([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        loop.quit();            /* loop 正在 epoll_wait，quit 必须唤醒它 */
        tDone = true;
    });

    loop.loop();                /* 没有其它事件，只能靠 quit 的 wakeup 返回 */
    t.join();

    EXPECT_TRUE(tDone);
}

int main()
{
    testRunInLoopSameThreadIsSynchronous();
    testIsInLoopThreadFromOtherThread();
    testRunInLoopFromOtherThreadIsDeferred();
    testQueueInLoopPreservesOrderAndWakesUp();
    testQuitFromOtherThreadWhileBlocked();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
