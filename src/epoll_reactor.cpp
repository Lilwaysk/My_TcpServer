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
#include <signal.h>         /* signal：忽略 SIGPIPE，否则一个断开的客户端就能杀死进程 */
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/tcp.h>    /* TCP_NODELAY：关掉 Nagle，小包交互不用干等 200ms */
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
#define IDLE_TIMEOUT 60000      /* 空闲多久断开连接（毫秒） */
#define MAX_WAIT_MS  1000       /* epoll_wait 单次最长阻塞时间 */

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
    int sendpos;                                        /* 已经发出去多少 —— 新增：输出缓冲区的核心 */
    long expire;                                        /* 定时器到期时刻（毫秒）—— 新增 */
    int hidx;                                           /* 在定时器堆里的下标，-1 表示不在堆里 —— 新增 */
};

int g_efd;                                 // epoll 实例的 fd（epoll_create 的返回值）
struct myevent_s g_events[MAX_EVENTS + 1]; // 所有连接的状态表，最后一个位置留给 listen fd

/* 定时器最小堆：堆里放的是 myevent_s 指针，按 expire 排序 */
static struct myevent_s *g_heap[MAX_EVENTS];
static int g_heap_size = 0;

/* 单调时钟，毫秒。别用 time(NULL)，那个精度只有 1 秒                  */
long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* 最小堆：timers 用                                            */
static void heap_swap(int i, int j)
{
    struct myevent_s *a = g_heap[i];
    struct myevent_s *b = g_heap[j];
    g_heap[i] = b;
    b->hidx = i;
    g_heap[j] = a;
    a->hidx = j;
}

static void heap_up(int i)
{
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (g_heap[parent]->expire <= g_heap[i]->expire)
            break;
        heap_swap(parent, i);
        i = parent;
    }
}

static void heap_down(int i)
{
    for (;;) {
        int l = i * 2 + 1, r = l + 1, small = i;

        if (l < g_heap_size && g_heap[l]->expire < g_heap[small]->expire)
            small = l;
        if (r < g_heap_size && g_heap[r]->expire < g_heap[small]->expire)
            small = r;
        if (small == i)
            break;

        heap_swap(small, i);
        i = small;
    }
}

/* 把连接放进定时器堆（新连接、或者之前没在堆里的时候用） */
static void timer_add(struct myevent_s *ev, long when)
{
    ev->expire = when;
    ev->hidx = g_heap_size;
    g_heap[g_heap_size++] = ev;
    heap_up(ev->hidx);
}

/* 把连接从堆里彻底摘掉 */
static void timer_del(struct myevent_s *ev)
{
    int i = ev->hidx;

    if (i < 0)
        return ;

    ev->hidx = -1;
    g_heap_size--;

    if (i == g_heap_size)           /* 摘的正好是最后一个，收工 */
        return ;

    /* 把最后一个元素挪到空出来的位置，然后上下各调整一次 */
    g_heap[i] = g_heap[g_heap_size];
    g_heap[i]->hidx = i;
    heap_up(i);
    heap_down(i);
}

/* 续期：收到/发出数据时把到期时间往后推，O(log n) */
static void timer_refresh(struct myevent_s *ev, long when)
{
    if (ev->hidx < 0) {
        timer_add(ev, when);
        return ;
    }

    ev->expire = when;
    heap_up(ev->hidx);              /* 变大了可能往下沉，变小了可能往上浮 */
    heap_down(ev->hidx);
}

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

/* 统一的关闭入口：定时器、epoll、fd 三样一起收，避免漏             */
void conn_close(struct myevent_s *ev)
{
    printf("[fd=%d] close\n", ev->fd);

    timer_del(ev);              /* 先从定时器堆摘掉，否则就是野指针 */
    eventdel(g_efd, ev);        /* 再从 epoll 摘掉 */
    close(ev->fd);

    ev->fd = -1;
    ev->len = 0;
    ev->sendpos = 0;            /* status 已经是 0，这个槽位后面会被复用 */
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
        ev->sendpos = 0;                                /* 新的一轮发送，从头开始 */
        ev->buf[len] = '\0';                            // 手动添加字符串结束标记，方便 printf
        printf("C[%d]:%s\n", fd, ev->buf);

        timer_refresh(ev, now_ms() + IDLE_TIMEOUT);   /* 有数据往来就续期 */

        eventdel(g_efd, ev);
        eventset(ev, fd, senddata, ev);                 // 设置该 fd 对应的回调函数为 senddata
        eventadd(g_efd, EPOLLOUT, ev);                  // 将 fd 挂到 epoll 中，监听它的写事件
        return ;
    }

    if (len == 0) {
        conn_close(ev);                                  // recv 返回 0：对端正常关闭连接
        // ev -g_events 两个地址相减，得到的是该元素在数组中的下标
        printf("[fd=%d] pos[%ld], closed\n", fd, ev-g_events);
        return;
    }

    /* len < 0 的三种情况要分开处理，别混在一起 */
    if (errno == EINTR)
        return ;                    /* 被信号打断，不是错误，等下次事件再来 */

    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return ;                    /* 非阻塞 fd 上没有数据可读，属于正常情况 */

    printf("recv[fd=%d] error[%d]:%s\n", fd, errno, strerror(errno));
    conn_close(ev);

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
    len = send(fd, ev->buf + ev->sendpos, ev->len - ev->sendpos, 0);

    if (len > 0) {
        ev->sendpos += len;
        timer_refresh(ev, now_ms() + IDLE_TIMEOUT);

        if (ev->sendpos == ev->len) {           /* 全部发完，切回等读 */
            printf("send[fd=%d] done, [%d]%s\n", fd, ev->len, ev->buf);

            eventdel(g_efd, ev);
            eventset(ev, fd, recvdata, ev);
            eventadd(g_efd, EPOLLIN, ev);
        }
        /*
         * 没发完：什么都不用做。
         * EPOLLOUT 还挂在 epoll 上，等内核发送缓冲区腾出空间，会再通知我们一次。
         */
        return ;
    }

    if (len < 0 && errno == EINTR)
        return ;                    /* 被信号打断，重发即可 */

    /* 发送缓冲区满了，等下一次 EPOLLOUT —— 这里绝对不能 close */
    if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return ;

    printf("send[fd=%d] error %s\n", fd, strerror(errno));
    conn_close(ev);

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

    /*
     * 一次 epoll_wait 期间可能同时到了好几个连接，必须循环 accept 到 EAGAIN。
     * LT 下漏掉只是「处理得慢」，但以后切 ET 就是直接丢连接 —— 现在就写对。
     */
    for (;;) {
        len = sizeof(cin);
        if ((cfd = accept(lfd, (struct sockaddr *)&cin, &len)) == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                printf("%s: accept, %s\n", __func__, strerror(errno));
            break;                      /* 这一轮的新连接已经接完了 */
        }

        /* 连接数已达上限：必须 close，否则 fd 泄漏 */
        for (i = 0; i < MAX_EVENTS; ++i)
            if (g_events[i].status == 0)
                break;

        if (i == MAX_EVENTS) {
            printf("%s: max connect limit[%d]\n", __func__, MAX_EVENTS);
            close(cfd);
            break;
        }

        int flag = fcntl(cfd, F_GETFL, 0);
        if (flag < 0 || fcntl(cfd, F_SETFL, flag | O_NONBLOCK) < 0) {
            printf("%s: fcntl nonblocking failed, %s\n", __func__, strerror(errno));
            close(cfd);
            continue;                   /* 这个连接放弃，后面的还要接 */
        }

        /* 关掉 Nagle：echo 这种小包往返场景，延迟比吞吐重要 */
        int nodelay = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        memset(g_events[i].buf, 0, sizeof(g_events[i].buf));
        g_events[i].len = 0;
        g_events[i].sendpos = 0;
        eventset(&g_events[i], cfd, recvdata, &g_events[i]);
        eventadd(g_efd, EPOLLIN, &g_events[i]);

        /* 新连接进定时器堆：60 秒没有任何往来就会被踢掉 */
        timer_add(&g_events[i], now_ms() + IDLE_TIMEOUT);

        printf("new connect [%s:%d], pos[%d]\n",
                inet_ntoa(cin.sin_addr), ntohs(cin.sin_port), i);
    }

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

/* ------------------------------------------------------------------ */
/* 处理所有到期的定时器                                                */
/* ------------------------------------------------------------------ */
void timer_expire_process(void)
{
    long now = now_ms();

    while (g_heap_size > 0 && g_heap[0]->expire <= now) {
        struct myevent_s *ev = g_heap[0];
        printf("[fd=%d] timeout\n", ev->fd);
        conn_close(ev);         /* conn_close 里会 timer_del，把堆顶弹掉 */
    }
}

int main(int argc, char *argv[])
{
    unsigned short port = SERV_PORT;

    if (argc == 2)
        port = atoi(argv[1]);       // 也可以在启动时指定端口：./epoll_reactor 8080

    /*
     * 第一件保命的事：往已经关闭的连接 send 会触发 SIGPIPE，
     * 默认动作是「杀死进程」—— 一个客户端正常断开就能带走整个服务器。
     */
    signal(SIGPIPE, SIG_IGN);

    /* hidx 必须显式初始化成 -1：0 是合法下标，不能用 0 表示“不在堆里” */
    for (int i = 0; i <= MAX_EVENTS; ++i) {
        g_events[i].status = 0;
        g_events[i].hidx = -1;
        g_events[i].fd = -1;
    }

    g_efd = epoll_create(MAX_EVENTS + 1);   // 参数现在只要求大于 0，具体值内核已忽略
    if (g_efd < 0) {
        printf("create efd in %s err %s\n", __func__, strerror(errno));
        return -1;
    }

    initlistensocket(g_efd, port);

    struct epoll_event events[MAX_EVENTS + 1];
    printf("server running:port[%d]\n", port);

    for (;;) {
        /*
         * 超时时间按堆顶算：最近一个定时器还有多久到期。
         * 没有定时器就等 1 秒 —— 这里不再需要“每轮扫 100 个连接”。
         */
        int timeout_ms = MAX_WAIT_MS;

        if (g_heap_size > 0) {
            long diff = g_heap[0]->expire - now_ms();

            if (diff <= 0)
                timeout_ms = 0;
            else if (diff < MAX_WAIT_MS)
                timeout_ms = (int)diff;
        }

        int nfd = epoll_wait(g_efd, events, MAX_EVENTS + 1, timeout_ms);
        if (nfd < 0) {
            if (errno == EINTR)
                continue;
            printf("epoll_wait error, exit\n");
            break;
        }

        for (int i = 0; i < nfd; ++i) {
            struct myevent_s *ev = (struct myevent_s *)events[i].data.ptr;
            uint32_t re = events[i].events;

            if (ev == NULL)
                continue;

            /*
             * 第二件保命的事：EPOLLERR / EPOLLHUP 是内核无条件上报的，
             * 即使我们没在 eventadd 里注册。原代码只看 EPOLLIN / EPOLLOUT，
             * 对端发 RST 时两个 if 都不匹配 -> 回调不会执行 -> 连接永远不关、fd 泄漏。
             * 这里直接关掉：这两个事件意味着连接已经没救了。
             */
            if (re & (EPOLLERR | EPOLLHUP)) {
                printf("[fd=%d] epollerr/hup, close it\n", ev->fd);
                conn_close(ev);
                continue;
            }

            if ((re & EPOLLIN) && (ev->events & EPOLLIN))
                ev->call_back(ev->fd, re, ev->arg);

            if ((re & EPOLLOUT) && (ev->events & EPOLLOUT))
                ev->call_back(ev->fd, re, ev->arg);
        }

        /* 事件处理完再统一清理超时连接 */
        timer_expire_process();
    }

    return 0;
}
