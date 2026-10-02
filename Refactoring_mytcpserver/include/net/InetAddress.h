#ifndef NET_INETADDRESS_H
#define NET_INETADDRESS_H

#include <netinet/in.h>
#include <cstdint>
#include <string>

/*
 * sockaddr_in 的封装。
 *
 * 这是项目里第二个允许出现平台特有头文件的地方（第一个是 Poller 管 epoll）。
 * 理由一样：把「结构体初始化、字节序、地址解析」这些琐事关在一个文件里，
 * 上层只看到 ip 字符串和端口号。
 *
 * 顺带解决老代码里的两个手写活儿：
 *   - 非法 IP 的静默失败：老代码用 inet_addr(ip)，出错返回 INADDR_NONE
 *     而 INADDR_NONE 正好等于 255.255.255.255，没法区分；这里用 inet_pton 查返回值。
 *   - htons 的字节序转换，统一在这里做一次。
 */
class InetAddress {
public:
    /* 监听用：port 传 0 让内核挑；loopbackOnly 决定绑 127.0.0.1 还是 INADDR_ANY */
    explicit InetAddress(uint16_t port = 0, bool loopbackOnly = false);

    /* 连接用：ip 是点分十进制字符串 */
    InetAddress(const std::string& ip, uint16_t port);

    explicit InetAddress(const struct sockaddr_in& addr) : addr_(addr) {}

    std::string toIp() const;
    std::string toIpPort() const;
    uint16_t port() const;              /* 主机字节序 */

    const struct sockaddr_in& getSockAddrInet() const { return addr_; }
    void setSockAddrInet(const struct sockaddr_in& addr) { addr_ = addr; }

private:
    struct sockaddr_in addr_;
};

#endif
