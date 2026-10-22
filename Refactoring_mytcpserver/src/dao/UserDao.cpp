#include "dao/UserDao.h"

#include <cstring>

#include <mysql/mysqld_error.h>

// 本文件是最小 M2 版本的用户 DAO：
//   - 注册时把明文密码写入 users.password；
//   - 登录时按用户名读取用户及密码；
//   - SQL 全部使用 prepared statement，避免手工拼接用户输入。
// 注意：明文密码只适用于当前学习阶段，不能用于正式环境。

namespace {

/**
 * @brief MYSQL_STMT 的 RAII 包装。
 *
 * prepared statement 必须调用 mysql_stmt_close 释放。这里把关闭动作放进
 * 析构函数，确保函数提前 return 时也不会泄漏。
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

    // 取得底层句柄，供 prepare/bind/execute 等 MySQL C API 使用。
    MYSQL_STMT* get() const { return statement_; }

private:
    // mysql_stmt 就是 MySQL 的预处理语句句柄，用于安全高效执行带参数的 SQL
    MYSQL_STMT* statement_;
};

/**
 * @brief 把一个 std::string 绑定成 MySQL 字符串参数。
 *
 * MYSQL_BIND 必须清零，否则可能残留随机标志。length 指向调用方保存的
 * 字符串长度变量，该变量必须一直有效到 mysql_stmt_execute 返回。
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

} // namespace

namespace m2 {

/**
 * @brief 向 users 表插入一个新用户。
 *
 * 不先执行 SELECT 检查用户名，而是直接 INSERT，并依赖 name 上的唯一索引
 * 保证并发情况下也不会产生重复账号。
 *
 * @param name 用户名，对应 users.name。
 * @param password 明文密码，对应 users.password。
 * @param nick 昵称，对应 users.nick。
 * @param uid 输出参数；成功时写入 MySQL 生成的自增 uid。
 * @return Ok 表示成功，DuplicateName 表示用户名已存在，
 *         DbError 表示连接、SQL 或绑定失败。
 */
DbResult UserDao::create(const std::string& name,
                         const std::string& password,
                         const std::string& nick,
                         std::uint64_t* uid)
{
    // 数据库连接必须已经由 Database::connect 初始化成功。
    if (db_ == nullptr || db_->handle() == nullptr) {
        return DbResult::DbError;
    }

    // Statement 析构时会自动释放 prepared statement。
    Statement statement(db_->handle());
    if (statement.get() == nullptr) {
        return DbResult::DbError;
    }

    // password 在 MySQL 中可能被识别为关键字，因此使用反引号转义。
    const char* sql =
        "INSERT INTO users(name, `password`, nick) VALUES (?, ?, ?)";
    if (mysql_stmt_prepare(statement.get(), sql,
                           static_cast<unsigned long>(std::strlen(sql))) != 0) {
        return DbResult::DbError;
    }

    // 三个 ? 分别绑定 name、password、nick。
    MYSQL_BIND parameters[3];
    unsigned long lengths[3];
    bindString(&parameters[0], name, &lengths[0]);
    bindString(&parameters[1], password, &lengths[1]);
    bindString(&parameters[2], nick, &lengths[2]);

    if (mysql_stmt_bind_param(statement.get(), parameters) != 0
        || mysql_stmt_execute(statement.get()) != 0) {
        // MySQL 错误码 1062 表示违反唯一键，即用户名已存在。
        if (mysql_stmt_errno(statement.get()) == ER_DUP_ENTRY) {
            return DbResult::DuplicateName;
        }
        return DbResult::DbError;
    }

    // 成功插入后读取 AUTO_INCREMENT 生成的 uid。
    if (uid != nullptr) {
        *uid = static_cast<std::uint64_t>(
            mysql_stmt_insert_id(statement.get()));
    }
    return DbResult::Ok;
}

/**
 * @brief 按用户名查询用户。
 *
 * 登录时由 UserService 调用，随后比较查询出的明文密码。
 *
 * @param name 要查询的用户名。
 * @param out 输出参数，成功时填充 uid/name/password/nick/icon。
 * @return Ok 表示找到用户，NotFound 表示没有匹配记录，
 *         DbError 表示参数、连接或 SQL 执行失败。
 */
DbResult UserDao::findByName(const std::string& name, UserRow* out)
{
    if (db_ == nullptr || db_->handle() == nullptr || out == nullptr) {
        return DbResult::DbError;
    }

    Statement statement(db_->handle());
    if (statement.get() == nullptr) {
        return DbResult::DbError;
    }

    // LIMIT 1 保证最多返回一条记录；name 本身也有唯一索引。
    const char* sql =
        "SELECT uid, name, `password`, nick, icon "
        "FROM users WHERE name = ? LIMIT 1";
    if (mysql_stmt_prepare(statement.get(), sql,
                           static_cast<unsigned long>(std::strlen(sql))) != 0) {
        return DbResult::DbError;
    }

    // 绑定查询参数。
    MYSQL_BIND parameter;
    unsigned long nameLength = static_cast<unsigned long>(name.size());
    bindString(&parameter, name, &nameLength);
    if (mysql_stmt_bind_param(statement.get(), &parameter) != 0
        || mysql_stmt_execute(statement.get()) != 0
        || mysql_stmt_store_result(statement.get()) != 0) {
        return DbResult::DbError;
    }

    // 为每一列准备固定大小的接收缓冲区。
    // 长度与表字段上限一致，并额外留 1 字节给字符串结尾 '\0'。
    char nameBuffer[65] = {0};
    char passwordBuffer[65] = {0};
    char nickBuffer[65] = {0};
    char iconBuffer[256] = {0};
    unsigned long lengths[5] = {0};
    bool isNull[5] = {false};
    bool errors[5] = {false};

    // result[0] 对应 uid，其余四个对应 SELECT 中的四个字符串列。
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
    result[1].length = &lengths[1];
    result[1].is_null = &isNull[1];
    result[1].error = &errors[1];
    result[2].buffer_type = MYSQL_TYPE_STRING;
    result[2].buffer = passwordBuffer;
    result[2].buffer_length = sizeof(passwordBuffer);
    result[2].length = &lengths[2];
    result[2].is_null = &isNull[2];
    result[2].error = &errors[2];
    result[3].buffer_type = MYSQL_TYPE_STRING;
    result[3].buffer = nickBuffer;
    result[3].buffer_length = sizeof(nickBuffer);
    result[3].length = &lengths[3];
    result[3].is_null = &isNull[3];
    result[3].error = &errors[3];
    result[4].buffer_type = MYSQL_TYPE_STRING;
    result[4].buffer = iconBuffer;
    result[4].buffer_length = sizeof(iconBuffer);
    result[4].length = &lengths[4];
    result[4].is_null = &isNull[4];
    result[4].error = &errors[4];

    if (mysql_stmt_bind_result(statement.get(), result) != 0) {
        return DbResult::DbError;
    }

    // 读取查询结果：没有数据表示用户不存在。
    const int fetchResult = mysql_stmt_fetch(statement.get());
    if (fetchResult == MYSQL_NO_DATA) {
        return DbResult::NotFound;
    }
    if (fetchResult != 0) {
        return DbResult::DbError;
    }

    // 按 MySQL 返回的列长度构造字符串，避免把缓冲区末尾的 '\0' 也复制进去。
    out->name.assign(nameBuffer, lengths[1]);
    out->password.assign(passwordBuffer, lengths[2]);
    out->nick.assign(nickBuffer, lengths[3]);
    out->icon.assign(iconBuffer, lengths[4]);
    return DbResult::Ok;
}

} // namespace m2
