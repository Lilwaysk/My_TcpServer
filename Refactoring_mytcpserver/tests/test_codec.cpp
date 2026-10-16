#include "net/Buffer.h"
#include "protocol/Codec.h"
#include "protocol/Protocol.h"

#include <cstdio>
#include <string>
#include <vector>

/*
 * M1 验收程序：协议编解码（服务端侧）
 *
 * 用例与客户端 tests/test_codec.cpp 一一对应 —— 两边跑同一套用例，才算「协议冻结」。
 * 覆盖：正常往返、字节序金标准、粘包、半包（逐字节）、100KB 大包、
 *      非法帧头保护、seq 边界值。
 *
 * 编译运行：
 *   cd build && cmake .. && make -j && ./test_codec
 *
 * 想在不启动任何服务端的情况下，压一次「真实 socket 逐字节投递」：
 *   先起 ./protocol_echo_server，然后执行下面这一行（整条复制，别换行）
 *   python3 -c "import socket,time; s=socket.create_connection(('127.0.0.1',8888)); raw=bytes.fromhex('EC030200000001000000')+b'{}'; [ (s.send(bytes([b])), time.sleep(0.005)) for b in raw ]; print(s.recv(64).hex())"
 *   期望输出 12 字节：ec030200000001000000 7b7d（也就是原样回了一帧）
 */

static int g_checks = 0;
static int g_failed = 0;

#define EXPECT_TRUE(expr)                                                   \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(expr)) {                                                      \
            ++g_failed;                                                     \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);        \
        }                                                                   \
    } while (0)

static void appendAll(Buffer* buf, const std::string& raw)
{
    buf->append(raw);
}

// 1. 正常往返
static void testRoundTrip()
{
    printf("testRoundTrip\n");

    const std::string body = "{\"name\":\"alice\",\"pwd\":\"123\"}";
    Buffer buf;
    appendAll(&buf, proto::Codec::encode(proto::ID_LOGIN_USER, 7, body));

    std::vector<proto::Frame> frames = proto::Codec::tryDecode(&buf);
    EXPECT_TRUE(frames.size() == 1);
    if (frames.size() == 1) {
        EXPECT_TRUE(frames[0].msgId == proto::ID_LOGIN_USER);
        EXPECT_TRUE(frames[0].seq == 7u);
        EXPECT_TRUE(frames[0].body == body);
    }
    EXPECT_TRUE(buf.readableBytes() == 0);      // 消费干净
}

// 2. 字节序金标准：手工比对每一个字节。
//    这 12 个字节是全项目唯一的一致性硬保证，客户端必须断言出完全一样的结果。
static void testByteLayout()
{
    printf("testByteLayout\n");

    // msgid = 1004(0x03EC), seq = 1, body = "{}"
    const std::string raw = proto::Codec::encode(1004, 1, "{}");
    const unsigned char expect[] = {
        0xEC, 0x03,                         // msgid   小端
        0x02, 0x00, 0x00, 0x00,             // bodyLen = 2
        0x01, 0x00, 0x00, 0x00,             // seq = 1
        0x7B, 0x7D                          // "{}"
    };

    EXPECT_TRUE(raw.size() == 12);
    for (size_t i = 0; i < 12 && i < raw.size(); ++i)
        EXPECT_TRUE(static_cast<unsigned char>(raw[i]) == expect[i]);
}

// 3. 粘包：三帧一次送达
static void testStickyPackets()
{
    printf("testStickyPackets\n");

    std::string stream;
    stream += proto::Codec::encode(1004, 1, "{\"a\":1}");
    stream += proto::Codec::encode(1005, 2, "{\"b\":2}");
    stream += proto::Codec::encode(1023, 3, std::string());     // 空正文

    Buffer buf;
    appendAll(&buf, stream);

    std::vector<proto::Frame> frames = proto::Codec::tryDecode(&buf);
    EXPECT_TRUE(frames.size() == 3);
    if (frames.size() == 3) {
        EXPECT_TRUE(frames[0].msgId == 1004);
        EXPECT_TRUE(frames[1].msgId == 1005);
        EXPECT_TRUE(frames[2].msgId == 1023);
        EXPECT_TRUE(frames[2].body.empty());        // 空正文不能算半包
        EXPECT_TRUE(frames[2].seq == 3u);
    }
    EXPECT_TRUE(buf.readableBytes() == 0);
}

// 4. 半包：一帧按 1 字节切分送进来（顺带压 Buffer 的内存整理逻辑）
static void testByteByByte()
{
    printf("testByteByByte\n");

    const std::string raw = proto::Codec::encode(1002, 42, "{\"name\":\"bob\"}");

    Buffer buf;
    int got = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        buf.append(&raw[i], 1);
        const std::vector<proto::Frame> frames = proto::Codec::tryDecode(&buf);
        got += static_cast<int>(frames.size());
        if (i + 1 < raw.size())
            EXPECT_TRUE(frames.empty());            // 收齐之前一帧都不能出来
        else
            EXPECT_TRUE(frames.size() == 1);
    }
    EXPECT_TRUE(got == 1);
    EXPECT_TRUE(buf.readableBytes() == 0);
}

// 5. 大包：100KB 正文，并且分两次送达
static void testBigFrame()
{
    printf("testBigFrame\n");

    std::string big(100 * 1024, '\0');
    for (size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<char>('A' + (i % 26));

    const std::string raw = proto::Codec::encode(proto::ID_TEXT_CHAT_MSG_REQ, 99, big);

    Buffer buf;
    buf.append(raw.data(), raw.size() / 2);
    EXPECT_TRUE(proto::Codec::tryDecode(&buf).empty());     // 只有前半段

    buf.append(raw.data() + raw.size() / 2, raw.size() - raw.size() / 2);
    const std::vector<proto::Frame> frames = proto::Codec::tryDecode(&buf);
    EXPECT_TRUE(frames.size() == 1);
    if (frames.size() == 1) {
        EXPECT_TRUE(frames[0].body.size() == big.size());
        EXPECT_TRUE(frames[0].body == big);
    }
    EXPECT_TRUE(buf.readableBytes() == 0);
}

// 6. 非法帧头：bodyLen 明显越界时不能死等，也不能崩，并且要报给调用方
static void testBadHeader()
{
    printf("testBadHeader\n");

    std::string bad;
    bad.push_back(static_cast<char>(0xEC));
    bad.push_back(static_cast<char>(0x03));     // msgid = 1004
    bad.push_back(static_cast<char>(0xFF));
    bad.push_back(static_cast<char>(0xFF));
    bad.push_back(static_cast<char>(0xFF));
    bad.push_back(static_cast<char>(0x7F));     // bodyLen ≈ 2GB
    bad.append(4, '\0');                        // seq

    Buffer buf;
    appendAll(&buf, bad);

    bool fatal = false;
    const std::vector<proto::Frame> frames = proto::Codec::tryDecode(&buf, &fatal);
    EXPECT_TRUE(frames.empty());
    EXPECT_TRUE(fatal);                         // 调用方靠它决定断开连接
    EXPECT_TRUE(buf.readableBytes() == 0);       // 脏数据排空，不会无限增长
}

// 7. seq 边界值
static void testSeqEdges()
{
    printf("testSeqEdges\n");

    const uint32_t values[] = {0u, 1u, 0x7FFFFFFFu, 0xFFFFFFFFu};
    for (int i = 0; i < 4; ++i) {
        Buffer buf;
        appendAll(&buf, proto::Codec::encode(proto::ID_HEART_BEAT_REQ, values[i], "{}"));

        const std::vector<proto::Frame> frames = proto::Codec::tryDecode(&buf);
        EXPECT_TRUE(frames.size() == 1);
        if (frames.size() == 1)
            EXPECT_TRUE(frames[0].seq == values[i]);
    }
}

int main()
{
    testRoundTrip();
    testByteLayout();
    testStickyPackets();
    testByteByByte();
    testBigFrame();
    testBadHeader();
    testSeqEdges();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
