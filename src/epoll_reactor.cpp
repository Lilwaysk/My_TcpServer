#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>   /* waitpid：回收子进程 */
#include <signal.h>     /* sigaction：注册 SIGCHLD 处理函数 */
#include <pthread.h>
#include <sys/epoll.h>  /* epoll_create / epoll_ctl / epoll_wait 和 struct epoll_event */
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h> /* sockaddr_in、htons、INADDR_ANY 等 */
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>

#define MAX_EVENTS 1024
#define BUFLEN 4096
#define SERV_PORT 8080

void recvdata(int fd, int events, void *arg);
void senddata(int fd, int events, void *arg);

// 描述就绪文件描述符相关信息

struct myevent_s {
    int fd;                                             // 要监听的fd
    int events;                                         // 对应的监听事件(读或写)
    void *arg;                                          // 泛型参数
    void (*call_back)(int fd, int events, void *arg);   // 回调函数
    int status;                                         // 是否在监听：1 = 在红黑树上；0 = 不在红黑树上
    char buf[BUFLEN];                                   // 接收缓冲区
    int len;                                            // 收到的字节数长度(应该？)
    long last_active;                                   // 记录每次加入红黑树 g_efd 的时间值
};

int g_efd;                                              // 全局变量，保存epoll_create返回的文件描述符
struct myevent_s g_events[MAX_EVENTS + 1];              // 自定义结构体类型数组，+1--->listen fd

void recvdata(int fd, int events, void *arg)
{

}

void eventset(struct myevent_s *ev, int fd, void (*call_back)(int, int, void *), void *arg)
{
    ev->fd = fd;
    ev->call_back = call_back;
    ev->events = 0;
    ev->arg = arg;
    ev->status = 0;
    memset(ev->buf, 0, sizeof(ev->buf));
    ev->len = 0;
    ev->last_active = time(NULL);

    return ;
}

// 把事件挂树上，设置是读还是写
void eventadd(int efd, int events, struct myevent_s *ev)
{
    struct epoll_event epv = {0, {0}};
    int op;
    epv.data.ptr = ev;
    epv.events = ev->events = events;

    if (ev->status == 0) {
        op = EPOLL_CTL_ADD;
        ev->status = 1;
    }

    if (epoll_ctl(efd, op, ev->fd, &epv) < 0)
        printf("event add failed [fd=%d], events[%s]\n", ev->fd, events);
    else
        printf("event add OK [fd=%d], op=%d, events[%0X]\n", ev->fd, events);

    return ;
}

// 当有文件描述符就绪， epoll返回，回调该函数与客户端建立连接
void acceptconn(int fd, int events, void *arg)
{
    struct sockaddr_in cin;
    socklen_t len = sizeof(cin);
    int cfd, i;

    if ((cfd = accept(lfd, (struct sockaddr *)&cin, &len)) == -1) {
        if (errno != EAGAIN && errno != EINTR) {
            /* 暂不做处理 */
        }
        printf("%s: accept, %s\n", __func__, strerror(errno));
        return ;
    }

    do {
        for(i = 0; i < MAX_EVENTS; ++i)
            if (g_events[i].status == 0)
                break;

        if (i == MAX_EVENTS) {
            printf("%s: max connect limit[%d]\n", __func__, MAX_EVENTS);
            break;
        }

        int flag = 0;
        if ((flag = fcntl(cfd, F_SETFL, O_NONBLOCK)) < 0) {
            printf("%s: fcntl nonblocking failed, %s\n", __func__, sizeof(errno));
            break;
        }

        /*难点*/
        eventset(&g_events[i], cfd, recvdata, &g_events[i]);    // 给新的cfd设置一个myevent_s 结构体，在g_events这个事件数组里面占个位置，回调函数设置为recvdata
        eventadd(g_efd, EPOLLIN, &g_events[i]);                 // 将cfd添加到红黑树g_efd中，监听读事件

    } while(0);

    printf("new connect [%s:%d][time:%ld], pos[%d]\n",
            inet_ntoa(cin.sin_addr), ntohs(cin.sin_port), g_events[i].last_active, i);

    return ;
}

void initlistensocket(int efd, short port)
{
    struct sockaddr_in sin;

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(lfd, F_SETFL, O_NONBLOCK);

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port);

    bind(lfd, (struct sockaddr *)&sin, sizeof(sin));

    listen(lfd, 20);

    /*
    g_events[MAX_EVENTS + 1] 长度为 MAX_EVENTS + 1，最后一个值表示为g_events[MAX_EVENTS]，这里是把listenfd存放到数组最后一个位置
    */
    eventset(&g_events[MAX_EVENTS], lfd, acceptconn, &g_events[MAX_EVENTS]);

    eventadd(efd, EPOLLIN, &g_events[MAX_EVENTS]);

    return ;
}


 int main(int argc, char *argv[])
 {
    unsigned short port = SERV_PORT;

    if (argc == 2)
        port = atoi(argv[1]);

    g_efd  = epoll_create(MAX_EVENTS+1);
    if (g_efd <= 0)
        printf("create efd in %s err %s\n", __func__, strerror(errno));

    initlistensocket(g_efd, port);

    struct epoll_event events[MAX_EVENTS+1];
    printf("server running:port[%d]\n", port);

    int checkpos = 0, i;
    while(1) {
        long now = time(NULL);
        for(i = 0; i < 100; ++i) {
            if (checkpos == MAX_EVENTS)
                checkpos = 0;
            if (g_events[checkpos].status != 1)
                continue;

            long duration = now - g_events[checkpos].last_active;

            if (duration >= 60) {
                close(g_events[checkpos].fd);
                printf("[fd=%d] timeout\n", g_events[checkpos].fd);
                eventdel(g_efd, &g_events[checkpos]);
            }
        }

        int nfd = epoll_wait(g_efd, events, MAX_EVENTS+1, 1000);
        if (nfd < 0) {
            printf("epoll_wait error, exit\n");
            break;
        }

        for(i = 0; i < nfd; ++i) {
            // 难点
            struct myevent_s *ev = (struct myevent_s *)events[i].data.ptr;

            if ((events[i].events & EPOLLIN) && (ev->events & EPOLLIN))
                ev->call_back(ev->fd, events[i].events, ev->arg);

            if ((events[i].events & EPOLLOUT) && (ev->events & EPOLLOUT))
                ev->call_back(ev->fd, events[i].events, ev->arg);
        }
    }

    return 0;
}
