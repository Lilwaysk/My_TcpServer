#ifndef SERVICE_FRIEND_SERVICE_H
#define SERVICE_FRIEND_SERVICE_H

#include <cstdint>
#include <string>

namespace m2 {

class FriendDao;
class SessionManager;

/**
 * @brief M4 好友业务服务。
 *
 * 职责：
 *   - 解析好友相关 JSON；
 *   - 调用 FriendDao 完成数据库操作；
 *   - 通过 SessionManager 实时推送 1011/1015；
 *   - 生成 1010/1014/1034 响应。
 *
 * SQL 不进入本层，网络发送也不进入 DAO。
 */
class FriendService {
public:
    /**
     * @brief 注入 DAO 和在线会话管理器，不接管二者生命周期。
     */
    FriendService(FriendDao* friendDao,
                  SessionManager* sessions)
        : friendDao_(friendDao),
          sessions_(sessions)
    {
    }

    // 1007 -> 1008。
    void handleSearchUsers(std::uint64_t selfUid,
                           const std::string& requestBody,
                           std::string* responseBody);

    // 1009 -> 1010，并给接收方推送 1011。
    void handleAddFriend(std::uint64_t fromUid,
                         const std::string& requestBody,
                         std::string* responseBody);

    // 1013 -> 1014，并给申请方推送 1015。
    void handleAuthFriend(std::uint64_t currentUid,
                          const std::string& requestBody,
                          std::string* responseBody);

    // 1030 -> 1034。
    void handleListFriends(std::uint64_t uid,
                           std::string* responseBody);

private:
    /**
     * @brief 向申请方推送 1015。
     */
    void pushHandled(std::uint64_t toUid,
                     std::uint64_t applyId,
                     bool accept,
                     std::uint64_t handlerUid);

    // 好友关系持久层，所有好友 SQL 都经过它。
    FriendDao* friendDao_;

    // 在线会话表，用于推送 1011/1015。
    SessionManager* sessions_;
};

} // namespace m2

#endif // SERVICE_FRIEND_SERVICE_H
