#ifndef DAO_USER_DAO_H
#define DAO_USER_DAO_H

#include <cstdint>
#include <string>

#include "dao/Database.h"

namespace m2 {

struct UserRow {
    std::uint64_t uid;
    std::string name;
    std::string password;
    std::string nick;
    std::string icon;

    UserRow() : uid(0) {}
};

class UserDao {
public:
    explicit UserDao(Database* db) : db_(db) {}

    DbResult create(const std::string& name,
                    const std::string& password,
                    const std::string& nick,
                    std::uint64_t* uid);

    DbResult findByName(const std::string& name, UserRow* out);

private:
    Database* db_;
};

} // namespace m2

#endif // DAO_USER_DAO_H
