#include "dao/FriendDao.h"

#include <cstdlib>
#include <cstring>

// M4 好友关系持久层。
// 约定：
//   1. 所有 SQL 都使用 prepared statement，用户输入只通过 MYSQL_BIND 绑定；
//   2. 同意申请必须在同一事务中更新 friend_apply 并插入双向 friends；
//   3. 拒绝申请只更新状态，不创建好友关系。
namespace {

/**
 * @brief prepared statement 的 RAII 包装。
 */
class Statement {
public:
    /**
     * @brief 创建 prepared statement 句柄。
     */
    explicit Statement(MYSQL* mysql)
        : statement_(mysql_stmt_init(mysql))
    {
    }

    /**
     * @brief 自动关闭 prepared statement，避免异常路径泄漏。
     */
    ~Statement()
    {
        if (statement_ != nullptr) {
            mysql_stmt_close(statement_);
        }
    }

    /**
     * @brief 返回底层 MYSQL_STMT 句柄。
     */
    MYSQL_STMT* get() const { return statement_; }

private:
    // MySQL prepared statement 句柄。
    MYSQL_STMT* statement_;
};

/**
 * @brief 把 std::string 绑定为 MySQL 字符串参数。
 */
void bindString(MYSQL_BIND* bind, const std::string& value,
                unsigned long* length)
{
    // buffer 指向调用方字符串内存，因此参数必须在 mysql_stmt_execute()
    // 返回前一直有效。*length 也由调用方保存生命周期。
    std::memset(bind, 0, sizeof(*bind));
    *length = static_cast<unsigned long>(value.size());
    bind->buffer_type = MYSQL_TYPE_STRING;
    bind->buffer = const_cast<char*>(value.data());
    bind->buffer_length = *length;
    bind->length = length;
}

/**
 * @brief 把 uint64_t 绑定为 MySQL BIGINT UNSIGNED 参数。
 */
void bindUInt64(MYSQL_BIND* bind, std::uint64_t* value)
{
    // uid 使用 MySQL BIGINT UNSIGNED，必须显式设置 is_unsigned。
    std::memset(bind, 0, sizeof(*bind));
    bind->buffer_type = MYSQL_TYPE_LONGLONG;
    bind->buffer = value;
    bind->is_unsigned = true;
}

/**
 * @brief 统一的 mysql_stmt_prepare 封装。
 */
bool prepare(MYSQL_STMT* statement, const char* sql)
{
    // 统一准备 SQL，并自动计算字符串长度。
    return mysql_stmt_prepare(
               statement, sql,
               static_cast<unsigned long>(std::strlen(sql))) == 0;
}

/**
 * @brief 回滚当前事务。
 */
bool rollback(MYSQL* mysql)
{
    // 事务失败统一回滚，避免申请状态更新了但好友关系没有写入。
    return mysql_query(mysql, "ROLLBACK") == 0;
}

} // namespace

namespace m2 {

/**
 * @brief 按 uid 查询公开用户资料。
 *
 * 用于 1011/1015 推送时填充发送方名称、昵称和头像。
 */
bool FriendDao::findUserById(std::uint64_t uid, UserBrief* out)
{
    if (db_ == nullptr || db_->handle() == nullptr || out == nullptr) {
        return false;
    }

    Statement statement(db_->handle());
    if (statement.get() == nullptr) {
        return false;
    }

    const char* sql =
        "SELECT uid, name, nick, icon, sex "
        "FROM users WHERE uid = ? LIMIT 1";
    if (!prepare(statement.get(), sql)) {
        return false;
    }

    MYSQL_BIND parameter;
    bindUInt64(&parameter, &uid);
    if (mysql_stmt_bind_param(statement.get(), &parameter) != 0
        || mysql_stmt_execute(statement.get()) != 0
        || mysql_stmt_store_result(statement.get()) != 0) {
        return false;
    }

    // 表字段上限为 name/nick 64、icon 255，缓冲区额外留 1 字节 '\0'。
    char nameBuffer[65] = {0};
    char nickBuffer[65] = {0};
    char iconBuffer[256] = {0};
    unsigned long lengths[3] = {0};
    bool isNull[5] = {false};
    bool errors[5] = {false};

    MYSQL_BIND result[5];
    std::memset(result, 0, sizeof(result));
    result[0].buffer_type = MYSQL_TYPE_LONGLONG;
    result[0].buffer = &out->uid;
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
    result[4].buffer_type = MYSQL_TYPE_LONG;
    result[4].buffer = &out->sex;
    result[4].is_null = &isNull[4];
    result[4].error = &errors[4];

    if (mysql_stmt_bind_result(statement.get(), result) != 0
        || mysql_stmt_fetch(statement.get()) != 0) {
        return false;
    }

    // 使用 MySQL 返回的实际长度构造字符串，避免复制缓冲区中的残留字符。
    out->name.assign(nameBuffer, lengths[0]);
    out->nick.assign(nickBuffer, lengths[1]);
    out->icon.assign(iconBuffer, lengths[2]);
    return true;
}

/**
 * @brief 查询两个用户之间是否已经存在好友关系。
 */
bool FriendDao::areFriends(std::uint64_t uid1, std::uint64_t uid2)
{
    // 用单行查询快速判断两个用户之间是否已有好友关系。
    if (db_ == nullptr || db_->handle() == nullptr) {
        return false;
    }

    Statement statement(db_->handle());
    if (statement.get() == nullptr
        || !prepare(statement.get(),
                    "SELECT 1 FROM friends "
                    "WHERE uid = ? AND friend_uid = ? LIMIT 1")) {
        return false;
    }

    MYSQL_BIND parameters[2];
    bindUInt64(&parameters[0], &uid1);
    bindUInt64(&parameters[1], &uid2);
    if (mysql_stmt_bind_param(statement.get(), parameters) != 0
        || mysql_stmt_execute(statement.get()) != 0
        || mysql_stmt_store_result(statement.get()) != 0) {
        return false;
    }

    return mysql_stmt_fetch(statement.get()) == 0;
}

/**
 * @brief 查询 fromUid 是否已经向 toUid 发过待处理申请。
 */
bool FriendDao::hasPendingApply(std::uint64_t fromUid,
                                std::uint64_t toUid)
{
    // 只检查 status=0 的申请，已同意或已拒绝的历史申请允许忽略。
    if (db_ == nullptr || db_->handle() == nullptr) {
        return false;
    }

    Statement statement(db_->handle());
    if (statement.get() == nullptr
        || !prepare(statement.get(),
                    "SELECT id FROM friend_apply "
                    "WHERE from_uid = ? AND to_uid = ? "
                    "AND status = 0 LIMIT 1")) {
        return false;
    }

    MYSQL_BIND parameters[2];
    bindUInt64(&parameters[0], &fromUid);
    bindUInt64(&parameters[1], &toUid);
    if (mysql_stmt_bind_param(statement.get(), parameters) != 0
        || mysql_stmt_execute(statement.get()) != 0
        || mysql_stmt_store_result(statement.get()) != 0) {
        return false;
    }

    return mysql_stmt_fetch(statement.get()) == 0;
}

/**
 * @brief 按 UID 精确匹配或用户名模糊搜索其他用户。
 *
 * keyword 为纯数字时同时作为 uid 查询；否则只按用户名查询。
 * 返回数量由 SQL 的 LIMIT 控制，结果不包含密码字段。
 */
std::vector<UserBrief> FriendDao::searchUsers(
    const std::string& keyword,
    std::uint64_t selfUid)
{
    std::vector<UserBrief> users;
    if (db_ == nullptr || db_->handle() == nullptr) {
        return users;
    }

    // “添加好友”输入 UID 时，客户端传入的 keyword 是纯数字。
    std::uint64_t uidKeyword = 0;
    bool numericKeyword = !keyword.empty();
    for (std::size_t i = 0; i < keyword.size(); ++i) {
        if (keyword[i] < '0' || keyword[i] > '9') {
            numericKeyword = false;
            break;
        }
    }
    if (numericKeyword) {
        uidKeyword = static_cast<std::uint64_t>(
            std::strtoull(keyword.c_str(), nullptr, 10));
    }

    // 使用 prepared statement，既避免注入又排除当前用户自己。
    Statement statement(db_->handle());
    if (statement.get() == nullptr
        || !prepare(statement.get(),
                    "SELECT uid, name, nick, icon, sex "
                    "FROM users "
                    "WHERE uid <> ? AND (uid = ? OR name LIKE ?) "
                    "ORDER BY name LIMIT 50")) {
        return users;
    }

    // 通配符由服务端拼接，keyword 本身仍然作为 prepared statement 参数。
    const std::string pattern = "%" + keyword + "%";
    MYSQL_BIND parameters[3];
    unsigned long patternLength =
        static_cast<unsigned long>(pattern.size());
    bindUInt64(&parameters[0], &selfUid);
    bindUInt64(&parameters[1], &uidKeyword);
    bindString(&parameters[2], pattern, &patternLength);
    if (mysql_stmt_bind_param(statement.get(), parameters) != 0
        || mysql_stmt_execute(statement.get()) != 0
        || mysql_stmt_store_result(statement.get()) != 0) {
        return users;
    }

    char nameBuffer[65] = {0};
    char nickBuffer[65] = {0};
    char iconBuffer[256] = {0};
    unsigned long lengths[3] = {0};
    bool isNull[5] = {false};
    bool errors[5] = {false};

    MYSQL_BIND result[5];
    std::memset(result, 0, sizeof(result));
    result[0].buffer_type = MYSQL_TYPE_LONGLONG;
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
    result[4].buffer_type = MYSQL_TYPE_LONG;
    result[4].is_null = &isNull[4];
    result[4].error = &errors[4];

    std::uint64_t foundUid = 0;
    int sex = 0;
    result[0].buffer = &foundUid;
    result[4].buffer = &sex;

    if (mysql_stmt_bind_result(statement.get(), result) != 0) {
        return users;
    }

    // 最多由 SQL 限制 50 条，逐行取出公开资料。
    while (true) {
        const int fetchResult = mysql_stmt_fetch(statement.get());
        if (fetchResult == MYSQL_NO_DATA) {
            break;
        }
        if (fetchResult != 0) {
            users.clear();
            break;
        }

        UserBrief user;
        user.uid = foundUid;
        user.name.assign(nameBuffer, lengths[0]);
        user.nick.assign(nickBuffer, lengths[1]);
        user.icon.assign(iconBuffer, lengths[2]);
        user.sex = sex;
        users.push_back(user);
    }
    return users;
}

/**
 * @brief 插入一条好友申请并返回自增 apply_id。
 */
AddFriendResult FriendDao::createApply(
    std::uint64_t fromUid,
    std::uint64_t toUid,
    const std::string& message,
    std::uint64_t* applyId)
{
    // 插入一条待处理申请；调用方负责提前检查用户、好友和重复申请。
    if (db_ == nullptr || db_->handle() == nullptr) {
        return AddFriendResult::DbError;
    }

    Statement statement(db_->handle());
    if (statement.get() == nullptr
        || !prepare(statement.get(),
                    "INSERT INTO friend_apply(from_uid, to_uid, msg) "
                    "VALUES (?, ?, ?)")) {
        return AddFriendResult::DbError;
    }

    MYSQL_BIND parameters[3];
    unsigned long messageLength =
        static_cast<unsigned long>(message.size());
    bindUInt64(&parameters[0], &fromUid);
    bindUInt64(&parameters[1], &toUid);
    bindString(&parameters[2], message, &messageLength);

    if (mysql_stmt_bind_param(statement.get(), parameters) != 0
        || mysql_stmt_execute(statement.get()) != 0) {
        return AddFriendResult::DbError;
    }

    // 自增 id 供 1010 响应和 1011 推送定位申请记录。
    if (applyId != nullptr) {
        *applyId = static_cast<std::uint64_t>(
            mysql_stmt_insert_id(statement.get()));
    }
    return AddFriendResult::Ok;
}

/**
 * @brief 同意好友申请并创建双向好友关系。
 *
 * 整个过程在一个事务中完成，失败时回滚。
 */
ApplyHandleResult FriendDao::acceptApply(
    std::uint64_t applyId,
    std::uint64_t currentUid,
    std::uint64_t* fromUid)
{
    // 使用事务保证“申请状态变为已同意”和“双方好友记录写入”原子完成。
    MYSQL* mysql = db_ == nullptr ? nullptr : db_->handle();
    if (mysql == nullptr || fromUid == nullptr) {
        return ApplyHandleResult::DbError;
    }
    if (mysql_query(mysql, "START TRANSACTION") != 0) {
        return ApplyHandleResult::DbError;
    }

    // FOR UPDATE 锁定当前申请，防止两个客户端并发处理同一条申请。
    Statement selectStatement(mysql);
    if (selectStatement.get() == nullptr
        || !prepare(selectStatement.get(),
                    "SELECT from_uid, to_uid, status "
                    "FROM friend_apply WHERE id = ? FOR UPDATE")) {
        rollback(mysql);
        return ApplyHandleResult::DbError;
    }

    std::uint64_t selectedFromUid = 0;
    std::uint64_t selectedToUid = 0;
    int status = 0;
    MYSQL_BIND selectParameters[1];
    bindUInt64(&selectParameters[0], &applyId);
    if (mysql_stmt_bind_param(selectStatement.get(), selectParameters) != 0
        || mysql_stmt_execute(selectStatement.get()) != 0
        || mysql_stmt_store_result(selectStatement.get()) != 0) {
        rollback(mysql);
        return ApplyHandleResult::DbError;
    }

    bool isNull[3] = {false};
    bool errors[3] = {false};
    MYSQL_BIND result[3];
    std::memset(result, 0, sizeof(result));
    result[0].buffer_type = MYSQL_TYPE_LONGLONG;
    result[0].buffer = &selectedFromUid;
    result[0].is_unsigned = true;
    result[0].is_null = &isNull[0];
    result[0].error = &errors[0];
    result[1].buffer_type = MYSQL_TYPE_LONGLONG;
    result[1].buffer = &selectedToUid;
    result[1].is_unsigned = true;
    result[1].is_null = &isNull[1];
    result[1].error = &errors[1];
    result[2].buffer_type = MYSQL_TYPE_LONG;
    result[2].buffer = &status;
    result[2].is_null = &isNull[2];
    result[2].error = &errors[2];

    if (mysql_stmt_bind_result(selectStatement.get(), result) != 0) {
        rollback(mysql);
        return ApplyHandleResult::DbError;
    }

    const int fetchResult = mysql_stmt_fetch(selectStatement.get());
    if (fetchResult == MYSQL_NO_DATA) {
        rollback(mysql);
        return ApplyHandleResult::ApplyNotFound;
    }
    if (fetchResult != 0) {
        rollback(mysql);
        return ApplyHandleResult::DbError;
    }
    // 只有申请接收方本人有权同意，不能由申请发起方代替。
    if (selectedToUid != currentUid) {
        rollback(mysql);
        return ApplyHandleResult::NotOwner;
    }
    // 已经同意或拒绝的申请不能重复处理。
    if (status != 0) {
        rollback(mysql);
        return ApplyHandleResult::AlreadyHandled;
    }

    // 先把申请状态改为 accepted，再写入对称好友关系。
    Statement updateStatement(mysql);
    if (updateStatement.get() == nullptr
        || !prepare(updateStatement.get(),
                    "UPDATE friend_apply SET status = 1 "
                    "WHERE id = ? AND status = 0")) {
        rollback(mysql);
        return ApplyHandleResult::DbError;
    }

    MYSQL_BIND updateParameters[1];
    bindUInt64(&updateParameters[0], &applyId);
    if (mysql_stmt_bind_param(updateStatement.get(), updateParameters) != 0
        || mysql_stmt_execute(updateStatement.get()) != 0
        || mysql_stmt_affected_rows(updateStatement.get()) != 1) {
        rollback(mysql);
        return ApplyHandleResult::AlreadyHandled;
    }

    // 插入两条好友关系：A->B 和 B->A。
    // 使用 INSERT IGNORE 防止极端重复请求导致事务失败。
    Statement insertStatement(mysql);
    if (insertStatement.get() == nullptr
        || !prepare(insertStatement.get(),
                    "INSERT IGNORE INTO friends(uid, friend_uid) "
                    "VALUES (?, ?), (?, ?)")) {
        rollback(mysql);
        return ApplyHandleResult::DbError;
    }

    std::uint64_t pair[4] = {
        selectedFromUid,
        selectedToUid,
        selectedToUid,
        selectedFromUid
    };
    MYSQL_BIND insertParameters[4];
    for (int i = 0; i < 4; ++i) {
        bindUInt64(&insertParameters[i], &pair[i]);
    }
    if (mysql_stmt_bind_param(insertStatement.get(), insertParameters) != 0
        || mysql_stmt_execute(insertStatement.get()) != 0) {
        rollback(mysql);
        return ApplyHandleResult::DbError;
    }

    // 只有提交成功后才返回 Ok，Service 层随后推送 1015。
    if (mysql_query(mysql, "COMMIT") != 0) {
        rollback(mysql);
        return ApplyHandleResult::DbError;
    }

    *fromUid = selectedFromUid;
    return ApplyHandleResult::Ok;
}

/**
 * @brief 拒绝好友申请。
 *
 * 只更新 friend_apply.status，不写入 friends 表。
 */
ApplyHandleResult FriendDao::rejectApply(
    std::uint64_t applyId,
    std::uint64_t currentUid,
    std::uint64_t* fromUid)
{
    // 拒绝同样先锁定申请，确认操作人、状态后再更新为 rejected。
    MYSQL* mysql = db_ == nullptr ? nullptr : db_->handle();
    if (mysql == nullptr || fromUid == nullptr) {
        return ApplyHandleResult::DbError;
    }
    if (mysql_query(mysql, "START TRANSACTION") != 0) {
        return ApplyHandleResult::DbError;
    }

    Statement selectStatement(mysql);
    if (selectStatement.get() == nullptr
        || !prepare(selectStatement.get(),
                    "SELECT from_uid, to_uid, status "
                    "FROM friend_apply WHERE id = ? FOR UPDATE")) {
        rollback(mysql);
        return ApplyHandleResult::DbError;
    }

    std::uint64_t selectedFromUid = 0;
    std::uint64_t selectedToUid = 0;
    int status = 0;
    MYSQL_BIND selectParameters[1];
    bindUInt64(&selectParameters[0], &applyId);
    if (mysql_stmt_bind_param(selectStatement.get(), selectParameters) != 0
        || mysql_stmt_execute(selectStatement.get()) != 0
        || mysql_stmt_store_result(selectStatement.get()) != 0) {
        rollback(mysql);
        return ApplyHandleResult::DbError;
    }

    bool isNull[3] = {false};
    bool errors[3] = {false};
    MYSQL_BIND result[3];
    std::memset(result, 0, sizeof(result));
    result[0].buffer_type = MYSQL_TYPE_LONGLONG;
    result[0].buffer = &selectedFromUid;
    result[0].is_unsigned = true;
    result[0].is_null = &isNull[0];
    result[0].error = &errors[0];
    result[1].buffer_type = MYSQL_TYPE_LONGLONG;
    result[1].buffer = &selectedToUid;
    result[1].is_unsigned = true;
    result[1].is_null = &isNull[1];
    result[1].error = &errors[1];
    result[2].buffer_type = MYSQL_TYPE_LONG;
    result[2].buffer = &status;
    result[2].is_null = &isNull[2];
    result[2].error = &errors[2];

    if (mysql_stmt_bind_result(selectStatement.get(), result) != 0) {
        rollback(mysql);
        return ApplyHandleResult::DbError;
    }

    const int fetchResult = mysql_stmt_fetch(selectStatement.get());
    if (fetchResult == MYSQL_NO_DATA) {
        rollback(mysql);
        return ApplyHandleResult::ApplyNotFound;
    }
    if (fetchResult != 0) {
        rollback(mysql);
        return ApplyHandleResult::DbError;
    }
    // 只有申请接收方本人可以拒绝。
    if (selectedToUid != currentUid) {
        rollback(mysql);
        return ApplyHandleResult::NotOwner;
    }
    if (status != 0) {
        rollback(mysql);
        return ApplyHandleResult::AlreadyHandled;
    }

    // 只更新 friend_apply，不写 friends。
    Statement updateStatement(mysql);
    if (updateStatement.get() == nullptr
        || !prepare(updateStatement.get(),
                    "UPDATE friend_apply SET status = 2 "
                    "WHERE id = ? AND status = 0")) {
        rollback(mysql);
        return ApplyHandleResult::DbError;
    }

    MYSQL_BIND updateParameters[1];
    bindUInt64(&updateParameters[0], &applyId);
    if (mysql_stmt_bind_param(updateStatement.get(), updateParameters) != 0
        || mysql_stmt_execute(updateStatement.get()) != 0
        || mysql_stmt_affected_rows(updateStatement.get()) != 1) {
        rollback(mysql);
        return ApplyHandleResult::AlreadyHandled;
    }

    if (mysql_query(mysql, "COMMIT") != 0) {
        rollback(mysql);
        return ApplyHandleResult::DbError;
    }

    *fromUid = selectedFromUid;
    return ApplyHandleResult::Ok;
}

/**
 * @brief 查询 uid 的好友列表。
 */
std::vector<FriendInfo> FriendDao::listFriends(std::uint64_t uid)
{
    // friends 保存双向记录，因此这里只按 uid 查询自己的一侧。
    std::vector<FriendInfo> friends;
    if (db_ == nullptr || db_->handle() == nullptr) {
        return friends;
    }

    Statement statement(db_->handle());
    if (statement.get() == nullptr
        || !prepare(statement.get(),
                    "SELECT u.uid, u.name, u.nick, u.icon, "
                    "u.sex, f.remark "
                    "FROM friends f "
                    "JOIN users u ON u.uid = f.friend_uid "
                    "WHERE f.uid = ? "
                    "ORDER BY u.name")) {
        return friends;
    }

    MYSQL_BIND parameter;
    bindUInt64(&parameter, &uid);
    if (mysql_stmt_bind_param(statement.get(), &parameter) != 0
        || mysql_stmt_execute(statement.get()) != 0
        || mysql_stmt_store_result(statement.get()) != 0) {
        return friends;
    }

    // 每列对应一个固定缓冲区，循环 fetch 时复用。
    char nameBuffer[65] = {0};
    char nickBuffer[65] = {0};
    char iconBuffer[256] = {0};
    char remarkBuffer[65] = {0};
    unsigned long lengths[4] = {0};
    bool isNull[6] = {false};
    bool errors[6] = {false};

    MYSQL_BIND result[6];
    std::memset(result, 0, sizeof(result));
    result[0].buffer_type = MYSQL_TYPE_LONGLONG;
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
    result[4].buffer_type = MYSQL_TYPE_LONG;
    result[4].is_null = &isNull[4];
    result[4].error = &errors[4];
    result[5].buffer_type = MYSQL_TYPE_STRING;
    result[5].buffer = remarkBuffer;
    result[5].buffer_length = sizeof(remarkBuffer);
    result[5].length = &lengths[3];
    result[5].is_null = &isNull[5];
    result[5].error = &errors[5];

    std::uint64_t friendUid = 0;
    int sex = 0;
    result[0].buffer = &friendUid;
    result[4].buffer = &sex;

    if (mysql_stmt_bind_result(statement.get(), result) != 0) {
        return friends;
    }

    // 一条好友记录对应一个 FriendInfo。
    while (true) {
        const int fetchResult = mysql_stmt_fetch(statement.get());
        if (fetchResult == MYSQL_NO_DATA) {
            break;
        }
        if (fetchResult != 0) {
            friends.clear();
            break;
        }

        FriendInfo info;
        info.uid = friendUid;
        info.name.assign(nameBuffer, lengths[0]);
        info.nick.assign(nickBuffer, lengths[1]);
        info.icon.assign(iconBuffer, lengths[2]);
        info.sex = sex;
        info.remark.assign(remarkBuffer, lengths[3]);
        friends.push_back(info);
    }
    return friends;
}

} // namespace m2
