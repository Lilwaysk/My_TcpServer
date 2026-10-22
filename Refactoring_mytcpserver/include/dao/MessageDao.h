#ifndef DAO_MESSAGE_DAO_H
#define DAO_MESSAGE_DAO_H

#include <cstdint>
#include <string>
#include <vector>

#include "dao/Database.h"

namespace m2 {

// 消息详情
struct MessageInfo {
    std::uint64_t msgId;
    std::uint64_t fromUid;
    std::uint64_t toUid;
    std::string content;
    int msgType;
    std::int64_t sendTime;
};

struct ConversationInfo {
    std::uint64_t peerUid;
    std::string name;
    std::string nick;
    std::string icon;
    std::uint64_t lastMsgId;
    std::string lastContent;
    std::int64_t lastTime;
    int unread;
};

enum class InsertMessageResult {
    Ok,
    DbError
};

class MessageDao {
public:
    explicit MessageDao(Database* db) : db_(db) {}

    InsertMessageResult insertText (
        std::uint64_t fromUid,
        std::uint64_t toUid,
        const std::string& content,
        std::int64_t sendTime,
        std::uint64_t* msgId);

    std::vector<ConversationInfo> listConversations(std::uint64_t uid);
private:
    Database* db_;
};

}


#endif
