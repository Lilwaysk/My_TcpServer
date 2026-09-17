/*
 * wrap.c —— 把容易出错、出错后又没法继续跑的系统调用包一层。
 *
 * 返回值约定（很重要，调用时按这个理解）：
 *   - Socket/Bind/Listen/Accept/Connect/Close：失败即致命，打印原因后 exit(1)。
 *     这类错误（端口被占、地址不可用……）在服务器里通常没法恢复。
 *   - Read/Write/Readn/Writen/Readline：失败返回 -1，把决定权留给调用者。
 *     网络对端随时可能断开，这属于正常业务分支，不该直接退出进程。
 *   - 所有函数都会自动重试被信号打断（EINTR）的系统调用，调用者不用关心。
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "wrap.h"

#
void perr_exit(const char *s)
{
    perror(s);
    exit(1);
}

int Socket(int family, int type, int protocol)
{
    int n;

    if ((n = socket(family, type, protocol)) < 0) {
        perr_exit("socket error");
    }
    return n;
}

int Bind(int fd, const struct sockaddr *sa, socklen_t salen)
{
    if (bind(fd, sa, salen) < 0) {
        perr_exit("bind error");
    }
    return 0;
}

int Listen(int fd, int backlog)
{
    if (listen(fd, backlog) < 0) {
        perr_exit("listen error");
    }
    return 0;
}

int Accept(int fd, struct sockaddr *sa, socklen_t *salenptr)
{
    int n;

    /* ECONNABORTED：连接在完成三次握手前被对端放弃，内核要求重新 accept。 */
    do {
        n = accept(fd, sa, salenptr);
    } while (n < 0 && (errno == EINTR || errno == ECONNABORTED));

    if (n < 0) {
        perr_exit("accept error");
    }
    return n;
}

int Connect(int fd, const struct sockaddr *sa, socklen_t salen)
{
    int n;

    do {
        n = connect(fd, sa, salen);
    } while (n < 0 && errno == EINTR);

    /* Linux 上 connect 被信号打断后重试，可能返回 EISCONN（连接其实已经建好），
       这种情况按成功处理，否则会误报错误。 */
    if (n < 0 && errno != EISCONN) {
        perr_exit("connect error");
    }
    return 0;
}

/*
 * Read/Write 只负责“重试 EINTR”，不保证读满/写满，返回值就是系统调用的返回值：
 *   0 = 对端关闭（EOF），-1 = 出错，>0 = 实际读写的字节数（可能少于要求）。
 * 需要“读满 n 字节”或“写完整份数据”时用下面的 Readn/Writen。
 */
ssize_t Read(int fd, void *ptr, size_t nbytes)
{
    ssize_t n;

    do {
        n = read(fd, ptr, nbytes);
    } while (n < 0 && errno == EINTR);

    return n;
}

ssize_t Write(int fd, const void *ptr, size_t nbytes)
{
    ssize_t n;

    do {
        n = write(fd, ptr, nbytes);
    } while (n < 0 && errno == EINTR);

    return n;
}

/*
 * 注意：这里刻意不重试 EINTR。Linux 上 close() 被信号打断时，文件描述符可能
 * 已经被内核释放，重试会去关一个“看起来还是这个号”的 fd——在多线程程序里那个
 * 号可能已经被别的线程复用，于是关掉别人的连接。这是教程代码里常见的一个隐患。
 */
int Close(int fd)
{
    if (close(fd) < 0) {
        perr_exit("close error");
    }
    return 0;
}

/*
 * 读满 n 字节（或读到 EOF/出错才返回），返回值是实际读到的字节数。
 * 返回值 < n 说明对端提前关闭了连接。
 */
ssize_t Readn(int fd, void *vptr, size_t n)
{
    size_t nleft = n;
    char *ptr = vptr;

    while (nleft > 0) {
        ssize_t nread = read(fd, ptr, nleft);

        if (nread < 0) {
            if (errno == EINTR) {
                continue; /* 被信号打断，重来一次，不算错误 */
            }
            return -1;
        }
        if (nread == 0) {
            break; /* 对端关闭连接 */
        }
        nleft -= (size_t)nread;
        ptr += nread;
    }
    return (ssize_t)(n - nleft);
}

/*
 * 写满 n 字节。write 可能只写进去一部分（内核发送缓冲区满），所以要循环；
 * 返回值是实际写进去的字节数，< n 说明中途出错。
 */
ssize_t Writen(int fd, const void *vptr, size_t n)
{
    size_t nleft = n;
    const char *ptr = vptr;

    while (nleft > 0) {
        ssize_t nwritten = write(fd, ptr, nleft);

        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (nwritten == 0) {
            break; /* 正常情况下不会发生 */
        }

        nleft -= (size_t)nwritten;
        ptr += nwritten;
    }
    return (ssize_t)(n - nleft);
}

/*
 * my_read / Readline 是“按行读”的实现：每次都从内核多读一块放进缓冲区，
 * 再一个字节一个字节地交给 Readline，避免每读一个字符就调用一次 read()。
 *
 * 缓冲区用 _Thread_local 声明，每个线程一份。原版教程用的是 static 全局数组，
 * 多个线程同时按行读会互相踩数据；改成线程局部存储后，同样的 API 就是线程安全的了。
 */
#define MY_READ_BUF_SIZE 4096

static _Thread_local char read_buf[MY_READ_BUF_SIZE];
static _Thread_local int read_cnt = 0;
static _Thread_local char *read_ptr = NULL;

/* 从 fd 取一个字节放进 *ptr：返回 1 表示成功，0 表示对端关闭，-1 表示出错。 */
ssize_t my_read(int fd, char *ptr)
{
    if (read_cnt <= 0) {
        ssize_t n;

        do {
            n = read(fd, read_buf, sizeof(read_buf));
        } while (n < 0 && errno == EINTR);

        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            return 0; /* EOF */
        }

        read_cnt = (int)n;
        read_ptr = read_buf;
    }

    read_cnt--;
    *ptr = *read_ptr++;
    return 1;
}

/*
 * 读一行（以 '\n' 结尾），最多读 maxlen-1 个字符，末尾补 '\0'。
 * 返回值是读到的字符数（含 '\n'），0 表示对端已关闭，-1 表示出错。
 * 注意：缓冲区满且没遇到换行时会提前返回，剩下没读完的部分留在内核里，
 * 下一次 Readline 会接着读——所以要写“超长行”必须自己处理这种情况。
 */
ssize_t Readline(int fd, void *vptr, size_t maxlen)
{
    char c;
    char *ptr = vptr;
    size_t nread = 0;

    while (nread + 1 < maxlen) { /* 留一个字节放结尾的 '\0' */
        ssize_t rc = my_read(fd, &c);

        if (rc == 1) {
            *ptr++ = c;
            nread++;
            if (c == '\n') {
                break;
            }
        } else if (rc == 0) {
            break; /* 对端关闭，把已经读到的部分返回 */
        } else {
            return -1;
        }
    }

    *ptr = '\0';
    return (ssize_t)nread;
}
