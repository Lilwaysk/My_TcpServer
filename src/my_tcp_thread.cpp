#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>

#include "wrap.h"

#define MAXLINE 8192
#define SERV_PORT 8000

// 定义一个结构体，将地址结构和cfd绑定
struct s_info {
    struct sockaddr_in cliaddr;
    int connfd;
};

void *do_work(void *arg)
{
    int n,i;
    struct s_info *ts = (struct s_info*)arg;
    char buf[MAXLINE];
    char str[INET_ADDRSTRLEN];

    while(1) {
        // 读出客户端发送的信息到buffer
        n = Read(ts->connfd, buf, MAXLINE);
        // n=0时，客户端关闭连接，跳出循环
        if(n == 0) {
            printf("the client %d closed...\n", ts->connfd);
            break;
        }
        printf("received from %s at PORT %d\n",
                inet_ntop(AF_INET, &(*ts).cliaddr.sin_addr, str, sizeof(str)),
                ntohs((*ts).cliaddr.sin_port));

        for(i = 0; i < n; ++i)
            buf[i] = toupper(buf[i]);

        Write(STDOUT_FILENO, buf, n);
        Write(ts->connfd, buf, n);
    }
    Close(ts->connfd);

    return (void *)0;
}

int main(void)
{
    struct sockaddr_in servaddr,cliaddr;
    socklen_t cliaddr_len;
    int listenfd, connfd;
    pthread_t tid;

    // 创建结构体数组,相当于可以存放多个客户端的地址结构
    struct s_info ts[256];
    int i = 0;

    // 创建一个监听的fd
    listenfd = Socket(AF_INET, SOCK_STREAM, 0);

    // 地址结构清零
    bzero(&servaddr, sizeof(servaddr));

    // 选择ipv4协议族-->指定本地任意ip-->指定端口号
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(SERV_PORT);

    // 将监听的fd绑定到服务器上
    Bind(listenfd, (struct sockaddr*)&servaddr, sizeof(servaddr));

    // 设置同一时刻连接服务器上限数
    Listen(listenfd, 128);

    printf("Accepting client connect ...\n");

    while(1) {
        cliaddr_len = sizeof(cliaddr);
        // 阻塞监听客户端链接请求
        connfd = Accept(listenfd, (struct sockaddr *)&cliaddr, &cliaddr_len);
        ts[i].cliaddr = cliaddr;
        ts[i].connfd = connfd;

        pthread_create(&tid, NULL, do_work, (void *)&ts[i]);
        // 子线程分离，防止僵尸线程产生
        pthread_detach(tid);
        ++i;
    }


    return 0;
}
