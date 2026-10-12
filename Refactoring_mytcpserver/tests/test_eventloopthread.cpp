#include "net/EventLoopThread.h"

#include <atomic>
#include <cstdio>

#include "net/EventLoop.h"

int main()
{
    std::atomic<bool> callbackRan(false);
    std::atomic<bool> callbackInLoopThread(false);

    {
        EventLoopThread thread([&](EventLoop* loop) {
            callbackInLoopThread = loop->isInLoopThread();
            callbackRan = true;
        });

        EventLoop* loop = thread.startLoop();
        if (loop == nullptr) {
            std::fprintf(stderr, "startLoop returned a null EventLoop\n");
            return 1;
        }
        loop->quit();
    }

    if (!callbackRan || !callbackInLoopThread) {
        std::fprintf(stderr, "thread initialization callback did not run correctly\n");
        return 1;
    }

    std::puts("EventLoopThread startup, callback, quit, and join passed");
    return 0;
}
