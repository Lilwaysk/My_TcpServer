#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>

#include "wrap.h"
#define SRV_PORT 9999

using namespace std;

int main()
{
    int lfd, cfd, ret;
    pid_t pid;
    struct sockaddr_in srv_addr, clt_addr;
    char buf[BUFSIZ];
    int i;
    socklen_t clt_addr_len;

    // 将地址结构清零
    bzero(&srv_addr, sizeof(srv_addr));

    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(SRV_PORT);
    srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    lfd = Socket(AF_INET, SOCK_STREAM, 0);

    Bind(lfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));

    Listen(lfd, 128);

    clt_addr_len = sizeof(clt_addr);

    while(1) {
        // 连接成功就返回一个新的cfd，对应一个新客户端
        cfd = Accept(lfd, (struct sockaddr *)&clt_addr , &clt_addr_len);

        // fork返回进程id
        pid = fork();
        if (pid < 0)
            perr_exit("fork error");
        else if (pid == 0) {
            close(lfd);
            break;
        } else {
            close(cfd);
            continue;
        }

    }

    // 处理子进程
    if(pid == 0) {
        for(;;) {
            ret = read(cfd, buf, sizeof(buf));
            if(ret == 0) {
                close(cfd);
                exit(1);
            }

            for(i = 0; i < ret; ++i)
                buf[i] = toupper(buf[i]);
            write(cfd, buf, ret);
            write(STDOUT_FILENO, buf, ret);
        }
    }
    return 0;
}
