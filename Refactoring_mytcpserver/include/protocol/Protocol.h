#ifndef PROTOCOL_PROTOCOL_H
#define PROTOCOL_PROTOCOL_H

#include <cstdint>
#include <string>

/*
 * 通信协议（客户端 / 服务端必须完全一致，联调期间只改这里）
 *
 * 这份头文件是客户端 net/protocol.h 的服务端镜像：数值、偏移、上限必须逐项相等。
 * 区别只有一个 —— 客户端用 Qt 的 quint16/quint32，这里用 std 的 uint16_t/uint32_t。
 *
 * 固定头 10 字节，全部小端（Little-Endian），后面跟 UTF-8 JSON 正文：
 *
 *   offset 0       2        6        10
 *          +--------+--------+--------+-------------------+
 *          | msgid  | bodyLen|  seq   |  body (UTF-8 JSON)|
 *          | uint16 | uint32 | uint32 |    bodyLen 字节   |
 *          +--------+--------+--------+-------------------+
 *
 *   msgid   : 消息编号，取值见下方 MsgId
 *   bodyLen : 仅正文长度，不含头
 *   seq     : 请求方自增；响应原样带回，用于请求-响应配对
 *
 * 为什么不直接传结构体：Linux x86-64 与 MinGW 的对齐、字节序约定不同，
 * 逐字节显式编解码最稳，也便于抓包排查。
 */
namespace proto {

    // 帧头各字段的偏移与长度
    const int kMsgIdOffset   = 0;
    const int kBodyLenOffset = 2;
    const int kSeqOffset     = 6;
    const int kHeaderSize    = 10;

    // 单帧正文上限，防止脏数据把内存撑爆（超过即认为是非法帧）
    const uint32_t kMaxBodySize = 4 * 1024 * 1024;

    enum MsgId{
        ID_GET_VARIFY_CODE          = 1001, // 获取验证码（暂未使用）
        ID_REG_USER                 = 1002, // 注册
        ID_RESET_PWD                = 1003, // 重置密码（暂未使用）
        ID_LOGIN_USER               = 1004, // 登录，返回 token
        ID_CHAT_LOGIN               = 1005, // 长连接上线认证
        ID_CHAT_LOGIN_RSP           = 1006,
        ID_SEARCH_USER_REQ          = 1007, // 搜索用户
        ID_SEARCH_USER_RSP          = 1008,
        ID_ADD_FRIEND_REQ           = 1009, // 发起好友申请
        ID_ADD_FRIEND_RSP           = 1010,
        ID_NOTIFY_ADD_FRIEND_REQ    = 1011, // 推送：有人申请加你
        ID_AUTH_FRIEND_REQ          = 1013, // 同意 / 拒绝好友申请
        ID_AUTH_FRIEND_RSP          = 1014,
        ID_NOTIFY_AUTH_FRIEND_REQ   = 1015, // 推送：对方同意了
        ID_TEXT_CHAT_MSG_REQ        = 1017, // 发送文本消息
        ID_TEXT_CHAT_MSG_RSP        = 1018,
        ID_NOTIFY_TEXT_CHAT_MSG_REQ = 1019, // 推送：收到新消息
        ID_NOTIFY_OFF_LINE_REQ      = 1021, // 被踢下线（异地登录）
        ID_HEART_BEAT_REQ           = 1023, // 心跳
        ID_HEARTBEAT_RSP            = 1024,
        ID_FRIEND_LIST_REQ          = 1030, // 拉好友列表
        ID_CHAT_LIST_REQ            = 1031, // 拉会话列表
        ID_OFFLINE_MSG_REQ          = 1032, // 拉离线消息
        ID_MSG_READ_REQ             = 1033, // 已读回执
        ID_FRIEND_LIST_RSP          = 1034,
        ID_CHAT_LIST_RSP            = 1035,
        ID_OFFLINE_MSG_RSP          = 1036,
    };

    struct Frame {
        uint16_t    msgId;
        uint32_t    seq;    // 让一条请求和它的返回对应起来
        std::string body;   // UTF-8 JSON，无参数时为空

        Frame() : msgId(0), seq(0) {}
    };

} // namespace proto

#endif // PROTOCOL_PROTOCOL_H
