#ifndef DAO_DATABASE_H
#define DAO_DATABASE_H

#include <string>

#include <mysql/mysql.h>

namespace m2 {

class Config;

/**
 * @brief DAO 层统一使用的数据库操作结果。
 *
 * DAO 不直接把 MySQL 错误码暴露给上层，而是转换成业务层容易判断的结果：
 * - Ok：操作成功；
 * - NotFound：查询没有找到记录；
 * - DuplicateName：插入时违反用户名唯一约束；
 * - DbError：连接失败、SQL 错误或其他数据库异常。
 */
enum class DbResult {
    Ok,
    NotFound,
    DuplicateName,
    DbError
};

/**
 * @brief MySQL 连接的 RAII 封装。
 *
 * Database 只负责连接生命周期、字符集、时区和错误信息，不包含用户业务 SQL。
 * 具体 SQL 由 UserDao 等 DAO 类通过 handle() 取得连接后执行。
 *
 * 同一个 MYSQL 连接不能被多个线程同时使用。当前 M2 使用单 Reactor，
 * 因此整个进程只持有一个 Database 实例。
 */
class Database {
public:
    /**
     * @brief 初始化 MySQL 句柄。
     *
     * mysql_init 失败时对象仍可构造，但 connect() 会返回 false。
     */
    Database();

    /**
     * @brief 释放 MySQL 连接句柄。
     */
    ~Database();

    // 禁止拷贝，避免两个对象重复关闭同一个 MYSQL 句柄。
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /**
     * @brief 根据配置文件建立数据库连接。
     *
     * 从 Config 读取 mysql_host、mysql_port、mysql_user、mysql_password、
     * mysql_db 等字段，并完成以下初始化：
     * - 设置连接超时；
     * - 设置 utf8mb4 字符集；
     * - 启用 caching_sha2_password 所需的公钥获取；
     * - 连接后把会话时区设置为 UTC。
     *
     * @param config 已加载的配置对象。
     * @param selectDatabase 是否在连接时选择 mysql_db 指定的数据库。
     *                       只测试账号连通性时可以传 false。
     * @return 连接及会话初始化成功返回 true，否则返回 false。
     */
    bool connect(const Config& config, bool selectDatabase = true);

    /**
     * @brief 检查连接是否仍然可用。
     * @return MySQL 响应正常返回 true，否则返回 false。
     */
    bool ping();

    /**
     * @brief 执行一条不需要结果集的 SQL。
     *
     * 适合 SET、INSERT、UPDATE、DELETE 等语句。失败时通过 error 返回
     * 包含 MySQL 错误码和错误信息的文本。
     *
     * @param sql 要执行的 SQL。
     * @param error 可选的错误输出参数，允许传 nullptr。
     * @return SQL 执行成功返回 true，否则返回 false。
     */
    bool execute(const std::string& sql, std::string* error);

    /**
     * @brief 返回底层 MySQL 句柄，供 DAO 层创建 prepared statement。
     * @return 底层 MYSQL 指针；对象不可用时可能为 nullptr。
     */
    MYSQL* handle() const { return mysql_; }

    /**
     * @brief 返回最近一次数据库操作的错误描述。
     */
    const std::string& lastError() const { return lastError_; }

    /**
     * @brief 返回最近一次数据库操作的 MySQL 错误码。
     */
    unsigned int lastErrno() const { return lastErrno_; }

private:
    /**
     * @brief 保存 MySQL 最近一次错误码和错误信息。
     * @param operation 发生错误的操作名，例如 mysql_real_connect。
     */
    void setError(const char* operation);

    // 底层 MySQL 连接句柄，由构造函数创建、析构函数释放。
    MYSQL* mysql_;

    // 最近一次数据库错误的可读描述。
    std::string lastError_;

    // 最近一次数据库错误的 MySQL 错误码。
    unsigned int lastErrno_;
};

} // namespace m2

#endif // DAO_DATABASE_H
