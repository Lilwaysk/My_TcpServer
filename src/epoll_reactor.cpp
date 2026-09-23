/*
 * epoll_reactor.cpp —— 单进程 + epoll 的 Reactor（反应堆）模型 TCP 服务器
 *
 * 核心思想：
 *   1. 用一个 epoll 实例（g_efd）同时管理「监听 socket」和「所有客户端 socket」；
 *   2. 每个被管理的 fd 都配一个 myevent_s 结构，记录它关心的事件和就绪后要调用的回调；
 *   3. 主循环里 epoll_wait 只负责“等”，谁就绪就调谁的回调函数 —— 事件分发和业务处理
 *      就此分开，这就是 Reactor 模式。
 *
 * 一次完整的事件流转：
 *   lfd 可读 -> acceptconn 接入新连接 -> cfd 可读 -> recvdata 收数据
 *            -> cfd 可写 -> senddata 回写    -> 切回 recvdata 继续等读
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>       /* time：记录连接的最后活跃时间，用于超时踢人 */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h> /* sockaddr_in、htons、INADDR_ANY 等 */
#include <arpa/inet.h>  /* inet_ntoa、ntohs */
#include <sys/epoll.h>  /* epoll_create / epoll_ctl / epoll_wait 和 struct epoll_event */

#define MAX_EVENTS 1024 /* 最多同时管理的客户端连接数 */
#define BUFLEN 4096     /* 每个连接的收发缓冲区大小 */
#define SERV_PORT 8080  /* 默认监听端口 */

void recvdata(int fd, int events, void *arg);
void senddata(int fd, int events, void *arg);

/*
 * 一个 fd（socket）对应的全部状态：fd 是谁、关心哪些事件、就绪后执行哪个函数、
 * 数据存在哪、多久没活动了。
 * 内核的 epoll_event 里只能塞一个 void*，我们塞的就是这个结构体的地址，
 * 事件就绪时再原样取回来，就知道该处理哪个连接。
 */
struct myevent_s {
    int fd;                                             // 要监听的 fd（listen fd 或 client fd）
    int events;                                         // 当前监听的事件：EPOLLIN（读）或 EPOLLOUT（写）
    void *arg;                                          // 回调时回传的参数，本项目里就是本结构体自己
    void (*call_back)(int fd, int events, void *arg);   // 就绪后要执行的函数（回调）
    int status;                                         // 是否挂在 epoll 上：1 = 在，0 = 不在
    char buf[BUFLEN];                                   // 收发缓冲区
    int len;                                            // buf 中实际有效的字节数（recv 读到的长度）
    long last_active;                                   // 最后一次活跃的时间戳，用于超时断开
};

int g_efd;                                 // epoll 实例的 fd（epoll_create 的返回值）
struct myevent_s g_events[MAX_EVENTS + 1]; // 所有连接的状态表，最后一个位置留给 listen fd

/* 把 fd 从 epoll 上摘下来（只是不再监听，并没有 close） */
void eventdel(int efd, struct myevent_s *ev)
{
    struct epoll_event epv = {0,{0}};

    if (ev->status != 1)                                // 本来就不在 epoll 上，直接返回
        return ;

    epv.data.ptr = NULL;
    ev->status = 0;                                     // 先改状态，再摘除
    epoll_ctl(efd, EPOLL_CTL_DEL, ev->fd, &epv);        // 从 epoll 实例 efd 中删除 ev->fd

    return ;
}

/* 初始化（重置）一个 myevent_s：登记 fd、回调函数和参数，此时还没挂到 epoll 上 */
void eventset(struct myevent_s *ev, int fd, void (*call_back)(int, int, void *), void *arg)
{
    ev->fd = fd;
    ev->call_back = call_back;
    ev->events = 0;
    ev->arg = arg;
    ev->status = 0;                     // 只填结构体，还没上树，所以状态是 0
    ev->last_active = time(NULL);       // 记下活跃时间，供主循环的超时扫描使用

    /*
     * 注意：这里只重置控制信息，千万不要 memset(ev->buf) 或者把 ev->len 清零。
     * recvdata 收到数据后要靠 eventset 把回调换成 senddata，
     * 一旦在这里把缓冲区清空，刚收的数据就没了，senddata 只能发出一个空包。
     */
    return ;
}

/* 把 fd 挂到 epoll 上，并指明这次监听读还是写 */
void eventadd(int efd, int events, struct myevent_s *ev)
{
    struct epoll_event epv = {0, {0}};
    int op;
    epv.data.ptr = ev;                  // 关键：把结构体指针存进 epoll_event，就绪时能原样取回
    epv.events = ev->events = events;   // 内核侧和结构体侧各记一份，两边保持一致

    if (ev->status == 0) {
        op = EPOLL_CTL_ADD;             // 还不在 epoll 上 -> 新增
        ev->status = 1;
    } else {
        op = EPOLL_CTL_MOD;             // 已经在 epoll 上 -> 修改监听的事件类型
    }

    if (epoll_ctl(efd, op, ev->fd, &epv) < 0)
        printf("event add failed [fd=%d], events[%d]\n", ev->fd, events);
    else
        printf("event add OK [fd=%d], op=%d, events[%#X]\n", ev->fd, op, (unsigned)events);

    return ;
}

/* 回调：客户端 fd 可读 —— 收数据，然后把监听方向改成“可写”，准备回发 */
void recvdata(int fd, int events, void *arg)
{
    struct myevent_s *ev = (struct myevent_s *)arg;
    int len;

    (void)events;   // 回调的签名是统一的，这里用不到 events，加 (void) 避免编译警告

    /* 只收 sizeof(buf)-1 个字节，给下面的 '\0' 留个位置，
       否则收满 4096 字节时 ev->buf[len] 就越界了 */
    len = recv(fd, ev->buf, sizeof(ev->buf) - 1, 0);

    eventdel(g_efd, ev);                                // 先从 epoll 上摘掉，后面按需重新挂

    if (len > 0) {
        ev->len = len;
        ev->buf[len] = '\0';                            // 手动添加字符串结束标记，方便 printf
        printf("C[%d]:%s\n", fd, ev->buf);

        eventset(ev, fd, senddata, ev);                 // 设置该 fd 对应的回调函数为 senddata
        eventadd(g_efd, EPOLLOUT, ev);                  // 将 fd 挂到 epoll 中，监听它的写事件

    } else if (len == 0) {
        close(ev->fd);                                  // recv 返回 0：对端正常关闭连接
        // ev -g_events 两个地址相减，得到的是该元素在数组中的下标
        printf("[fd=%d] pos[%ld], closed\n", fd, ev-g_events);
    } else {
        close(ev->fd);                                  // 出错（比如收到 RST），直接关掉
        printf("recv[fd=%d] error[%d]:%s\n", fd, errno, strerror(errno));
    }

    return ;
}

/* 回调：客户端 fd 可写 —— 把刚收到的数据原样回发，再切回“等读” */
void senddata(int fd, int events, void *arg)
{
    struct myevent_s *ev = (struct myevent_s *)arg;
    int len;

    (void)events;   // 同上，签名统一要求

    /* 这里只调一次 send，没处理“数据只发出去一部分”和 EAGAIN 的情况，数据量小时够用；
       要严谨的话应记录已发送的偏移量，没发完就继续监听 EPOLLOUT */
    len = send(fd, ev->buf, ev->len, 0);

    eventdel(g_efd, ev);                                // 从 epoll 上摘掉

    if (len > 0) {
        printf("send[fd=%d], [%d]%s\n", fd, len, ev->buf);
        eventset(ev, fd, recvdata, ev);                 // 将该 fd 的回调函数改为 recvdata
        eventadd(g_efd, EPOLLIN, ev);                   // 重新挂到 epoll，监听它的读事件
    } else {
        close(ev->fd);                                  // 发送失败，关闭连接
        printf("send[fd=%d] error %s\n", fd, strerror(errno));
    }

    return ;
}

/* 回调：监听 socket 可读 —— 有新连接进来，accept 之后登记到 g_events 里 */
void acceptconn(int lfd, int events, void *arg)
{
    struct sockaddr_in cin;
    socklen_t len = sizeof(cin);
    int cfd, i;

    (void)events;   // 同上，签名统一要求
    (void)arg;

    if ((cfd = accept(lfd, (struct sockaddr *)&cin, &len)) == -1) {
        /* listen fd 是非阻塞的，暂时没有新连接可接时会返回 EAGAIN，属于正常情况，
           所以只在真正出错时才打印，避免刷屏 */
        if (errno != EAGAIN && errno != EINTR)
            printf("%s: accept, %s\n", __func__, strerror(errno));
        return ;
    }

    do {
        /* 在 g_events 的前 MAX_EVENTS 个槽位里找一个空的（status == 0）；
           最后一个是 listen fd 的位置，不能被客户端占用 */
        for(i = 0; i < MAX_EVENTS; ++i)
            if (g_events[i].status == 0)
                break;

        if (i == MAX_EVENTS) {                          // 连接数已达上限
            printf("%s: max connect limit[%d]\n", __func__, MAX_EVENTS);
            close(cfd);                                 // 必须关掉，否则 fd 泄漏
            break;
        }

        /* 先取原有标志位再追加 O_NONBLOCK，避免把 fd 上已有的属性覆盖掉 */
        int flag = fcntl(cfd, F_GETFL, 0);
        if (flag < 0 || fcntl(cfd, F_SETFL, flag | O_NONBLOCK) < 0) {
            printf("%s: fcntl nonblocking failed, %s\n", __func__, strerror(errno));
            close(cfd);
            break;
        }

        /* 难点：给新的 cfd 在事件数组里占个位置，并把回调函数设为 recvdata */
        memset(g_events[i].buf, 0, sizeof(g_events[i].buf));    // 复用旧槽位时先清掉上次残留的数据
        g_events[i].len = 0;
        eventset(&g_events[i], cfd, recvdata, &g_events[i]);
        eventadd(g_efd, EPOLLIN, &g_events[i]);         // 将 cfd 挂到 epoll 上，监听读事件

        printf("new connect [%s:%d][time:%ld], pos[%d]\n",
                inet_ntoa(cin.sin_addr), ntohs(cin.sin_port), g_events[i].last_active, i);

    } while(0);

    return ;
}

/* 创建监听 socket：socket -> bind -> listen，然后当成普通 fd 登记进 epoll */
void initlistensocket(int efd, short port)
{
    struct sockaddr_in sin;

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(lfd, F_SETFL, O_NONBLOCK);    // 监听 fd 也设成非阻塞，accept 才不会卡住

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));   // 重启服务器时可立即复用端口

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;   // 监听本机所有网卡
    sin.sin_port = htons(port);         // 端口要转成网络字节序

    if (bind(lfd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        printf("bind error: %s\n", strerror(errno));
        exit(1);
    }

    if (listen(lfd, 20) < 0) {          // 20：已完成三次握手的连接队列长度上限
        printf("listen error: %s\n", strerror(errno));
        exit(1);
    }

    /*
     * g_events 的长度是 MAX_EVENTS + 1，多出来的最后一个位置专门放 listen fd，
     * 这样监听 socket 和客户端 socket 就能走完全相同的流程（同一个结构体、同一套回调）。
     */
    eventset(&g_events[MAX_EVENTS], lfd, acceptconn, &g_events[MAX_EVENTS]);

    eventadd(efd, EPOLLIN, &g_events[MAX_EVENTS]);  // 监听它的可读事件 = 有新连接到来

    return ;
}

int main(int argc, char *argv[])
{
    unsigned short port = SERV_PORT;

    if (argc == 2)
        port = atoi(argv[1]);       // 也可以在启动时指定端口：./epoll_reactor 8080

    g_efd = epoll_create(MAX_EVENTS + 1);   // 参数现在只要求大于 0，具体值内核已忽略
    if (g_efd < 0) {
        printf("create efd in %s err %s\n", __func__, strerror(errno));
        return -1;
    }

    initlistensocket(g_efd, port);

    struct epoll_event events[MAX_EVENTS + 1];
    printf("server running:port[%d]\n", port);

    int checkpos = 0, i;
    while(1) {
        /* 心跳检测：每轮抽查 100 个连接，超过 60 秒没动静就断开 */
        long now = time(NULL);
        for(i = 0; i < 100; ++i, checkpos++) {
            if (checkpos == MAX_EVENTS)     // 只轮询客户端槽位，跳过最后的 listen fd
                checkpos = 0;
            if (g_events[checkpos].status != 1)
                continue;                   // 空槽位，跳过

            long duration = now - g_events[checkpos].last_active;

            if (duration >= 60) {
                printf("[fd=%d] timeout\n", g_events[checkpos].fd);
                eventdel(g_efd, &g_events[checkpos]);   // 先从 epoll 上摘除，再关掉 fd
                close(g_events[checkpos].fd);
            }
        }

        /* 等待事件，最多阻塞 1 秒；返回的是就绪的 fd 个数 */
        int nfd = epoll_wait(g_efd, events, MAX_EVENTS+1, 1000);
        if (nfd < 0) {
            if (errno == EINTR)     // 被信号打断不算错误，继续等
                continue;
            printf("epoll_wait error, exit\n");
            break;
        }

        /* 难点：遍历就绪事件，取出对应的 myevent_s，调用里面保存的回调函数 */
        for(i = 0; i < nfd; ++i) {
            struct myevent_s *ev = (struct myevent_s *)events[i].data.ptr;

            if ((events[i].events & EPOLLIN) && (ev->events & EPOLLIN))
                ev->call_back(ev->fd, events[i].events, ev->arg);

            if ((events[i].events & EPOLLOUT) && (ev->events & EPOLLOUT))
                ev->call_back(ev->fd, events[i].events, ev->arg);
        }
    }

    return 0;
}
