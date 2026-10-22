#include "dao/Database.h"

#include <sstream>

#include "util/Config.h"

namespace m2 {

Database::Database()
    : mysql_(mysql_init(nullptr)),
      lastErrno_(0)
{
    if (mysql_ == nullptr) {
        lastError_ = "mysql_init failed";
    }
}

Database::~Database()
{
    if (mysql_ != nullptr) {
        mysql_close(mysql_);
    }
}

bool Database::connect(const Config& config, bool selectDatabase)
{
    if (mysql_ == nullptr) {
        return false;
    }

    unsigned int timeout = 3;
    bool getServerPublicKey = true;
    mysql_options(mysql_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(mysql_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    mysql_options(mysql_, MYSQL_OPT_GET_SERVER_PUBLIC_KEY,
                  &getServerPublicKey);

    const std::string host = config.get("mysql_host", "127.0.0.1");
    const unsigned int port =
        static_cast<unsigned int>(config.getInt("mysql_port", 3306));
    const std::string user = config.get("mysql_user", "clove");
    const std::string password = config.get("mysql_password", "");
    const std::string database = config.get("mysql_db", "");
    const char* databaseName =
        selectDatabase && !database.empty() ? database.c_str() : nullptr;

    MYSQL* result = mysql_real_connect(
        mysql_,
        host.c_str(),
        user.c_str(),
        password.c_str(),
        databaseName,
        port,
        nullptr,
        0);
    if (result == nullptr) {
        setError("mysql_real_connect");
        return false;
    }

    if (mysql_set_character_set(mysql_, "utf8mb4") != 0) {
        setError("mysql_set_character_set");
        return false;
    }

    std::string error;
    if (!execute("SET time_zone = '+00:00'", &error)) {
        lastError_ = error;
        return false;
    }
    return true;
}

bool Database::ping()
{
    if (mysql_ == nullptr || mysql_ping(mysql_) != 0) {
        setError("mysql_ping");
        return false;
    }
    return true;
}

bool Database::execute(const std::string& sql, std::string* error)
{
    if (mysql_ == nullptr) {
        if (error != nullptr) *error = "database is not initialized";
        return false;
    }
    if (mysql_query(mysql_, sql.c_str()) != 0) {
        setError("mysql_query");
        if (error != nullptr) *error = lastError_;
        return false;
    }
    return true;
}

void Database::setError(const char* operation)
{
    lastErrno_ = mysql_errno(mysql_);
    std::ostringstream out;
    out << operation << ": errno=" << lastErrno_
        << " message=" << mysql_error(mysql_);
    lastError_ = out.str();
}

} // namespace m2
