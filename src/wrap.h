#ifndef __WRAP_H_
#define __WRAP_H_

/* 头文件自己带上需要的系统头，这样谁 include 它都不会因为顺序问题报
   "unknown type name 'socklen_t'" 之类的怪错。 */
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* wrap.c 是用 C 编译的，从 C++ 调用（my_tcp.cpp）需要告诉编译器别做名字修饰，
   否则链接时会报 undefined reference to `Accept(int, sockaddr*, unsigned int*)`。 */
#ifdef __cplusplus
extern "C" {
#endif

void perr_exit(const char *s);
int Accept(int fd, struct sockaddr *sa, socklen_t *salenptr);
int Bind(int fd, const struct sockaddr *sa, socklen_t salen);
int Connect(int fd, const struct sockaddr *sa, socklen_t salen);
int Listen(int fd, int backlog);
int Socket(int family, int type, int protocol);
ssize_t Read(int fd, void *ptr, size_t nbytes);
ssize_t Write(int fd, const void *ptr, size_t nbytes);
int Close(int fd);
ssize_t Readn(int fd, void *vptr, size_t n);
ssize_t Writen(int fd, const void *vptr, size_t n);
ssize_t my_read(int fd, char *ptr);
ssize_t Readline(int fd, void *vptr, size_t maxlen);
int max(int x, int y);


#ifdef __cplusplus
}
#endif

#endif

