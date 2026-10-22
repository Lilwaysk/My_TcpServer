#ifndef SERVICE_MESSAGE_SERVICE_H
#define SERVICE_MESSAGE_SERVICE_H

#include <cstdint>
#include <string>

namespace m2 {

class MessageDao;
class FriendDao;
class SessionManager;

class MessageService {
public:
    /**
     * @brief 注入消息 DAO、好友 DAO 和在线会话管理器。
     */
    MessageService(MessageDao* messageDao,
                   FriendDao* friendDao,
                   SessionManager* sessions);

    /**
     * @brief 处理 1017 文本消息：校验好友、落库、返回 1018、推送 1019。
     */
    void handleSendText(std::uint64_t fromUid,
                        const std::string& requestBody,
                        std::string* responseBody);

    /**
     * @brief 处理 1031 会话列表请求并生成 1035 响应。
     */
    void handleChatList(std::uint64_t uid,
                        std::string* responseBody);

private:
    // 消息持久层。
    MessageDao* messageDao_;

    // 好友关系校验和发送方公开资料查询。
    FriendDao* friendDao_;

    // 在线会话表，用于推送 1019。
    SessionManager* sessions_;
};

} // namespace m2

#endif // SERVICE_MESSAGE_SERVICE_H
