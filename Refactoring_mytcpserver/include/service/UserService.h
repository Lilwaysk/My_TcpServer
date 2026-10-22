#ifndef SERVICE_USER_SERVICE_H
#define SERVICE_USER_SERVICE_H

#include <cstdint>
#include <string>

namespace m2 {

class UserDao;

class UserService {
public:
    /**
     * @brief 注入用户 DAO，不接管其生命周期。
     */
    explicit UserService(UserDao* userDao) : userDao_(userDao) {}

    /**
     * @brief 处理 1002 注册请求。
     */
    bool handleRegister(const std::string& requestBody,
                        std::string* responseBody);

    /**
     * @brief 处理 1004 登录请求，并写出登录用户 uid。
     */
    bool handleLogin(const std::string& requestBody,
                     std::string* responseBody,
                     std::uint64_t* uid);

private:
    // 用户持久层，注册和登录 SQL 都集中在其实现中。
    UserDao* userDao_;
};

} // namespace m2

#endif // SERVICE_USER_SERVICE_H
