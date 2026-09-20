/*
 * epoll.cpp —— 用 epoll（Linux 专有的 I/O 多路复用）实现的单进程并发 TCP 回显服务器。
 *
 * 和 select 版的区别：
 *   select.cpp   用 fd_set 位图：每次调用都要把整份 fd 集合从用户态拷进内核，
 *                内核再线性扫描所有 fd 找就绪的；能监视的 fd 数还受
 *                FD_SETSIZE（通常 1024）限制，而且 select 会就地改掉传进去的集合，
 *                所以每轮都得从 allset 重新拷一份
 *   epoll.cpp    只需要把"要监视哪些 fd"注册一次进内核的 epoll 实例，
 *                之后 epoll_wait 直接返回"已经就绪"的 fd 列表，
 *                不用重复传全集，也不用线性扫描。连接数一多优势就很明显。
 *
 * epoll 的三个核心接口：
 *   epoll_create  创建 epoll 实例，返回一个 fd（下面叫 efd）代表这个实例
 *   epoll_ctl     往实例里增 / 删 / 改要监视的 fd 和事件
 *                 （EPOLL_CTL_ADD / EPOLL_CTL_DEL / EPOLL_CTL_MOD）
 *   epoll_wait    阻塞等待，返回就绪事件的个数，就绪的事件被填进数组
 *
 * 内核里的 epoll 实例主要由两部分组成（网上说的"efd 指向红黑树根节点"就是这个结构）：
 *   红黑树    存所有被监视的 fd —— 所以增删改只要 O(log n)，注册一次就一直有效
 *   就绪链表  事件触发时内核回调把对应节点挂进来，epoll_wait 直接从这儿取
 *
 * 和 select 版最关键的一处用法差别：
 *   select 返回后，你得自己遍历所有 fd，用 FD_ISSET 一个个问"你准备好了吗"；
 *   epoll_wait 返回的数组里**只有就绪的 fd**，所以外层循环上界是 nready，
 *   不需要关心那些没动静的 fd。
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/epoll.h>  /* epoll_create / epoll_ctl / epoll_wait 和 struct epoll_event */
#include <errno.h>
#include <ctype.h>      /* toupper：把收到的字符转成大写 */
#include <iostream>     /* 这份代码实际用的是 printf 和封装好的 Write，
                           iostream 没派上用场，是多余的一行（删掉不影响编译） */

#include "wrap.h"       /* Socket/Bind/Listen/Accept/Read/Write/Close/perr_exit 等封装：
                           Socket 这类"失败就没法继续"的调用出错会直接 exit(1)，
                           Read/Write 失败只返回 -1，把决定权留给调用者 */

using namespace std;    /* 同上，本文件没用到 std 里的任何东西 */

#define MAXLINE 8192    /* 读缓冲区的最大长度 */
#define SERV_PORT 8989  /* 监听端口。注意和另外几版都不同：fork 版和 select 版是 9999，
                           pthread 版是 8000，这里是 8989，所以它可以和那些程序同时跑 */

#define OPEN_MAX 5000   /* 名字借用了系统的 OPEN_MAX（单进程能打开的文件数上限），
                           但这里只是拿它当 epoll_wait 的数组容量用：
                           一次最多处理 5000 个就绪事件，跟系统上限没关系 */

int main()
/* 这份服务器不需要命令行参数，所以直接写 int main()。
   原来是 int main(int agrc, char *argv[])：参数名 agrc 是 argc 的笔误，
   而且这两个参数通篇都没用到，会被 -Wextra 报两条 unused parameter 警告。 */
{
    int i, listenfd, connfd, sockfd;
    /* i：遍历就绪事件数组（ep）的下标；
       listenfd：监听套接字，全程只有一个；
       connfd：accept 返回的新客户端连接；
       sockfd：当前正在处理的那个客户端 fd */
    int n, j, num = 0;
    /* n：Read 读到的字节数；
       j：专门给下面"把 buf 里每个字节转成大写"的小循环用；
       num：累计接入的客户端数，只用于打印编号。
       注意这里**不能**再定义一次 i：i 是外层遍历就绪事件用的下标，
       原先本行和上一行都定义了 i，构成重复定义，编译直接报 redeclaration of 'int i'，
       整个文件都编不过。现在把 i 从本行去掉了。 */


    ssize_t nready, efd, res;
    /* nready：本轮就绪的事件个数，也是后面循环的上界；
       efd：epoll 实例的 fd；
       res：epoll_ctl 的返回值。
       严格说 efd 就是个文件描述符编号，声明成 int 更贴切，用 ssize_t 也能跑，只是不严谨。 */
    char buf[MAXLINE], str[INET_ADDRSTRLEN];
    /* buf：收发缓冲区。注意所有客户端共用这一份，因为这里是单进程单线程、
       一次只处理一个 fd，读完立刻写回，不存在两个连接同时用 buf 的情况；
       要是以后改成多线程，这个 buf 就必须挪进每个线程自己的栈里。
       str：存点分十进制的 IP 字符串，INET_ADDRSTRLEN = 16，
       刚好放得下 "255.255.255.255" 加结尾的 '\0'。 */
    socklen_t cli_len;
    /* cli_len 是 accept 的传入传出参数：调用前必须先填成 sizeof(cli_addr)，
       accept 返回后它会被改写成内核实际写入的地址结构长度。 */


    struct sockaddr_in cli_addr, serv_addr;
    /* cli_addr：accept 时内核把对端地址填在这里；
       serv_addr：本机要绑定的地址端口 */
    // tep：epoll_ctl的参数；ep[]：epoll_wait的参数
    struct epoll_event tep, ep[OPEN_MAX];
    /* struct epoll_event 只有两个成员：
         events   事件掩码（EPOLLIN 可读、EPOLLOUT 可写、EPOLLERR 出错……）
         data     联合体。这里用 data.fd 记"这个事件属于哪个 fd"，
                  也可以是 data.ptr 存指针，二选一，看你怎么用
       tep：单个事件的模板，配 epoll_ctl 用；
       ep ：数组，epoll_wait 把本轮就绪的事件填进这里。
       提醒一句：OPEN_MAX = 5000，这个数组在栈上占 5000 × 12 ≈ 60KB，
       跑得动但偏大，实际写 1024 之类的容量就够了。 */


    /* 第一步：创建监听套接字（SOCK_STREAM = TCP，封装失败即 exit） */
    listenfd = Socket(AF_INET, SOCK_STREAM, 0);

    /* 第二步：打开 SO_REUSEADDR，让服务器 Ctrl+C 之后能立刻重启，
       否则会卡在 TIME_WAIT 上，重新启动时报 "bind error: Address already in use" */
    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 第三步：填地址 → bind → listen
         htons 把端口转成网络字节序（主机字节序和网络字节序可能相反，不转打印/连接都会错）；
         htonl(INADDR_ANY) 表示绑定本机所有网卡，这样局域网里别的机器也能连上；
         bind 之前先 bzero 清零，避免结构体里的填充字节残留垃圾值 */
    bzero(&serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERV_PORT);
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    Bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    Listen(listenfd, 20);
    /* 20 是 backlog：已完成三次握手、排队等着 accept 的连接队列长度。
       它不限制服务器能同时保持多少个连接，只影响"还来不及 accept 时能排多少个"。 */


    // 创建epoll模型，efd指向红黑树根节点
    /* epoll_create 创建一个 epoll 实例，返回代表它的 fd（这里存进 efd）。
       参数 size 从 Linux 2.6.8 起就被内核忽略了，只要求它大于 0，
       所以填 OPEN_MAX 只是习惯写法，并**没有**"最多只能监视 5000 个 fd"的作用。
       现在写新代码一般用 epoll_create1(0)，还能顺手带上 EPOLL_CLOEXEC 标志。 */
    efd = epoll_create(OPEN_MAX);
    if (efd == -1)
        perr_exit("epoll_create error");

    // 把lfd设置为读事件
    /* 注释里的 lfd 指的就是上面的 listenfd（listenfd 才是它真正的名字，别被简称绕住）。
       EPOLLIN = 可读。对监听套接字来说，"可读"意味着有客户端完成了三次握手，
       可以去 accept 了；对普通连接来说，"可读"就是对方发来了数据。 */
    tep.events = EPOLLIN;
    tep.data.fd = listenfd;
    // 将lfd及对应的结构体设置到树上，efd可以找到这棵树(红黑树根节点)
    /* EPOLL_CTL_ADD：把 listenfd 和它关心的事件注册进这个 epoll 实例，
       内核在红黑树里建一个节点保存这条记录。注册一次就一直有效，
       以后每轮 epoll_wait 不用再重复传，这也是 epoll 比 select 省的地方。 */
    res = epoll_ctl(efd, EPOLL_CTL_ADD, listenfd, &tep);
    if (res == -1)
        perr_exit("epoll_ctl error");


    /* 服务器主循环：死循环，一直跑 */
    for(;;) {
        // epoll 为 server 阻塞监听事件，ep为struct epoll_event类型的数组，OPEN_MAX为数组容量，-1表示永久阻塞
        /* epoll_wait 的四个参数：
             efd        等哪个 epoll 实例
             ep         就绪事件往哪个数组里填
             OPEN_MAX   数组最多能装多少个（就绪数超过这个值会被截断）
             -1         超时毫秒数；-1 = 永久阻塞，一直等到有事件才返回（0 = 立刻返回，不阻塞）
           返回值 nready 是这一轮真正就绪的事件个数，它就是下面循环的上界。 */
        nready = epoll_wait(efd, ep, OPEN_MAX, -1);
        if (nready == -1)
            perr_exit("epoll_wait error");

        /* 只遍历这 nready 个就绪事件，而不是把所有 fd 都扫一遍
           —— 这正是 epoll_wait 相对 select 的便利之处 */
        for(i = 0; i < nready; ++i) {
            // 不是读事件，则继续循环
            /* 这层括号是必须的：! 的优先级比 & 高，
               写成 !ep[i].events & EPOLLIN 会被解析成 (!ep[i].events) & EPOLLIN，
               只要 events 不为 0，!events 就是 0，判断恒为假，这个守卫就等于白写。
               原来正是这么写的，编译器会给一条 -Wparentheses 警告，现在补上括号修好了。
               这里只处理可读事件，其余事件直接跳过。
               补充一句：EPOLLERR / EPOLLHUP 这类事件就没机会被处理了，
               对端异常断开时留下的连接会泄漏 —— 练习程序这样写够了，
               生产代码一般还要单独判断错误事件并主动关掉连接。 */
            if (!(ep[i].events & EPOLLIN)) continue;
            // 如果满足事件的fd是lfd
            /* data.fd 是当初注册时自己填进去的那个 fd（不是 listenfd 就是某个 connfd）。
               它等于 listenfd，说明这次是"有新客户端连上来了"。 */
            if (ep[i].data.fd == listenfd) {
                cli_len = sizeof(cli_addr);
                connfd = Accept(listenfd, (struct sockaddr*)&cli_addr, &cli_len);
                /* 监听套接字可读 = 已经有连接在队列里了，所以这次 accept 不会阻塞。
                   Accept 是封装函数：出错会打印原因并 exit(1)。 */

                printf("received from %s at PORT %d\n",
                        inet_ntop(AF_INET, &cli_addr.sin_addr, str, sizeof(str)),
                        ntohs(cli_addr.sin_port));
                /* inet_ntop 把 4 字节的二进制 IP 转成 "127.0.0.1" 这种字符串，
                   结果写进 str 并把 str 返回，所以这里可以直接把返回值交给 %s；
                   ntohs 把端口从网络字节序转回主机字节序，不然打印出来是反的。 */
                printf("cfd %d---client %d\n", connfd, ++num);
                /* ++num 是前置自增：先加 1 再用，所以第一个客户端显示的就是 1 */

                tep.events = EPOLLIN;
                tep.data.fd = connfd;
                res = epoll_ctl(efd, EPOLL_CTL_ADD, connfd, &tep);
                /* 新连接也要注册进 epoll，下一轮 epoll_wait 才会开始监视它。
                   这一步漏掉的话，客户端发来的数据永远不会被处理。 */
                if (res == -1)
                    perr_exit("epoll_ctl error");
            } else {
                // 如果不是lfd
                /* 走到这儿说明是某个已经连上的客户端有数据可读了 */
                sockfd = ep[i].data.fd;
                n = Read(sockfd, buf, MAXLINE);
                /* Read 是封装：被信号打断（EINTR）会自动重试，不会把 EINTR 返回给调用者；
                   成功返回读到的字节数，出错返回 -1。 */

                // n=0时，对端关闭
                if (n == 0) {
                    /* read 返回 0 = 对端发来了 FIN，连接正常关闭。
                       顺序是先从 epoll 实例里摘掉（EPOLL_CTL_DEL）再 close(sockfd)。
                       EPOLL_CTL_DEL 的第四个参数在 Linux 2.6.9 以后允许传 NULL，
                       老内核上必须传有效的 epoll_event，这里传 NULL 没问题。 */
                    res = epoll_ctl(efd, EPOLL_CTL_DEL, sockfd, NULL);
                    if (res == -1)
                        perr_exit("epoll_ctl error");
                    Close(sockfd);
                    printf("client[%d] closed connection\n", sockfd);
                } else if (n < 0) {
                    /* 读出错：打印 errno 对应的原因，然后摘掉并关闭这个连接。
                       ⚠ 这里不调用 perr_exit 是对的 —— 单个客户端出错不该拖垮整个服务器。
                       但 select.cpp 里同样的情况用了 perr_exit("read error") 直接退出进程，
                       两版行为不一致，select 版那样写其实更脆弱。 */
                    perror("read n < 0 error: ");
                    res = epoll_ctl(efd, EPOLL_CTL_DEL, sockfd, NULL);
                    Close(sockfd);
                } else {
                    /* n > 0：正常读到数据，转成大写再回显 */
                    /* 这里必须用 j，不能再用 i：i 是外层"遍历就绪事件"的循环变量，
                       一旦被内层改写，外层 for(i = 0; i < nready; ++i) 的节奏就被打乱，
                       后面的就绪事件会被跳过。原来这里写的是 i，现在改用 j 了。 */
                    for(j = 0; j < n; ++j)
                        buf[j] = toupper(buf[j]);

                    Write(STDOUT_FILENO, buf, n);   /* 写到服务器终端，方便肉眼观察 */
                    Write(sockfd, buf, n);          /* 回显给客户端 */
                }
            }
        }
    }

    /* for(;;) 是死循环，正常永远执行不到这两行；
       程序也没有任何主动退出的路径（按 Ctrl+C 是由信号直接杀掉进程的）。
       所以这两行属于"写了但跑不到"的代码，留着不影响运行。 */
    Close(listenfd);
    return 0;
}
