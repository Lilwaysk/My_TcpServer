#include "service/UserService.h"

#include <nlohmann/json.hpp>

#include "dao/UserDao.h"

namespace {

// 注册和登录向客户端返回的业务错误码。
enum ErrorCode {
    kSuccess = 0,
    kInvalidArgument = 1,
    kDuplicateName = 2,
    kBadCredentials = 3,
    kDatabaseError = 5
};

std::string errorResponse(int error, const std::string& message)
{
    // 使用统一 JSON 结构，客户端可以通过 error 集中映射提示文案。
    nlohmann::json response;
    response["error"] = error;
    response["message"] = message;
    return response.dump();
}

} // namespace

namespace m2 {

/**
 * @brief 处理 1002 注册请求。
 *
 * 校验输入后调用 UserDao::create，用户名唯一性由数据库唯一索引兜底。
 */
bool UserService::handleRegister(const std::string& requestBody,
                                 std::string* responseBody)
{
    if (responseBody == nullptr) {
        return false;
    }

    // non-throwing parse：格式错误时返回业务错误，而不是抛异常退出服务端。
    const nlohmann::json request =
        nlohmann::json::parse(requestBody, nullptr, false);
    if (request.is_discarded() || !request.is_object()
        || !request.contains("name") || !request["name"].is_string()
        || !request.contains("pwd") || !request["pwd"].is_string()) {
        *responseBody = errorResponse(kInvalidArgument, "invalid json");
        return true;
    }

    const std::string name = request["name"].get<std::string>();
    const std::string password = request["pwd"].get<std::string>();
    std::string nick;
    if (request.contains("nick")) {
        if (!request["nick"].is_string()) {
            *responseBody = errorResponse(kInvalidArgument, "invalid fields");
            return true;
        }
        nick = request["nick"].get<std::string>();
    }

    if (name.empty() || name.size() > 64
        || password.empty() || password.size() > 64
        || nick.size() > 64) {
        *responseBody = errorResponse(kInvalidArgument, "invalid fields");
        return true;
    }

    std::uint64_t createdUid = 0;
    const DbResult result =
        userDao_->create(name, password, nick, &createdUid);
    if (result == DbResult::DuplicateName) {
        *responseBody = errorResponse(kDuplicateName, "name already exists");
        return true;
    }
    if (result != DbResult::Ok) {
        *responseBody = errorResponse(kDatabaseError, "database error");
        return true;
    }

    nlohmann::json response;
    response["error"] = kSuccess;
    response["uid"] = createdUid;
    response["name"] = name;
    response["nick"] = nick;
    *responseBody = response.dump();
    return true;
}

/**
 * @brief 处理 1004 登录请求。
 *
 * 成功后不仅生成客户端响应，还通过 uid 输出参数把登录用户告诉 ChatServer，
 * 以便绑定当前 TCP 连接。
 */
bool UserService::handleLogin(const std::string& requestBody,
                              std::string* responseBody,
                              std::uint64_t* uid)
{
    if (responseBody == nullptr) {
        return false;
    }

    const nlohmann::json request =
        nlohmann::json::parse(requestBody, nullptr, false);
    if (request.is_discarded() || !request.is_object()
        || !request.contains("name") || !request["name"].is_string()
        || !request.contains("pwd") || !request["pwd"].is_string()) {
        *responseBody = errorResponse(kInvalidArgument, "invalid json");
        return true;
    }

    const std::string name = request["name"].get<std::string>();
    const std::string password = request["pwd"].get<std::string>();
    if (name.empty() || password.empty()) {
        *responseBody = errorResponse(kInvalidArgument, "invalid fields");
        return true;
    }

    UserRow user;
    const DbResult result = userDao_->findByName(name, &user);
    if (result == DbResult::NotFound
        || (result == DbResult::Ok && user.password != password)) {
        *responseBody = errorResponse(kBadCredentials, "bad credentials");
        return true;
    }
    if (result != DbResult::Ok) {
        *responseBody = errorResponse(kDatabaseError, "database error");
        return true;
    }

    // ChatServer 需要这个 uid 把当前 TCP 连接绑定到登录用户。
    // 只写 JSON 响应而不写 uid 会导致后续好友申请被判定为未登录。
    if (uid != nullptr) {
        *uid = user.uid;
    }

    nlohmann::json response;
    response["error"] = kSuccess;
    response["uid"] = user.uid;
    response["name"] = user.name;
    response["nick"] = user.nick;
    response["icon"] = user.icon;
    *responseBody = response.dump();
    return true;
}

} // namespace m2
