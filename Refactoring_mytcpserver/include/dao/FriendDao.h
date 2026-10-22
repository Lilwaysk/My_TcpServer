#ifndef DAO_FRIEND_DAO_H
#define DAO_FRIEND_DAO_H

#include <cstdint>
#include <string>
#include <vector>

#include "dao/Database.h"

namespace m2 {

// 推送申请时需要的发送方摘要，不包含密码等敏感字段。
struct UserBrief {
    // 用户唯一 id，对应 users.uid。
    std::uint64_t uid;

    // 登录用户名，对应 users.name。
    std::string name;

    // 昵称，对应 users.nick。
    std::string nick;

    // 头像资源地址，对应 users.icon。
    std::string icon;

    // 性别标记，对应 users.sex。
    int sex;

    UserBrief() : uid(0), sex(0) {}
};

// 好友列表中的一条记录。
struct FriendInfo {
    // 好友的 uid。
    std::uint64_t uid;

    // 好友用户名。
    std::string name;

    // 好友昵称。
    std::string nick;

    // 好友头像地址。
    std::string icon;

    // 当前用户为好友设置的备注。
    std::string remark;

    // 好友性别标记。
    int sex;

    FriendInfo() : uid(0), sex(0) {}
};

enum class AddFriendResult {
    Ok,
    UserNotFound,
    AlreadyFriend,
    PendingApplyExists,
    DbError
};

enum class ApplyHandleResult {
    Ok,
    ApplyNotFound,
    NotOwner,
    AlreadyHandled,
    DbError
};

/**
 * @brief M4 好友关系持久层。
 *
 * 所有 SQL 都集中在这个类中。Service 层只负责协议和推送，
 * 不直接拼接或执行 SQL。
 */
class FriendDao {
public:
    /**
     * @brief 绑定数据库连接，不接管 Database 的生命周期。
     */
    explicit FriendDao(Database* db) : db_(db) {}

    /**
     * @brief 按 uid 查询公开用户资料。
     */
    bool findUserById(std::uint64_t uid, UserBrief* out);

    /**
     * @brief 判断 uid1 和 uid2 是否已经是好友。
     */
    bool areFriends(std::uint64_t uid1, std::uint64_t uid2);

    /**
     * @brief 判断 fromUid 是否已向 toUid 提交了待处理申请。
     */
    bool hasPendingApply(std::uint64_t fromUid, std::uint64_t toUid);

    /**
     * @brief 按 UID 精确匹配或用户名模糊搜索用户，最多返回 50 条。
     *
     * keyword 为纯数字时同时按 uid 查询；只返回公开资料字段，不返回密码。
     * selfUid 对应的用户不会出现在结果中。
     */
    std::vector<UserBrief> searchUsers(
        const std::string& keyword,
        std::uint64_t selfUid);

    AddFriendResult createApply(
        std::uint64_t fromUid,
        std::uint64_t toUid,
        const std::string& message,
        std::uint64_t* applyId);

    /**
     * @brief 同意好友申请。
     *
     * 在一个事务中完成：
     *   - 锁定并校验申请；
     *   - 将 status 改为 1；
     *   - 插入两条对称好友记录。
     */
    ApplyHandleResult acceptApply(
        std::uint64_t applyId,
        std::uint64_t currentUid,
        std::uint64_t* fromUid);

    /**
     * @brief 拒绝好友申请，只更新状态为 2。
     */
    ApplyHandleResult rejectApply(
        std::uint64_t applyId,
        std::uint64_t currentUid,
        std::uint64_t* fromUid);

    /**
     * @brief 查询指定用户的好友列表。
     */
    std::vector<FriendInfo> listFriends(std::uint64_t uid);

private:
    // Database 由 main 持有，FriendDao 只保存借用指针。
    Database* db_;
};

} // namespace m2

#endif // DAO_FRIEND_DAO_H
