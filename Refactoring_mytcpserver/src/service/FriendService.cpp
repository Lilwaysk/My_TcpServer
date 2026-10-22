#include "service/FriendService.h"

#include <nlohmann/json.hpp>

#include "dao/FriendDao.h"
#include "protocol/Codec.h"
#include "protocol/Protocol.h"
#include "session/SessionManager.h"

// 本文件只处理好友业务：校验 JSON、调用 FriendDao、生成响应和推送。
// 所有 SQL 都在 FriendDao 中，网络帧编码统一交给 proto::Codec。
namespace {

// 好友业务对客户端返回的错误码。
enum ErrorCode {
    kSuccess = 0,
    kInvalidArgument = 1,
    kDatabaseError = 5,
    kUserNotFound = 7,
    kCannotAddSelf = 8,
    kAlreadyFriend = 9,
    kApplyInvalid = 10,
    kNotApplyOwner = 11
};

std::string errorResponse(int error, const std::string& message)
{
    // 所有好友业务错误都使用统一 JSON 结构，客户端可集中处理。
    nlohmann::json response;
    response["error"] = error;
    response["message"] = message;
    return response.dump();
}

} // namespace

namespace m2 {

/**
 * @brief 处理 1007 用户搜索。
 *
 * 搜索只返回公开资料，不包含密码；当前登录用户不会出现在自己的搜索结果中。
 */
void FriendService::handleSearchUsers(
    std::uint64_t selfUid,
    const std::string& requestBody,
    std::string* responseBody)
{
    // 使用 non-throwing parse，畸形 JSON 通过 error 返回，不让服务端退出。
    const nlohmann::json request =
        nlohmann::json::parse(requestBody, nullptr, false);
    if (request.is_discarded() || !request.is_object()
        || !request.contains("keyword")
        || !request["keyword"].is_string()) {
        *responseBody = errorResponse(kInvalidArgument, "invalid json");
        return;
    }

    const std::string keyword =
        request["keyword"].get<std::string>();
    if (keyword.empty() || keyword.size() > 64) {
        *responseBody = errorResponse(kInvalidArgument, "invalid keyword");
        return;
    }

    // DAO 使用 prepared statement 和 LIKE 参数，避免 SQL 注入。
    const std::vector<UserBrief> users =
        friendDao_->searchUsers(keyword, selfUid);

    // 只构造客户端需要的公开字段。
    nlohmann::json array = nlohmann::json::array();
    for (std::size_t i = 0; i < users.size(); ++i) {
        nlohmann::json item;
        item["uid"] = users[i].uid;
        item["name"] = users[i].name;
        item["nick"] = users[i].nick;
        item["icon"] = users[i].icon;
        item["sex"] = users[i].sex;
        array.push_back(item);
    }

    nlohmann::json response;
    response["error"] = kSuccess;
    response["users"] = array;
    *responseBody = response.dump();
}

/**
 * @brief 处理 1009 发起好友申请。
 *
 * 数据库写入成功后生成 1010 响应，并向目标用户推送 1011。
 * 目标离线时推送失败，但申请已经持久化，因此业务仍返回成功。
 */
void FriendService::handleAddFriend(
    std::uint64_t fromUid,
    const std::string& requestBody,
    std::string* responseBody)
{
    const nlohmann::json request =
        nlohmann::json::parse(requestBody, nullptr, false);
    if (request.is_discarded() || !request.is_object()
        || !request.contains("to_uid")
        || (!request["to_uid"].is_number_integer()
            && !request["to_uid"].is_number_unsigned())) {
        *responseBody = errorResponse(kInvalidArgument, "invalid json");
        return;
    }

    const std::int64_t signedToUid =
        request["to_uid"].get<std::int64_t>();
    if (signedToUid <= 0) {
        *responseBody = errorResponse(kInvalidArgument, "invalid to_uid");
        return;
    }

    // JSON 正数可能被 nlohmann 表示为 unsigned，也可能表示为 signed，
    // 因此先取 int64 再检查范围，最后转成 uid 使用的 uint64。
    const std::uint64_t toUid =
        static_cast<std::uint64_t>(signedToUid);
    const std::string message =
        request.value("msg", std::string("你好"));

    if (message.size() > 255) {
        *responseBody = errorResponse(kInvalidArgument, "message too long");
        return;
    }
    if (toUid == fromUid) {
        *responseBody = errorResponse(kCannotAddSelf, "cannot add yourself");
        return;
    }

    // 基础业务检查：目标存在、不能重复加好友、不能重复提交待处理申请。
    UserBrief target;
    if (!friendDao_->findUserById(toUid, &target)) {
        *responseBody = errorResponse(kUserNotFound, "user not found");
        return;
    }
    if (friendDao_->areFriends(fromUid, toUid)) {
        *responseBody = errorResponse(kAlreadyFriend, "already friends");
        return;
    }
    if (friendDao_->hasPendingApply(fromUid, toUid)) {
        *responseBody =
            errorResponse(kApplyInvalid, "apply already pending");
        return;
    }

    // 插入申请并获得自增 apply_id，后续 1011/1013 都靠它定位申请。
    std::uint64_t applyId = 0;
    const AddFriendResult result =
        friendDao_->createApply(fromUid, toUid, message, &applyId);
    if (result != AddFriendResult::Ok) {
        *responseBody = errorResponse(kDatabaseError, "database error");
        return;
    }

    // 推送中只带发送方公开资料，不带密码等敏感字段。
    UserBrief fromUser;
    if (friendDao_->findUserById(fromUid, &fromUser)) {
        nlohmann::json push;
        push["apply_id"] = applyId;
        push["from_uid"] = fromUid;
        push["name"] = fromUser.name;
        push["nick"] = fromUser.nick;
        push["icon"] = fromUser.icon;
        push["msg"] = message;

        // 目标离线时 sendTo 返回 false，但申请已经持久化，业务仍成功。
        sessions_->sendTo(
            toUid,
            proto::Codec::encode(
                proto::ID_NOTIFY_ADD_FRIEND_REQ,
                0,
                push.dump()));
    }

    nlohmann::json response;
    response["error"] = kSuccess;
    response["apply_id"] = applyId;
    *responseBody = response.dump();
}

/**
 * @brief 处理 1013 同意或拒绝好友申请。
 *
 * DAO 内部负责事务和权限校验；本函数只在事务成功后推送 1015。
 */
void FriendService::handleAuthFriend(
    std::uint64_t currentUid,
    const std::string& requestBody,
    std::string* responseBody)
{
    const nlohmann::json request =
        nlohmann::json::parse(requestBody, nullptr, false);
    if (request.is_discarded() || !request.is_object()
        || !request.contains("apply_id")
        || !request.contains("accept")
        || !request["accept"].is_boolean()
        || (!request["apply_id"].is_number_integer()
            && !request["apply_id"].is_number_unsigned())) {
        *responseBody = errorResponse(kInvalidArgument, "invalid json");
        return;
    }

    const std::int64_t signedApplyId =
        request["apply_id"].get<std::int64_t>();
    if (signedApplyId <= 0) {
        *responseBody = errorResponse(kInvalidArgument, "invalid apply_id");
        return;
    }

    const std::uint64_t applyId =
        static_cast<std::uint64_t>(signedApplyId);
    const bool accept = request["accept"].get<bool>();

    // accept=true 进入同意事务，false 进入拒绝事务。
    std::uint64_t fromUid = 0;
    const ApplyHandleResult result = accept
        ? friendDao_->acceptApply(applyId, currentUid, &fromUid)
        : friendDao_->rejectApply(applyId, currentUid, &fromUid);

    if (result == ApplyHandleResult::ApplyNotFound) {
        *responseBody = errorResponse(kApplyInvalid, "apply not found");
        return;
    }
    if (result == ApplyHandleResult::NotOwner) {
        *responseBody = errorResponse(kNotApplyOwner, "permission denied");
        return;
    }
    if (result == ApplyHandleResult::AlreadyHandled) {
        *responseBody =
            errorResponse(kApplyInvalid, "apply already handled");
        return;
    }
    if (result != ApplyHandleResult::Ok) {
        *responseBody = errorResponse(kDatabaseError, "database error");
        return;
    }

    // 数据库提交成功后再推送，避免对方看到未落库的处理结果。
    pushHandled(fromUid, applyId, accept, currentUid);

    nlohmann::json response;
    response["error"] = kSuccess;
    response["apply_id"] = applyId;
    response["accept"] = accept;
    *responseBody = response.dump();
}

/**
 * @brief 向申请方推送 1015。
 *
 * toUid 是原申请发起人，handlerUid 是同意/拒绝该申请的人。
 */
void FriendService::pushHandled(std::uint64_t toUid,
                                std::uint64_t applyId,
                                bool accept,
                                std::uint64_t handlerUid)
{
    UserBrief handler;
    if (!friendDao_->findUserById(handlerUid, &handler)) {
        return;
    }

    // seq=0 表示服务端主动推送，不对应某个请求。
    nlohmann::json push;
    push["apply_id"] = applyId;
    push["to_uid"] = handlerUid;
    push["accept"] = accept;
    push["name"] = handler.name;
    push["nick"] = handler.nick;

    sessions_->sendTo(
        toUid,
        proto::Codec::encode(
            proto::ID_NOTIFY_AUTH_FRIEND_REQ,
            0,
            push.dump()));
}

/**
 * @brief 处理 1030 好友列表请求。
 *
 * FriendDao 已过滤当前用户并 JOIN users 取得公开资料；
 * 本函数只负责把 vector<FriendInfo> 转成 1034 所需的 JSON。
 */
void FriendService::handleListFriends(
    std::uint64_t uid,
    std::string* responseBody)
{
    const std::vector<FriendInfo> friends =
        friendDao_->listFriends(uid);

    nlohmann::json array = nlohmann::json::array();
    for (std::size_t i = 0; i < friends.size(); ++i) {
        nlohmann::json item;
        item["uid"] = friends[i].uid;
        item["name"] = friends[i].name;
        item["nick"] = friends[i].nick;
        item["icon"] = friends[i].icon;
        item["sex"] = friends[i].sex;
        item["remark"] = friends[i].remark;
        array.push_back(item);
    }

    nlohmann::json response;
    response["error"] = kSuccess;
    response["friends"] = array;
    *responseBody = response.dump();
}

} // namespace m2
