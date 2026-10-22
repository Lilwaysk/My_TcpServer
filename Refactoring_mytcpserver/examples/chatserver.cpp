#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "base/Logger.h"
#include "dao/Database.h"
#include "dao/FriendDao.h"
#include "dao/MessageDao.h"
#include "dao/UserDao.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/TcpConnection.h"
#include "net/TcpServer.h"
#include "protocol/Codec.h"
#include "protocol/Protocol.h"
#include "service/FriendService.h"
#include "service/UserService.h"
#include "session/SessionManager.h"
#include "service/MessageService.h"
#include "util/Config.h"

using namespace std::placeholders;

namespace {

/**
 * @brief 生成“不支持的消息编号”响应。
 */
std::string unsupportedResponse(std::uint16_t msgId)
{
    nlohmann::json response;
    response["error"] = 6;
    response["message"] = "unsupported msgid";
    response["msgid"] = msgId;
    return response.dump();
}

/**
 * @brief 生成“登录后才能执行该操作”的响应。
 */
std::string loginRequiredResponse()
{
    // 需要 uid 的业务请求统一返回该错误，避免把匿名连接当成已登录用户。
    nlohmann::json response;
    response["error"] = 1;
    response["message"] = "login required";
    return response.dump();
}

} // namespace

/**
 * @brief 聊天服务端门面。
 *
 * 负责协议分发、连接生命周期、心跳和业务服务装配；
 * 具体用户和好友逻辑分别交给 UserService/FriendService。
 */
class ChatServer {
public:
    /**
     * @brief 装配 TCP 服务器回调，并保存业务服务指针。
     */
    ChatServer(EventLoop* loop,
               const InetAddress& listenAddr,
               m2::UserService* userService,
               m2::FriendService* friendService,
               m2::MessageService* messageService,
               m2::SessionManager* sessions)
        : _loop(loop),
          server_(loop, listenAddr, "ChatServer", TcpServer::kReusePort),
          userService_(userService),
          friendService_(friendService),
          messageService_(messageService),
          _sessions(sessions)
    {
        server_.setConnectionCallback(
            std::bind(&ChatServer::onConnection, this, _1));
        server_.setMessageCallback(
            std::bind(&ChatServer::onMessage, this, _1, _2, _3));
    }

    /**
     * @brief 设置 IO 线程数；M4 建议保持 0（单 Reactor）。
     */
    void setThreadNum(int count)
    {
        server_.setThreadNum(count);
    }

    /**
     * @brief 启动监听并注册 10 秒周期心跳扫描。
     */
    void start()
    {
        server_.start();
        startHeartbeat();
    }

private:
    /**
     * @brief 每条连接的心跳状态。
     */
    struct ClientState {
        // 连接本身，保持 shared_ptr 生命周期。
        TcpConnectionPtr conn;

        // 最近一次收到任意帧的时间，用于 35 秒超时判断。
        Timestamp lastActive;
    };

    /**
     * @brief 刷新连接最后活跃时间。
     */
    void touch(const TcpConnectionPtr& conn)
    {
        std::map<std::string, ClientState>::iterator it =
            _clients.find(conn->name());
        if (it != _clients.end()) {
            it->second.lastActive = Timestamp::now();
        }
    }

    /**
     * @brief 连接建立/断开回调。
     *
     * UP 时加入心跳表；DOWN 时同时清理心跳表和 uid 在线表。
     */
    void onConnection(const TcpConnectionPtr& conn)
    {
        if (conn->connected()) {
            // 新连接先进入心跳表，此时尚未登录。
            ClientState state;
            state.conn = conn;
            state.lastActive = Timestamp::now();
            _clients[conn->name()] = state;
            LOG_INFO << conn->peerAddress().toIpPort() << " UP";
        } else {
            // 连接断开时同时清理心跳表和 uid 在线表，防止继续向死连接推送。
            _clients.erase(conn->name());
            _sessions->remove(conn);
            LOG_INFO << conn->peerAddress().toIpPort() << " DOWN";
        }
    }

    /**
     * @brief 解码 TCP 帧并按 msgid 分发业务请求。
     */
    void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp)
    {
        touch(conn);

        bool fatal = false;
        const std::vector<proto::Frame> frames =
            proto::Codec::tryDecode(buf, &fatal);

        for (std::size_t i = 0; i < frames.size(); ++i) {
            const proto::Frame& frame = frames[i];
            std::uint16_t responseMsgId = frame.msgId;
            std::string response;

            LOG_INFO << "recv msgid=" << frame.msgId
                     << " seq=" << frame.seq
                     << " from=" << conn->peerAddress().toIpPort();

            // 1002 注册：只创建账号，不绑定在线状态。
            if (frame.msgId == proto::ID_REG_USER) {
                userService_->handleRegister(frame.body, &response);
            } else if (frame.msgId == proto::ID_LOGIN_USER) {
                // 1004 登录：UserService 写出 uid，随后建立 uid -> conn 映射。
                // M4 的 1011/1015 实时推送依赖这条映射。
                std::uint64_t loginUid = 0;
                userService_->handleLogin(
                    frame.body, &response, &loginUid);
                if (loginUid != 0) {
                    _sessions->bind(loginUid, conn);
                }
            } else if (frame.msgId == proto::ID_SEARCH_USER_REQ) {
                // 1007 搜索：响应 msgid 必须改为 1008。
                responseMsgId = proto::ID_SEARCH_USER_RSP;
                const std::uint64_t uid = _sessions->uidOf(conn);
                if (uid == 0) {
                    response = loginRequiredResponse();
                } else {
                    friendService_->handleSearchUsers(
                        uid, frame.body, &response);
                }
            } else if (frame.msgId == proto::ID_ADD_FRIEND_REQ) {
                // 1009 发起申请：响应 1010，FriendService 内部推送 1011。
                responseMsgId = proto::ID_ADD_FRIEND_RSP;
                const std::uint64_t uid = _sessions->uidOf(conn);
                if (uid == 0) {
                    response = loginRequiredResponse();
                } else {
                    friendService_->handleAddFriend(
                        uid, frame.body, &response);
                }
            } else if (frame.msgId == proto::ID_AUTH_FRIEND_REQ) {
                // 1013 同意/拒绝：响应 1014，事务成功后推送 1015。
                responseMsgId = proto::ID_AUTH_FRIEND_RSP;
                const std::uint64_t uid = _sessions->uidOf(conn);
                if (uid == 0) {
                    response = loginRequiredResponse();
                } else {
                    friendService_->handleAuthFriend(
                        uid, frame.body, &response);
                }
            } else if (frame.msgId == proto::ID_FRIEND_LIST_REQ) {
                // 1030 拉好友列表：响应 1034。
                responseMsgId = proto::ID_FRIEND_LIST_RSP;
                const std::uint64_t uid = _sessions->uidOf(conn);
                if (uid == 0) {
                    response = loginRequiredResponse();
                } else {
                    friendService_->handleListFriends(uid, &response);
                }
            } else if (frame.msgId == proto::ID_HEART_BEAT_REQ) {
                // 1023 -> 1024，保持 M3 连接保活行为。
                responseMsgId = proto::ID_HEARTBEAT_RSP;
                nlohmann::json body;
                body["error"] = 0;
                body["server_time"] =
                    static_cast<long long>(std::time(nullptr));
                response = body.dump();
            } else if (frame.msgId == proto::ID_TEXT_CHAT_MSG_REQ) {
                // M5：1017 文本消息，响应 1018；接收方在线时 Service 推送 1019。
                responseMsgId = proto::ID_TEXT_CHAT_MSG_RSP;

                const std::uint64_t uid = _sessions->uidOf(conn);
                if (uid == 0) {
                    response = loginRequiredResponse();
                } else {
                    messageService_->handleSendText(uid, frame.body, &response);
                }

            } else if (frame.msgId == proto::ID_CHAT_LIST_REQ) {
                // M5：1031 会话列表，响应 1035。
                responseMsgId = proto::ID_CHAT_LIST_RSP;

                const std::uint64_t uid = _sessions->uidOf(conn);
                if (uid == 0) {
                    response = loginRequiredResponse();
                } else {
                    messageService_->handleChatList(uid, &response);
                }

            } else {
                response = unsupportedResponse(frame.msgId);
            }

            // 所有普通响应沿用请求 seq，客户端按 seq 配对回调。
            conn->send(proto::Codec::encode(
                responseMsgId, frame.seq, response));
        }

        if (fatal) {
            LOG_ERROR << "bad frame header from "
                      << conn->peerAddress().toIpPort();
            conn->forceClose();
        }
    }

    /**
     * @brief 注册周期心跳扫描任务。
     */
    void startHeartbeat()
    {
        // 周期任务挂在主 EventLoop；当前 M4 保持单 Reactor，避免 map 竞争。
        _loop->runEvery(10.0, [this]() {
            checkTimeouts();
        });
    }

    /**
     * @brief 关闭超过 35 秒没有消息的连接。
     */
    void checkTimeouts()
    {
        const Timestamp now = Timestamp::now();
        std::vector<TcpConnectionPtr> expired;

        // 先收集所有超时连接，再统一关闭，避免在遍历中重复触发回调。
        std::map<std::string, ClientState>::iterator it =
            _clients.begin();
        while (it != _clients.end()) {
            const double idleSeconds =
                static_cast<double>(
                    now.microSecondsSinceEpoch()
                    - it->second.lastActive.microSecondsSinceEpoch())
                / Timestamp::kMicroSecondsPerSecond;

            if (idleSeconds > 35.0) {
                expired.push_back(it->second.conn);
                _clients.erase(it++);
            } else {
                ++it;
            }
        }

        for (std::size_t i = 0; i < expired.size(); ++i) {
            LOG_INFO << "heartbeat timeout: "
                     << expired[i]->peerAddress().toIpPort();
            expired[i]->forceClose();
        }
    }

    // 主 EventLoop，周期定时器挂在它上面。
    EventLoop* _loop;

    // 现有 Reactor 网络服务器。
    TcpServer server_;

    // 用户业务：注册、登录。
    m2::UserService* userService_;

    // 好友业务：搜索、申请、同意/拒绝、好友列表。
    m2::FriendService* friendService_;

    // 消息业务：发送文本、消息落库、1019 推送、会话列表。
    m2::MessageService* messageService_;

    // uid -> 连接映射，用于 1011/1015 实时推送。
    m2::SessionManager* _sessions;

    // 连接名 -> 心跳状态。
    std::map<std::string, ClientState> _clients;
};

/**
 * @brief 程序入口：加载配置、连接 MySQL、装配服务并进入事件循环。
 */
int main(int argc, char* argv[])
{
    // 默认配置文件；支持 -c <path> 或直接传路径覆盖。
    std::string configPath = "conf/server.conf";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-c" && i + 1 < argc) {
            configPath = argv[++i];
        } else {
            configPath = argv[i];
        }
    }

    try {
        const m2::Config config = m2::Config::load(configPath);
        m2::Database database;
        if (!database.connect(config, true)) {
            std::cerr << "database connect failed: "
                      << database.lastError() << std::endl;
            return 1;
        }

        m2::SessionManager sessions;
        m2::UserDao userDao(&database);
        m2::FriendDao friendDao(&database);
        m2::MessageDao messageDao(&database);
        m2::UserService userService(&userDao);
        m2::FriendService friendService(&friendDao, &sessions);
        m2::MessageService messageService(&messageDao, &friendDao, &sessions);

        EventLoop loop;
        const unsigned short port = static_cast<unsigned short>(
            config.getInt("listen_port", 8888));
        InetAddress listenAddr(port);
        ChatServer server(
            &loop,
            listenAddr,
            &userService,
            &friendService,
            &messageService,
            &sessions);
        server.setThreadNum(config.getInt("io_threads", 0));
        server.start();

        LOG_INFO << "ChatServer listening on port " << port;
        loop.loop();
    } catch (const std::exception& error) {
        std::cerr << "chatserver failed: " << error.what() << std::endl;
        return 1;
    }
    return 0;
}
