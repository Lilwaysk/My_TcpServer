/*
 * select.cpp —— 用 select（I/O 多路复用）实现的单进程并发 TCP 回显服务器。
 *
 * 和另外两版的区别：
 *   my_tcp.cpp         一连接一进程（fork）
 *   my_tcp_thread.cpp  一连接一线程（pthread）
 *   select.cpp         只有一个进程、一个线程，靠 select() 同时监视监听套接字和
 *                      所有已连接的客户端，谁就绪就处理谁
 *
 * 两个关键集合（fd_set 本质是位图，一位对应一个 fd）：
 *   allset  长期保存"目前所有需要监视的 fd"：监听 fd + 每个已连上的客户端
 *   rset    select 的传入传出参数：每轮从 allset 拷一份传进去，
 *           select 返回后它只剩下"这一轮真正就绪"的那些 fd
 * 所以判断某个 fd 能不能读写的依据永远是 FD_ISSET(i, &rset)。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>   /* waitpid：回收子进程。本版不 fork，这个头文件其实用不到，
                           是 my_tcp.cpp 那版留下来的 */
#include <signal.h>     /* sigaction：注册 SIGCHLD 处理函数。同上，本版用不到 */
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h> /* sockaddr_in、htons、INADDR_ANY 等 */
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <math.h>
#include <algorithm>    /* std::max：用来维护"当前最大的 fd 号" */

#include "wrap.h"   /* Socket/Bind/Listen/Accept 等封装：统一错误处理与 EINTR 重试 */
#define SRV_PORT 9999

int main()
{
    int listenfd, connfd;   /* listenfd：监听套接字；connfd：本次新接入的客户端连接 */

    char buf[BUFSIZ];       /* 收发数据的缓冲区，所有客户端共用这一份 */
    int i, j ,n;            /* i：轮询 fd 用；j：转大写时遍历缓冲区；n：read 的返回值 */

    struct sockaddr_in clie_addr, serv_addr;   /* 客户端、服务端地址结构 */

    socklen_t clie_addr_len;                   /* 客户端地址长度，accept 的传入传出参数 */

    /* 第一步：创建监听套接字（SOCK_STREAM = TCP） */
    listenfd = Socket(AF_INET, SOCK_STREAM, 0);

    /* 第二步：设置 SO_REUSEADDR，让服务器 Ctrl+C 之后能立刻重启，
       否则会卡在 TIME_WAIT 上报 "bind error: Address already in use" */
    int opt = 1;

    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 第三步：填服务端地址 → 绑定 → 监听
         sin_port 用 htons 转成网络字节序，sin_addr 用 INADDR_ANY 绑定本机所有网卡；
         bind 之前先 bzero 清零，避免结构体里残留垃圾值 */
    bzero(&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SRV_PORT);
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    Bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    Listen(listenfd, 128);

    // 定义读集合和所有连接集合
    fd_set rset, allset;
    /* 这两个集合的分工是理解 select 的关键：
         allset  长期保存"所有要监视的 fd"（监听 fd + 每个已连上的客户端），自己不参与 select
         rset    每轮从 allset 拷一份交给 select，select 会把它改成"这一轮就绪的 fd"
       fd_set 是位图，一位对应一个 fd 号。 */
    int ret, maxfd = 0;
    // 设置最大文件描述符
    maxfd = listenfd;
    /* 这里记的是"当前用到的最大 fd 号"，select 的第一个参数要传 maxfd+1，
       意思是"扫描 0 ~ maxfd 这一段"。它是扫描范围，不是 fd 的数量，也不是系统上限。 */

    // 清空所有连接集合
    FD_ZERO(&allset);
    /* 必须先清零：fd_set 是栈上的局部变量，不清空的话里面是随机值 */
    // 将监听fd添加到所有连接集合里
    FD_SET(listenfd, &allset);
    /* 此刻还没有任何客户端连进来，集合里只有监听 fd 这一个成员 */

    while(1) {
        // 拷贝一份所有连接集合到读集合中
        rset = allset;
        /* 这一句必须写在循环里：select 会就地修改传进去的集合，把"没就绪"的位清掉，
           所以每一轮都要从 allset 重新拷一份干净的出来 */

        // 用select等待这些 fd 中任意一个变为可读
        ret = select(maxfd+1, &rset, NULL, NULL, NULL);
        /* 四个集合参数依次是：读、写、异常，这里只关心"可读"，其余传 NULL；
           最后一个 NULL 表示不设超时，一直阻塞到有 fd 就绪为止；
           返回值 ret 是这一轮就绪的 fd 总数。 */
        if (ret < 0)
            perr_exit("select error");

        // listenfd 在就绪集合里 = 有新客户端连上来了
        if (FD_ISSET(listenfd, &rset)) {
            /* 监听套接字可读意味着有客户端完成了三次握手，此时 accept 不会阻塞 */
            clie_addr_len = sizeof(clie_addr);
            connfd = Accept(listenfd, (struct sockaddr *)&clie_addr, &clie_addr_len);

            FD_SET(connfd, &allset);
            /* 把新连接加入all集合，下一轮 select 才会开始监视它 */

            maxfd = max(maxfd, connfd); // 自己补的max函数
            /* 新 accept 出来的 fd 一般比之前的大，要同步抬高扫描上限。
               注意 max 住在 std 命名空间里，而本文件没有 using namespace std;，
               所以这里要写成 std::max 才编得过。 */

            if (ret == 1) continue;
            /* 这一轮只有新连接就绪、没有别的客户端发数据，直接回去继续 select */
        }

        // 走到这是有客户端发数据(有读fd)，开始处理发过来的数据
        for(i = listenfd + 1; i < maxfd + 1; ++i) {
            /* 轮询 0 ~ maxfd 之间的每个 fd 号。
               从 listenfd+1 开始，是因为 listenfd 夹在区间中间，而它上面已经处理过了；
               区间里没被用过的 fd 号，FD_ISSET 会返回假，直接跳过。 */
            // 找到在读集合里的那个fd(满足读事件的fd)
            if (FD_ISSET(i, &rset)) {
                n = Read(i, buf, sizeof(buf));
                if (n == 0) {
                    /* read 返回 0 = 对端关闭了连接：关掉 fd，
                       并从 allset 里摘掉，以后不用再监视它 */
                    Close(i);
                    FD_CLR(i, &allset);
                } else if (n == -1)
                    perr_exit("read error");

                /*
                    转成大写回显给客户端，同时打印到服务器终端。
                    ⚠ 注意：这两段写在了 if 的花括号外面（缩进看着在里面，其实不受
                    FD_ISSET 约束），所以每轮循环都会对 i 执行一次 write，
                    而且此时的 n 可能是上一轮残留的值。正确写法是挪进 if 里，
                    并且只在 n > 0 时才写。
                */
                for(j = 0; j < n; ++j)
                    buf[j] = toupper(buf[j]);

                write(i, buf, n);
                write(STDOUT_FILENO, buf, n);
            }

        }
    }

    Close(listenfd);

    return 0;
}
