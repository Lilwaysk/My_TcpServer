#include "service/MessageService.h"

#include <chrono>

#include <nlohmann/json.hpp>

#include "dao/FriendDao.h"
#include "dao/MessageDao.h"
#include "protocol/Codec.h"
#include "protocol/Protocol.h"
#include "session/SessionManager.h"

namespace {

// M5 消息业务错误码。
enum ErrorCode {
    kSuccess = 0,
    kInvalidArgument = 1,
    kDatabaseError = 5,
    kUserNotFound = 7,
    kCannotMessageSelf = 8,
    kNotFriends = 12
};

/**
 * @brief 生成统一错误 JSON。
 */
std::string errorResponse(int error, const std::string& message)
{
    nlohmann::json response;
    response["error"] = error;
    response["message"] = message;
    return response.dump();
}

/**
 * @brief 取得当前 Unix 毫秒时间戳。
 *
 * MessageDao 使用 DATETIME(3)，接口统一传毫秒，避免秒级时间导致同一秒
 * 内的消息显示顺序不精确。
 */
std::int64_t currentUnixMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

namespace m2 {

/**
 * @brief 构造消息服务，只保存借用指针。
 */
MessageService::MessageService(MessageDao* messageDao,
                               FriendDao* friendDao,
                               SessionManager* sessions)
    : messageDao_(messageDao),
      friendDao_(friendDao),
      sessions_(sessions)
{
}

/**
 * @brief 处理 1017 文本消息。
 *
 * 成功顺序：
 *   1. 确认接收方存在；
 *   2. 确认双方是好友；
 *   3. 写入 messages；
 *   4. 返回 1018；
 *   5. 接收方在线时推送 1019。
 *
 * 接收方离线时消息仍在数据库中，后续由 M6 的离线补发流程处理。
 */
void MessageService::handleSendText(
    std::uint64_t fromUid,
    const std::string& requestBody,
    std::string* responseBody)
{
    const nlohmann::json request =
        nlohmann::json::parse(requestBody, nullptr, false);
    if (request.is_discarded() || !request.is_object()
        || !request.contains("to_uid")
        || !request.contains("content")
        || !request.contains("client_msg_id")
        || !request["content"].is_string()
        || !request["client_msg_id"].is_string()
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

    const std::uint64_t toUid =
        static_cast<std::uint64_t>(signedToUid);
    const std::string content =
        request["content"].get<std::string>();
    const std::string clientMsgId =
        request["client_msg_id"].get<std::string>();

    if (toUid == fromUid) {
        *responseBody =
            errorResponse(kCannotMessageSelf, "cannot message yourself");
        return;
    }
    if (content.empty() || content.size() > 4096
        || clientMsgId.empty() || clientMsgId.size() > 64) {
        *responseBody = errorResponse(kInvalidArgument, "invalid message");
        return;
    }

    UserBrief target;
    if (!friendDao_->findUserById(toUid, &target)) {
        *responseBody = errorResponse(kUserNotFound, "user not found");
        return;
    }
    if (!friendDao_->areFriends(fromUid, toUid)) {
        *responseBody = errorResponse(kNotFriends, "not friends");
        return;
    }

    const std::int64_t sendTime = currentUnixMillis();
    std::uint64_t msgId = 0;
    const InsertMessageResult insertResult =
        messageDao_->insertText(
            fromUid, toUid, content, sendTime, &msgId);
    if (insertResult != InsertMessageResult::Ok) {
        *responseBody = errorResponse(kDatabaseError, "database error");
        return;
    }

    // 1018 只返回发送方需要的结果；client_msg_id 用于更新本地乐观气泡。
    nlohmann::json response;
    response["error"] = kSuccess;
    response["msg_id"] = msgId;
    response["client_msg_id"] = clientMsgId;
    response["send_time"] = sendTime;
    *responseBody = response.dump();

    UserBrief fromUser;
    if (!friendDao_->findUserById(fromUid, &fromUser)) {
        return;
    }

    // 接收方离线时 sendTo 返回 false，但消息已经落库，业务仍算成功。
    nlohmann::json push;
    push["msg_id"] = msgId;
    push["from_uid"] = fromUid;
    push["from_name"] = fromUser.name;
    push["from_nick"] = fromUser.nick;
    push["from_icon"] = fromUser.icon;
    push["content"] = content;
    push["msg_type"] = 0;
    push["send_time"] = sendTime;

    sessions_->sendTo(
        toUid,
        proto::Codec::encode(
            proto::ID_NOTIFY_TEXT_CHAT_MSG_REQ,
            0,
            push.dump()));
}

/**
 * @brief 处理 1031 会话列表请求。
 *
 * MessageDao 已按最近消息聚合会话，并计算对方发来的未读数。
 */
void MessageService::handleChatList(
    std::uint64_t uid,
    std::string* responseBody)
{
    const std::vector<ConversationInfo> conversations =
        messageDao_->listConversations(uid);

    nlohmann::json array = nlohmann::json::array();
    for (std::size_t i = 0; i < conversations.size(); ++i) {
        const ConversationInfo& item = conversations[i];

        nlohmann::json conversation;
        conversation["peer_uid"] = item.peerUid;
        conversation["name"] = item.name;
        conversation["nick"] = item.nick;
        conversation["icon"] = item.icon;
        conversation["last_msg_id"] = item.lastMsgId;
        conversation["last_content"] = item.lastContent;
        conversation["last_time"] = item.lastTime;
        conversation["unread"] = item.unread;
        array.push_back(conversation);
    }

    nlohmann::json response;
    response["error"] = kSuccess;
    response["conversations"] = array;
    *responseBody = response.dump();
}

} // namespace m2
