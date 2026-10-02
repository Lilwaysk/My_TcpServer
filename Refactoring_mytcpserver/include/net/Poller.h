#ifndef NET_POLLER_H
#define NET_POLLER_H

#include <map>
#include <vector>

#include "base/noncopyable.h"

class Channel;
class EventLoop;

/*
 * epoll 的唯一封装层。整个项目里只有 Poller.cpp 能碰 epoll_ctl / epoll_wait。
 *
 * 为什么值得这么死板：上面所有类都不再依赖 Linux 特有头文件，
 * 以后换 poll / kqueue / io_uring 只动这一个文件。
 *
 * 注意本头文件刻意不 include <sys/epoll.h>（README 第二节的规矩），
 * 所以 epoll_event 数组没有做成成员，而是在 poll() 内部按需分配。
 * 等这一步跑通、确实量出分配开销了，再用 PIMPL 挪回成员。
 */
class Poller : noncopyable {
public:
    typedef std::vector<Channel*> ChannelList;              // 定义一个 Channel 指针的数组，用于存储活跃的 Channel 对象

    explicit Poller(EventLoop* loop);
    ~Poller();

    /* 阻塞等待事件，把活跃的 Channel 填进 activeChannels */
    void poll(int timeoutMs, ChannelList* activeChannels);

    /* 注册 / 修改 / 注销，内部按 channel->index() 三态决定调哪个 epoll_ctl */
    void updateChannel(Channel* channel);                   // 更新 Channel 的 epoll 注册状态
    void removeChannel(Channel* channel);                   // 移除 Channel 的 epoll 注册状态

    void assertInLoopThread();                              // 确保 Poller 只能在它所属的 EventLoop 的线程中使用

private:
    void update(int operation, Channel* channel);           // 根据操作类型（EPOLL_CTL_ADD / EPOLL_CTL_MOD / EPOLL_CTL_DEL）更新 Channel 的 epoll 注册状态

    EventLoop* ownerLoop_;
    int epollfd_;
    std::map<int, Channel*> channels_;      /* fd -> Channel*，析构时要检查它是否为空 */
};

#endif
