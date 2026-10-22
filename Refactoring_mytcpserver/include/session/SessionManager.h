#ifndef SESSION_SESSION_MANAGER_H
#define SESSION_SESSION_MANAGER_H

#include <cstdint>
#include <map>
#include <string>

#include "net/TcpConnection.h"

namespace m2 {

/**
 * @brief 维护“已登录 uid -> TCP 连接”的在线会话表。
 *
 * M4 的实时推送依赖这张表：
 *   1. 1004 登录成功后调用 bind()；
 *   2. 好友申请、申请处理结果通过 sendTo() 推送；
 *   3. 连接断开时调用 remove()，避免向失效连接发送消息。
 *
 * 当前服务端使用单 Reactor（io_threads=0），因此这里暂不加锁。
 * 如果以后启用多个 IO 线程，需要给两个 map 加互斥锁，或统一把操作
 * 投递到主 EventLoop 执行。
 */
class SessionManager {
public:
    /**
     * @brief 绑定 uid 和连接。
     *
     * 同一个 uid 再次登录时会覆盖旧连接。当前版本尚未实现踢旧连接，
     * 只保证后续推送发送到最新绑定的连接。
     */
    void bind(std::uint64_t uid, const TcpConnectionPtr& conn);

    /**
     * @brief 连接断开时清理在线状态。
     */
    void remove(const TcpConnectionPtr& conn);

    /**
     * @brief 向在线用户发送一条已经编码好的完整帧。
     * @return 用户在线且连接有效时返回 true。
     */
    bool sendTo(std::uint64_t uid, const std::string& frame);

    /**
     * @brief 判断 uid 当前是否在线。
     */
    bool isOnline(std::uint64_t uid) const;

    /**
     * @brief 根据连接查找当前登录 uid。
     * @return 未登录时返回 0。
     */
    std::uint64_t uidOf(const TcpConnectionPtr& conn) const;

private:
    // uid -> 连接：向某个用户推送消息时使用。
    std::map<std::uint64_t, TcpConnectionPtr> _online;

    // 连接名 -> uid：连接断开时反查并清理在线状态。
    std::map<std::string, std::uint64_t> _connectionToUid;
};

} // namespace m2

#endif // SESSION_SESSION_MANAGER_H
