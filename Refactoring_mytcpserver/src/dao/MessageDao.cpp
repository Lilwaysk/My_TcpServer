#include "dao/MessageDao.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

// 消息持久层：
//   1. 发送时间在接口中使用 Unix 毫秒时间戳，数据库中保存为 DATETIME(3)；
//   2. 用户输入只通过 prepared statement 参数绑定；
//   3. 会话列表按双方最近一条消息聚合，未读数只统计对方发给自己的消息。
namespace {

/**
 * @brief prepared statement 的 RAII 包装。
 */
class Statement {
public:
    explicit Statement(MYSQL* mysql)
        : statement_(mysql_stmt_init(mysql))
    {
    }

    ~Statement()
    {
        if (statement_ != nullptr) {
            mysql_stmt_close(statement_);
        }
    }

    MYSQL_STMT* get() const { return statement_; }

private:
    // statement是用来封装mysql预处理语句句柄MYSQL_STMT*的
    MYSQL_STMT* statement_;
};

/**
 * @brief 把字符串绑定为 MySQL 参数。
 *
 * length 必须由调用方保存，并在 mysql_stmt_execute() 返回前保持有效。
 */
void bindString(MYSQL_BIND* bind, const std::string& value,
                unsigned long* length)
{
    std::memset(bind, 0, sizeof(*bind));
    *length = static_cast<unsigned long>(value.size());
    bind->buffer_type = MYSQL_TYPE_STRING;
    bind->buffer = const_cast<char*>(value.data());
    bind->buffer_length = *length;
    bind->length = length;
}

/**
 * @brief 把无符号 64 位整数绑定为 MySQL BIGINT UNSIGNED。
 */
void bindUInt64(MYSQL_BIND* bind, std::uint64_t* value)
{
    std::memset(bind, 0, sizeof(*bind));
    bind->buffer_type = MYSQL_TYPE_LONGLONG;
    bind->buffer = value;
    bind->is_unsigned = true;
}

/**
 * @brief 把浮点数绑定为 MySQL DOUBLE，用于 Unix 时间转换。
 */
void bindDouble(MYSQL_BIND* bind, double* value)
{
    std::memset(bind, 0, sizeof(*bind));
    bind->buffer_type = MYSQL_TYPE_DOUBLE;
    bind->buffer = value;
}

/**
 * @brief 准备 SQL 并自动计算字符串长度。
 */
bool prepare(MYSQL_STMT* statement, const char* sql)
{
    return mysql_stmt_prepare(
               statement, sql,
               static_cast<unsigned long>(std::strlen(sql))) == 0;
}

/**
 * @brief 把数据库中的 BIGINT UNSIGNED 未读数安全转换为 int。
 */
int toUnreadCount(std::uint64_t value)
{
    const std::uint64_t maxValue =
        static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    if (value > maxValue) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(value);
}

} // namespace

namespace m2 {

/**
 * @brief 插入一条文本消息。
 *
 * @param sendTime Unix 毫秒时间戳。
 * @param msgId 输出 MySQL 生成的消息 id，允许传 nullptr。
 */
InsertMessageResult MessageDao::insertText(
    std::uint64_t fromUid,
    std::uint64_t toUid,
    const std::string& content,
    std::int64_t sendTime,
    std::uint64_t* msgId)
{
    if (db_ == nullptr || db_->handle() == nullptr) {
        return InsertMessageResult::DbError;
    }

    Statement statement(db_->handle());
    if (statement.get() == nullptr) {
        return InsertMessageResult::DbError;
    }

    // sendTime 是毫秒，FROM_UNIXTIME 接收秒数，因此先除以 1000。
    const char* sql =
        "INSERT INTO messages(from_uid, to_uid, content, msg_type, send_time) "
        "VALUES (?, ?, ?, 0, FROM_UNIXTIME(? / 1000.0))";
    if (!prepare(statement.get(), sql)) {
        return InsertMessageResult::DbError;
    }

    MYSQL_BIND parameters[4];
    unsigned long contentLength =
        static_cast<unsigned long>(content.size());
    double sendMillis = static_cast<double>(sendTime);
    bindUInt64(&parameters[0], &fromUid);
    bindUInt64(&parameters[1], &toUid);
    bindString(&parameters[2], content, &contentLength);
    bindDouble(&parameters[3], &sendMillis);

    if (mysql_stmt_bind_param(statement.get(), parameters) != 0
        || mysql_stmt_execute(statement.get()) != 0) {
        return InsertMessageResult::DbError;
    }

    if (msgId != nullptr) {
        *msgId = static_cast<std::uint64_t>(
            mysql_stmt_insert_id(statement.get()));
    }
    return InsertMessageResult::Ok;
}

/**
 * @brief 查询 uid 的会话列表。
 *
 * 一条会话由双方所有方向的最近一条消息表示；最后时间转换成 Unix
 * 毫秒时间戳，未读数只统计 from_uid=peerUid、to_uid=uid 的未读消息。
 */
std::vector<ConversationInfo> MessageDao::listConversations(
    std::uint64_t uid)
{
    std::vector<ConversationInfo> conversations;
    if (db_ == nullptr || db_->handle() == nullptr) {
        return conversations;
    }

    Statement statement(db_->handle());
    if (statement.get() == nullptr) {
        return conversations;
    }

    const char* sql =
        "SELECT latest.peer_uid, u.name, u.nick, u.icon, "
        "m.msg_id, m.content, "
        "UNIX_TIMESTAMP(m.send_time), "
        "(SELECT COUNT(*) FROM messages unread "
        " WHERE unread.from_uid = latest.peer_uid "
        " AND unread.to_uid = ? AND unread.read_flag = 0) "
        "FROM ("
        " SELECT CASE WHEN from_uid = ? THEN to_uid ELSE from_uid END "
        " AS peer_uid, MAX(msg_id) AS last_msg_id "
        " FROM messages "
        " WHERE from_uid = ? OR to_uid = ? "
        " GROUP BY peer_uid"
        ") latest "
        "JOIN messages m ON m.msg_id = latest.last_msg_id "
        "JOIN users u ON u.uid = latest.peer_uid "
        "ORDER BY m.msg_id DESC";
    if (!prepare(statement.get(), sql)) {
        return conversations;
    }

    // 四处 uid 绑定同一个值，SQL 中分别是未读统计、会话对端计算和过滤条件。
    MYSQL_BIND parameters[4];
    for (int i = 0; i < 4; ++i) {
        bindUInt64(&parameters[i], &uid);
    }
    if (mysql_stmt_bind_param(statement.get(), parameters) != 0
        || mysql_stmt_execute(statement.get()) != 0
        || mysql_stmt_store_result(statement.get()) != 0) {
        return conversations;
    }

    char nameBuffer[65] = {0};
    char nickBuffer[65] = {0};
    char iconBuffer[256] = {0};
    unsigned long lengths[4] = {0};
    bool isNull[8] = {false};
    bool errors[8] = {false};

    // content 是 MEDIUMTEXT，先用较小缓冲区读取；遇到截断时按实际长度重取。
    std::vector<char> contentBuffer(4096, '\0');

    MYSQL_BIND result[8];
    std::memset(result, 0, sizeof(result));

    result[0].buffer_type = MYSQL_TYPE_LONGLONG;
    result[0].buffer = nullptr;
    result[0].is_unsigned = true;
    result[0].is_null = &isNull[0];
    result[0].error = &errors[0];

    result[1].buffer_type = MYSQL_TYPE_STRING;
    result[1].buffer = nameBuffer;
    result[1].buffer_length = sizeof(nameBuffer);
    result[1].length = &lengths[0];
    result[1].is_null = &isNull[1];
    result[1].error = &errors[1];

    result[2].buffer_type = MYSQL_TYPE_STRING;
    result[2].buffer = nickBuffer;
    result[2].buffer_length = sizeof(nickBuffer);
    result[2].length = &lengths[1];
    result[2].is_null = &isNull[2];
    result[2].error = &errors[2];

    result[3].buffer_type = MYSQL_TYPE_STRING;
    result[3].buffer = iconBuffer;
    result[3].buffer_length = sizeof(iconBuffer);
    result[3].length = &lengths[2];
    result[3].is_null = &isNull[3];
    result[3].error = &errors[3];

    result[4].buffer_type = MYSQL_TYPE_LONGLONG;
    result[4].buffer = nullptr;
    result[4].is_unsigned = true;
    result[4].is_null = &isNull[4];
    result[4].error = &errors[4];

    result[5].buffer_type = MYSQL_TYPE_STRING;
    result[5].buffer = contentBuffer.data();
    result[5].buffer_length =
        static_cast<unsigned long>(contentBuffer.size());
    result[5].length = &lengths[3];
    result[5].is_null = &isNull[5];
    result[5].error = &errors[5];

    result[6].buffer_type = MYSQL_TYPE_DOUBLE;
    result[6].buffer = nullptr;
    result[6].is_null = &isNull[6];
    result[6].error = &errors[6];

    result[7].buffer_type = MYSQL_TYPE_LONGLONG;
    result[7].buffer = nullptr;
    result[7].is_unsigned = true;
    result[7].is_null = &isNull[7];
    result[7].error = &errors[7];

    std::uint64_t peerUid = 0;
    std::uint64_t lastMsgId = 0;
    double lastSeconds = 0.0;
    std::uint64_t unread = 0;
    result[0].buffer = &peerUid;
    result[4].buffer = &lastMsgId;
    result[6].buffer = &lastSeconds;
    result[7].buffer = &unread;

    if (mysql_stmt_bind_result(statement.get(), result) != 0) {
        return conversations;
    }

    while (true) {
        const int fetchResult = mysql_stmt_fetch(statement.get());
        if (fetchResult == MYSQL_NO_DATA) {
            break;
        }

        // 只有 MEDIUMTEXT 字段允许截断，其余字段截断说明缓冲区或表结构不匹配。
        if (fetchResult != 0 && fetchResult != MYSQL_DATA_TRUNCATED) {
            conversations.clear();
            break;
        }

        if (fetchResult == MYSQL_DATA_TRUNCATED) {
            bool invalidColumn = false;
            for (int i = 0; i < 8; ++i) {
                if (errors[i] && i != 5) {
                    invalidColumn = true;
                    break;
                }
            }
            if (invalidColumn) {
                conversations.clear();
                break;
            }

            contentBuffer.resize(lengths[3] + 1);
            result[5].buffer = contentBuffer.data();
            result[5].buffer_length =
                static_cast<unsigned long>(contentBuffer.size());
            if (mysql_stmt_fetch_column(
                    statement.get(), &result[5], 5, 0) != 0) {
                conversations.clear();
                break;
            }
        }

        ConversationInfo info;
        info.peerUid = peerUid;
        info.name.assign(nameBuffer, lengths[0]);
        info.nick.assign(nickBuffer, lengths[1]);
        info.icon.assign(iconBuffer, lengths[2]);
        info.lastMsgId = lastMsgId;
        info.lastContent.assign(contentBuffer.data(), lengths[3]);
        info.lastTime = static_cast<std::int64_t>(
            std::llround(lastSeconds * 1000.0));
        info.unread = toUnreadCount(unread);
        conversations.push_back(info);
    }

    return conversations;
}

} // namespace m2
