#include "session/SessionManager.h"

namespace m2 {

/**
 * @brief 建立 uid 与连接的双向映射。
 *
 * 同一 uid 再次登录时会覆盖旧连接；当前版本尚未实现踢旧连接逻辑。
 */
void SessionManager::bind(std::uint64_t uid,
                          const TcpConnectionPtr& conn)
{
    // 正向表用于按 uid 推送消息，反向表用于连接断开时反查 uid。
    _online[uid] = conn;
    _connectionToUid[conn->name()] = uid;
}

/**
 * @brief 连接断开时清理 uid 与连接映射。
 */
void SessionManager::remove(const TcpConnectionPtr& conn)
{
    std::map<std::string, std::uint64_t>::iterator it =
        _connectionToUid.find(conn->name());
    if (it == _connectionToUid.end()) {
        return;
    }

    // 两张表必须同步删除，避免留下无法发送消息的僵尸连接。
    _online.erase(it->second);
    _connectionToUid.erase(it);
}

/**
 * @brief 向在线 uid 推送一条完整协议帧。
 * @return 用户离线、连接已关闭时返回 false。
 */
bool SessionManager::sendTo(std::uint64_t uid,
                            const std::string& frame)
{
    std::map<std::uint64_t, TcpConnectionPtr>::iterator it =
        _online.find(uid);
    if (it == _online.end() || !it->second->connected()) {
        return false;
    }

    // send() 内部会把写入投递到该连接所属的 EventLoop。
    it->second->send(frame);
    return true;
}

/**
 * @brief 查询 uid 是否在在线表中。
 */
bool SessionManager::isOnline(std::uint64_t uid) const
{
    return _online.find(uid) != _online.end();
}

/**
 * @brief 根据连接反查登录 uid。
 * @return 未登录连接返回 0。
 */
std::uint64_t SessionManager::uidOf(
    const TcpConnectionPtr& conn) const
{
    // 未登录或已经断开的连接没有 uid，用 0 表示。
    std::map<std::string, std::uint64_t>::const_iterator it =
        _connectionToUid.find(conn->name());
    return it == _connectionToUid.end() ? 0 : it->second;
}

} // namespace m2
