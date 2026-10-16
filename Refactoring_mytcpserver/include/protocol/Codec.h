#ifndef PROTOCOL_CODEC_H
#define PROTOCOL_CODEC_H

#include <cstdint>
#include <string>
#include <vector>

#include "protocol/Protocol.h"

class Buffer;

/*
 * 协议编解码。收发路径上所有字节都经过这里，上层只处理 proto::Frame。
 *
 * 这是客户端 net/codec.h 的另一份实现：规则完全一样，只是把 QByteArray 换成
 * std::string / Buffer。两端一致性靠 tests/test_codec.cpp 里的「字节金标准」保证。
 *
 * 依赖方向：protocol/ -> net/ -> base/。协议层可以用 netlib 的 Buffer，
 * 但 netlib 不认识 protocol —— 库依然可以被别的程序复用。
 */
namespace proto {

class Codec {
public:
    // 组帧：body 已经是 JSON 字节，返回可直接 conn->send() 的完整字节流
    static std::string encode(uint16_t msgId, uint32_t seq,
                             const std::string& body = std::string());

    // 语法糖重载：写字面量时不用手写 std::string
    static std::string encode(uint16_t msgId, uint32_t seq, const char* json);

    /*
     * 从连接输入缓冲区里取出所有完整帧，并把已消费的字节从 Buffer 里移走。
     * 半包会原样留在 Buffer 里，等下一次数据到达再接着解析。
     *
     * fatal：帧头里的 bodyLen 超过 kMaxBodySize 时置 true，说明数据流已经失步、
     * 不可能再对齐了，调用方应当记日志并断开连接（客户端那边选择清空缓冲区，
     * 服务端不能这么做 —— 对面的流还是错的，清了也回不来）。
     * 传 nullptr 表示不关心，但缓冲区一样会被排空。
     */
    static std::vector<Frame> tryDecode(Buffer* buf, bool* fatal = nullptr);
};

} // namespace proto

#endif // PROTOCOL_CODEC_H
