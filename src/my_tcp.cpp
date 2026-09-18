/*
 * my_tcp.cpp —— 基于 fork 的并发 TCP 回显服务器。
 *
 * 并发模型：一个连接一个子进程（process per connection）。
 *   父进程   只做一件事：accept() 取出新连接，然后 fork 出子进程去处理，自己
 *             立刻回去等下一个；
 *   子进程   独占这条连接，循环读取客户端数据、转成大写后写回（回显），直到
 *             对端关闭连接才退出。
 *
 * 这个模型最直观、进程之间互不干扰，代价是每个连接都要占一个进程的内存和
 * PID，连接数上千就撑不住了。要扛高并发得换成 epoll 这类事件驱动模型
 * （nginx、Redis 就是这么做的）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>   /* waitpid：回收子进程 */
#include <signal.h>     /* sigaction：注册 SIGCHLD 处理函数 */
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h> /* sockaddr_in、htons、INADDR_ANY 等 */
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>

#include "wrap.h"   /* Socket/Bind/Listen/Accept 等封装：统一错误处理与 EINTR 重试 */
#define SRV_PORT 9999   /* 监听端口，练习阶段先写死，实际项目应从配置或环境变量读 */

using namespace std;

/*
 * SIGCHLD 处理函数：回收已经退出的子进程。
 *
 * 子进程结束时不会立刻消失，而是变成僵尸进程（zombie）等着父进程收尸：内核
 * 要保留它的退出状态，直到父进程调用 wait()/waitpid()。父进程正忙着 accept()，
 * 没空管这些，所以交给信号处理函数来做。
 *
 * 用 while + WNOHANG 而不是只 waitpid 一次：SIGCHLD 信号不排队，多个子进程同时
 * 退出时父进程可能只收到一个信号，所以要非阻塞地一直收，直到没有可回收的子进程。
 */
void catch_child(int signum)
{
    while(waitpid(0, NULL, WNOHANG) > 0);
    return;
}

int main()
{
    int lfd, cfd, ret;                      /* lfd=监听套接字，cfd=与某个客户端通信的套接字 */
    pid_t pid;                              /* fork 的返回值，用来区分父进程和子进程 */
    struct sockaddr_in srv_addr, clt_addr;  /* 服务端、客户端地址结构 */
    char buf[BUFSIZ];                       /* 子进程收发数据的缓冲区，BUFSIZ 通常为 8192 */
    int i;                                  /* 遍历缓冲区用 */
    socklen_t clt_addr_len;                 /* 客户端地址结构的长度，属于传入传出参数 */

    /*
     * 第一步：填好服务端地址结构
     *   sin_family  IPv4
     *   sin_port    端口号，htons 把主机字节序转成网络字节序（大端）
     *   sin_addr    INADDR_ANY 表示绑定本机所有网卡，不挑具体 IP
     * 先用 bzero 清零，避免结构体里残留未初始化的垃圾值（老教程习惯，
     * 现代写法等价于在声明处直接写 = {0}）。
     */
    // 将地址结构清零
    bzero(&srv_addr, sizeof(srv_addr));

    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(SRV_PORT);
    srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /*
     * 第二步：创建 → 绑定 → 监听
     *   Socket 拿到一个监听用的文件描述符 lfd
     *   Bind   把 lfd 和上面的地址（端口 9999）绑定起来
     *   Listen 把主动套接字变成被动套接字，开始接受连接；参数 128 是内核
     *          全连接队列的长度，即最多同时暂存 128 个已经完成三次握手、
     *          但还没被 accept 取走的连接
     *
     * 注意：这里没有设置 SO_REUSEADDR，所以 Ctrl+C 之后立刻重启常常会报
     * "Address already in use"，得等一分钟左右 TIME_WAIT 过去。生产代码一般
     * 会在 bind 之前先 setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, ...)。
     */
    lfd = Socket(AF_INET, SOCK_STREAM, 0);

    Bind(lfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));

    Listen(lfd, 128);

    clt_addr_len = sizeof(clt_addr);

    /*
     * 第三步：父进程的 accept 循环
     * 父进程从此进入死循环，只做一件事：取出一个新连接，fork 一个子进程
     * 交给它处理，自己马上回去 accept 下一个客户端。
     */
    while(1) {
        // 连接成功就返回一个新的cfd，对应一个新客户端
        cfd = Accept(lfd, (struct sockaddr *)&clt_addr , &clt_addr_len);

        /*
         * 第四步：fork 出子进程处理这条连接
         * fork 之后父子进程各持有一份 cfd 的拷贝，靠返回值区分身份：
         *   返回 0    当前是子进程，break 跳出去执行下面的读写循环
         *   返回 > 0  当前是父进程，返回值就是子进程的 PID；关掉自己这份 cfd
         *             后 continue 回去 accept 下一个客户端
         *   返回 < 0  fork 失败（通常是进程数或内存耗尽），这里直接退出了事
         */
        // fork返回进程id
        pid = fork();
        if (pid < 0)
            perr_exit("fork error");
        else if (pid == 0) {
            /* 子进程不会再 accept，把监听套接字关掉，免得白白占着一个 fd */
            close(lfd);
            break;
        } else {
            /*
             * 父进程注册 SIGCHLD 处理函数，这样子进程一退出就能被回收掉。
             *
             * 注意：sigaction 注册一次就够，放在循环里每 accept 一个连接就重复
             * 注册一遍是白做的（不影响正确性）。更规范的写法是挪到循环之前。
             *
             * sa_flags = 0 表示不加 SA_RESTART，信号到来时阻塞中的 accept 会被
             * 打断返回 EINTR —— 这件事已经被 wrap.c 里的 Accept() 自动重试掉了，
             * 所以这里不需要额外操心。
             */
            struct sigaction act;

            act.sa_handler = catch_child;
            sigemptyset(&act.sa_mask);
            act.sa_flags = 0;

            ret = sigaction(SIGCHLD, &act, NULL);

            /* 这条连接归子进程管了，父进程关掉自己这份 cfd，手上只留 lfd */
            close(cfd);
            continue;
        }

    }

    /*
     * 第五步：子进程处理连接（只有 fork 返回 0 才会走到这里）
     *
     * 循环：读客户端数据 → 全部转成大写 → 写回客户端 → 同时打印到服务器终端。
     *
     * read 的返回值：
     *   > 0  实际读到的字节数
     *   = 0  对端关闭了连接（EOF），收工退出
     *   < 0  出错；被信号打断（EINTR）的情况已经被 wrap.c 的 Read() 重试掉了，
     *        其余错误（比如 ECONNRESET）这里暂时没有处理
     */
    // 处理子进程
    if(pid == 0) {
        for(;;) {
            ret = read(cfd, buf, sizeof(buf));
            if(ret == 0) {
                /* 对端正常关闭连接。exit(1) 是沿用教程写法，严格说这里算正常结束，
                   写成 exit(0) 或 break 更贴切。 */
                close(cfd);
                exit(1);
            }

            /* 转大写：toupper 一次只处理一个字符，所以自己遍历整个缓冲区 */
            for(i = 0; i < ret; ++i)
                buf[i] = toupper(buf[i]);

            /* 把转好的数据写回客户端，完成"回显"…… */
            write(cfd, buf, ret);
            /* ……再往服务器终端打一份，方便观察每个连接在干什么。
               注意：如果客户端已经断开，这里的 write 会触发 SIGPIPE 直接把子进程
               杀掉；生产代码一般会 signal(SIGPIPE, SIG_IGN) 并判断 EPIPE。 */
            write(STDOUT_FILENO, buf, ret);
        }
    }
    return 0;
}
