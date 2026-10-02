#include "net/InetAddress.h"

#include <arpa/inet.h>
#include <cstring>

/*
 * ============ InetAddress 实现清单（README 第四节「第 5 步」）============
 *
 * 1) InetAddress(uint16_t port, bool loopbackOnly)
 *      ::memset(&addr_, 0, sizeof addr_);
 *      addr_.sin_family = AF_INET;
 *      addr_.sin_addr.s_addr = loopbackOnly ? ::htonl(INADDR_LOOPBACK) : ::htonl(INADDR_ANY);
 *      addr_.sin_port = ::htons(port);
 *    loopbackOnly 的用途：本地测试时不想被外部连进来，绑 127.0.0.1 而不是 0.0.0.0。
 *
 * 2) InetAddress(const std::string& ip, uint16_t port)
 *      用 ::inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr)，
 *      返回值必须是 1（0 表示格式不对，-1 表示地址族不支持）。
 *      老代码的 addr.sin_addr.s_addr = inet_addr(ip) 遇到非法地址会静默返回
 *      INADDR_NONE（正好等于 255.255.255.255），根本查不出来。
 *
 * 3) toIp()     —— ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof buf)
 * 4) toIpPort() —— toIp() + ":" + port()，日志和连接名里都要用
 * 5) port()     —— ::ntohs(addr_.sin_port)，返回主机字节序
 *
 * 所有字节序转换都关在这个文件里，上层永远只看到「人话」格式。
 */

// InetAddress::InetAddress(uint16_t port, bool loopbackOnly) { /* ... */ }
// InetAddress::InetAddress(const std::string& ip, uint16_t port) { /* ... */ }
// std::string InetAddress::toIp() const { /* ... */ }
// std::string InetAddress::toIpPort() const { /* ... */ }
// uint16_t InetAddress::port() const { /* ... */ }
