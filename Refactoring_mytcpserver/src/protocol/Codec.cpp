#include "protocol/Codec.h"

#include <utility>

#include "net/Buffer.h"

namespace {

// ---- 小端读写：不使用结构体，避免对齐与字节序问题 ----

// 把 16 位整数按小端（低位字节优先）追加到字符串尾部
inline void appendLe16(std::string& out, uint16_t v)
{
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

// 把 32 位整数拆成 4 个字节按低 -> 高顺序追加
inline void appendLe32(std::string& out, uint32_t v)
{
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

// 先转无符号再移位，避免 char 有符号时左移出错
inline uint16_t readLe16(const char* p)
{
    return static_cast<uint16_t>(
        static_cast<uint16_t>(static_cast<uint8_t>(p[0])) |
        static_cast<uint16_t>(static_cast<uint16_t>(static_cast<uint8_t>(p[1])) << 8));
}

inline uint32_t readLe32(const char* p)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(p[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
}

} // namespace

namespace proto {

// 先把整个头 + 正文拼成形，再一次交给 conn->send()。
// 注意不要试图用 Buffer 的 prepend 区（kCheapPrepend = 8）塞这个 10 字节的头，塞不下。
std::string Codec::encode(uint16_t msgId, uint32_t seq, const std::string& body)
{
    std::string out;
    out.reserve(kHeaderSize + body.size());
    appendLe16(out, msgId);
    appendLe32(out, static_cast<uint32_t>(body.size()));
    appendLe32(out, seq);
    out += body;
    return out;
}

std::string Codec::encode(uint16_t msgId, uint32_t seq, const char* json)
{
    return encode(msgId, seq, json == nullptr ? std::string() : std::string(json));
}

/*
 * 粘包 / 半包处理，规则与任务书 3.4 一致：
 *
 *   while (可读字节 >= 10) {
 *       msgid, bodyLen, seq = 读取头部（不移动读指针）
 *       if (可读字节 < 10 + bodyLen) break;      // 半包，等下次
 *       取出完整帧，移动读指针 10 + bodyLen
 *       解析 JSON -> 分发
 *   }
 */
std::vector<Frame> Codec::tryDecode(Buffer* buf, bool* fatal)
{
    std::vector<Frame> frames;
    if (fatal != nullptr) *fatal = false;
    if (buf == nullptr) return frames;

    while (buf->readableBytes() >= static_cast<size_t>(kHeaderSize)) {
        // 每轮重新 peek：retrieve() 之后 Buffer 可能已经整理过内存，旧指针不能复用
        const char* p = buf->peek();

        const uint16_t msgId   = readLe16(p + kMsgIdOffset);
        const uint32_t bodyLen = readLe32(p + kBodyLenOffset);

        if (bodyLen > kMaxBodySize) {
            // 帧头已经不可信。这里绝不能只清缓冲区就当没事：
            // 对面的流已经错位，后面所有字节都会解析成垃圾。
            if (fatal != nullptr) *fatal = true;
            buf->retrieveAll();         // Buffer 没有 clear()，用它把脏数据排空
            return frames;
        }

        const size_t total = static_cast<size_t>(kHeaderSize) + bodyLen;
        if (buf->readableBytes() < total) break;    // 半包，等下次数据到达

        Frame frame;
        frame.msgId = msgId;
        frame.seq   = readLe32(p + kSeqOffset);
        frame.body.assign(p + kHeaderSize, static_cast<size_t>(bodyLen));
        frames.push_back(std::move(frame));

        buf->retrieve(total);
    }

    return frames;
}

} // namespace proto
