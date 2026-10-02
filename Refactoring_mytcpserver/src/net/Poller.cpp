#include "net/Poller.h"

/*
 * 这里是全项目唯一允许出现 epoll 头文件的地方。
 * 其它任何文件想碰 epoll，都得先来改这个类 —— 这就是设计要的效果。
 */
#include <sys/epoll.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>

#include "base/Logger.h"
#include "net/Channel.h"
#include "net/EventLoop.h"
#include <assert.h>

// 匿名空间 表达“这就是本文件的私有实现细节”，只在这个文件可见
namespace {
    const int kInitialEventListSize = 16;
}

Poller::Poller(EventLoop* loop) :
    ownerLoop_(loop),
    epollfd_(::epoll_create1(EPOLL_CLOEXEC))
{
    if (epollfd_ == -1) {
        LOG_FATAL << "epoll_create1 error: " << std::strerror(errno);
        abort();
    }
}

Poller::~Poller()
{
    assert(channels_.empty());
    ::close(epollfd_);
}

void Poller::assertInLoopThread()
{
    ownerLoop_->assertInLoopThread();
}

void Poller::poll(int timeoutMs, ChannelList* activeChannels)
{
    // 确保 Poller 只能在它所属的 EventLoop 的线程中使用
    assertInLoopThread();
    /* 1. epoll_wait
     * 2. events[i].data.ptr 转 Channel*
     * 3. set_revents(events[i].events)
     * 4. activeChannels->push_back(...)
     * 5. 处理 EINTR / errno
     */
    std::vector<struct epoll_event> events(kInitialEventListSize);      // 创建一个 epoll_event 数组，大小为 16
    /*
        * 阻塞等待事件，把活跃的 Channel 填进 activeChannels
        * epoll_wait 返回活跃事件的数量
        * events.data() 返回指向数组首元素的指针
        * static_cast<int>(events.size()) 将数组大小转换为 int 类型
        * timeoutMs 是等待的超时时间，单位为毫秒
        * 如果返回值大于 0，表示有活跃事件发生
        * 如果返回值等于 0，表示超时，没有活跃事件发生
        * 如果返回值小于 0，表示发生错误，errno 会被设置为相应的错误码
        * 需要处理 EINTR（被信号打断，继续）和其它 errno（记日志）。
    */

    // vector.data() 返回指向数组首元素的指针
    int numEvents = ::epoll_wait(epollfd_, events.data(), static_cast<int>(events.size()), timeoutMs);      // static_cast<int>(x.size()): 显示类型转换,将x.size()转换为int类型

    // 如果有事件发生，遍历事件数组，将每个事件的 Channel 对象添加到 activeChannels 中
    if (numEvents > 0) {
        for (int i = 0; i < numEvents; ++i) {
            Channel* channel = static_cast<Channel*>(events[i].data.ptr);   // 将epoll_event的data.ptr指针关联到Channel对象上
            channel->set_revents(events[i].events);                         // 将发生的事件类型设置到Channel对象中
            activeChannels->push_back(channel);                             // 将有事件发生的Channel对象添加到activeChannels列表中
        }
    }

    // 如果没有事件发生，表示超时，没有活跃事件发生
    if (numEvents == 0) {
        return ; // 超时，没有活跃事件发生
    }

    // 如果发生错误，处理 EINTR（被信号打断，继续）和其它 errno（记日志）。
    if (numEvents < 0) {
        if (errno != EINTR) {
            LOG_ERROR << "epoll_wait error: " << std::strerror(errno);
        } else {
            // 被信号打断，继续等待
            LOG_DEBUG << "epoll_wait interrupted by signal, continue waiting";
        }
    }

}

void Poller::update(int operation, Channel* channel)
{
    // 确保 Poller 只能在它所属的 EventLoop 的线程中使用
    assertInLoopThread();

    // 根据操作类型（EPOLL_CTL_ADD / EPOLL_CTL_MOD / EPOLL_CTL_DEL）更新 Channel 的 epoll 注册状态
    struct epoll_event event;
    event.events = channel->events();        // 设置事件类型
    event.data.ptr = channel;                // 将 Channel 对象的指针存储在 epoll_event 的 data.ptr 中

    // 调用 epoll_ctl 更新 epoll 注册状态
    if (::epoll_ctl(epollfd_, operation, channel->fd(), &event) < 0) {
        LOG_DEBUG << "epoll_ctl error: " << std::strerror(errno) << ", operation: " << operation << ", fd: " << channel->fd();
    }
}

void Poller::updateChannel(Channel* channel)
{
    assertInLoopThread();
    // 根据 channel->index() 三态决定调哪个 epoll_ctl
    int index = channel->index();
    int fd = channel->fd();

    if (index == Channel::kNew || index == Channel::kDeleted) {
        // 如果是新事件或者已经删除的事件，调用 epoll_ctl 添加事件
        // 这一支统一是 ADD：kNew 是第一次注册，
        // kDeleted 是 disableAll() 之后又重新启用（内核侧已经摘掉了）
        if (index == Channel::kNew) {
            // 防止旧的 Channel 还在 channels_ 里，新的 Channel 又来注册，导致 channels_ 里有两个 Channel 指针指向同一个 fd
            assert(channels_.find(fd) == channels_.end());
            channels_[fd] = channel;
        } else {
            assert(channels_.find(fd) != channels_.end());
            assert(channels_[fd] == channel);
        }
        channel->set_index(Channel::kAdded);
        update(EPOLL_CTL_ADD, channel);
    } else {
        // 已经注册的事件，调用 epoll_ctl 修改事件
        assert(index == Channel::kAdded);
        assert(channels_.find(fd) != channels_.end());
        assert(channels_[fd] == channel);

        if (channel->isNoneEvent()) {
            // 如果没有事件发生，调用 epoll_ctl 删除事件
            update(EPOLL_CTL_DEL, channel);
            channel->set_index(Channel::kDeleted);
        } else {
            // 如果有事件发生，调用 epoll_ctl 修改事件
            update(EPOLL_CTL_MOD, channel);
        }
    }



    /*
    不能直接调用epoll_ctl, 需要通过update函数来调用epoll_ctl, update函数是epoll_ctl唯一的调用接口，统一封装好方便管理
    if (index == Channel::kNew) {
        // 来新事件，第一次注册，调用 epoll_ctl 添加事件
        if (::epoll_ctl(epollfd_, EPOLL_CTL_ADD, fd, &event) < 0)
            LOG_ERROR << "epoll_ctl add error: " << std::strerror(errno);
    } else if (index == Channel::kAdded) {
        // 已经注册过的事件，修改事件类型，调用 epoll_ctl 修改事件
        if (::epoll_ctl(epollfd_, EPOLL_CTL_MOD, fd, &event) < 0)
            LOG_ERROR << "epoll_ctl mod error: " << std::strerror(errno);
    } else if (index == Channel::kDeleted) {
        // 已经删除的事件，重新注册事件，调用 epoll_ctl 添加事件
        if (::epoll_ctl(epollfd_, EPOLL_CTL_ADD, fd, &event) < 0)
            LOG_ERROR << "epoll_ctl add error: " << std::strerror(errno);
    }
    */

    // 更新 channel 的 index 状态
    channel->set_index(channel->index() == Channel::kNew ? Channel::kAdded : channel->index());

    // 记录 channels_ 映射关系
    channels_[fd] = channel;
}

void Poller::removeChannel(Channel* channel)
{
    // 拿东西
    assertInLoopThread();
    int index = channel->index();           // 先拿到channel的index状态
    int fd = channel->fd();                 // 拿到channel的fd

    // 判状态
    if (index == Channel::kAdded) {
        // 如果是已经注册的事件，调用 epoll_ctl 删除事件
        if (::epoll_ctl(epollfd_, EPOLL_CTL_DEL, fd, nullptr) < 0)
            LOG_ERROR << "epoll_ctl del error: " << std::strerror(errno);
    }
    /*
    只有真正注册在epoll上的才需要delete
    如果是kNew说明从来没注册成功过
    如果是kDeleted说明已经从epoll上删除过了
    这两种情况都不需要再调用epoll_ctl删除
    if (index == Channel::kDeleted) {
        // 如果是已经删除的事件，说明已经从 epoll 中移除了，不需要重复删除
        LOG_DEBUG << "Channel fd " <<fd << " is already deleted from epoll";
    }

    if (index == Channel::kNew) {
        // 如果是新事件说明还没有注册到 epoll 中，不需要删除
        LOG_DEBUG << "Channel fd " << fd << " is new, not registered in epoll";
    }
    */
    // 删channel
    channels_.erase(fd);
    // 这里有个坑: 不改成kDeleted而是改成kNew,
    channel->set_index(Channel::kNew);
}

/*
 * ============ Poller 实现清单（README 第四节「第 3 步」）============
 *
 * 0) 文件顶部加一组内部常量：
 *      namespace {
 *      const int kInitialEventListSize = 16;
 *      }
 *
 * 1) 构造：epollfd_ = ::epoll_create1(EPOLL_CLOEXEC);
 *    失败直接 abort —— epoll_create1 失败基本等于系统出问题了。
 *    注意用 epoll_create1 而不是老的 epoll_create(1024)。
 *
 * 2) 析构：assert(channels_.empty()); ::close(epollfd_);
 *    第一句是给自己看的：还在 epoll 里注册着的 Channel 一定说明有连接泄漏。
 *
 * 3) poll(timeoutMs, activeChannels)
 *      std::vector<struct epoll_event> events(kInitialEventListSize);
 *      int num = ::epoll_wait(epollfd_, events.data(), events.size(), timeoutMs);
 *      if (num > 0) 把每个 event 的 data.ptr 转回 Channel*，
 *                   set_revents(events[i].events)，塞进 activeChannels；
 *      else if (num == 0) 超时，什么都不做；
 *      else 处理 EINTR（被信号打断，继续）和其它 errno（记日志）。
 *
 *      为什么 epoll_event 数组不放在成员里：Poller.h 里没有 <sys/epoll.h>，
 *      类型不完整，标准库容器做不了成员。等真量出分配开销，再用 PIMPL 挪回成员。
 *
 *      注意 activeChannels->clear() 放在 poll() 里面还是外面都行，
 *      但一定要保证「每次 poll 前是空的」。建议放 EventLoop::loop() 里，
 *      让 Poller 只管填。
 *
 * 4) updateChannel(channel) —— 三态决定 epoll_ctl 的操作码：
 *      index == kNew 或 kDeleted -> EPOLL_CTL_ADD
 *      index == kAdded            -> EPOLL_CTL_MOD
 *    第一次 ADD 时记得 channel->set_index(kAdded)，
 *    并且记录 channels_[channel->fd()] = channel。
 *
 * 5) removeChannel(channel)
 *      从 channels_ 里 erase，调 EPOLL_CTL_DEL，
 *      然后把 index 设成 kNew（不是 kDeleted）——
 *      被 remove 掉的 Channel 通常马上就要析构了，不存在「删了还要加回来」。
 *      updateChannel 里判断「kDeleted 要 ADD」是为了支持 disableAll() 之后再 enableReading()。
 *
 * 6) update(operation, channel) —— 真正调 epoll_ctl 的地方，
 *    失败时要能分辨 EEXIST / ENOENT，日志里带上 operation 和 fd。
 *
 * 7) assertInLoopThread() —— 转发给 ownerLoop_->assertInLoopThread()。
 *    Poller 不是线程安全的，靠这一句把误用挡在开发期。
 *
 * 验收：写个 30 行的 main 直接驱动 Poller + Channel（先不引入 EventLoop），
 *       收发包能通就说明事件分发这一段是对的。
 */

// namespace {
// const int kInitialEventListSize = 16;
// }
//
// Poller::Poller(EventLoop* loop)
//     : ownerLoop_(loop),
//       epollfd_(::epoll_create1(EPOLL_CLOEXEC))
// {
//     if (epollfd_ < 0) {
//         LOG_FATAL << "epoll_create1 failed: " << std::strerror(errno);
//         abort();
//     }
// }
//
// Poller::~Poller()
// {
//     assert(channels_.empty());
//     ::close(epollfd_);
// }
//
// void Poller::poll(int timeoutMs, ChannelList* activeChannels)
// {
//     /* 1. epoll_wait
//      * 2. events[i].data.ptr 转 Channel*
//      * 3. set_revents(events[i].events)
//      * 4. activeChannels->push_back(...)
//      * 5. 处理 EINTR / errno
//      */
// }
//
// void Poller::updateChannel(Channel* channel) { /* ... */ }
// void Poller::removeChannel(Channel* channel) { /* ... */ }
// void Poller::update(int operation, Channel* channel) { /* epoll_ctl ... */ }
// void Poller::assertInLoopThread() { ownerLoop_->assertInLoopThread(); }
