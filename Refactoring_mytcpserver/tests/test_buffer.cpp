#include "net/Buffer.h"

#include <cstdio>
#include <string>

/*
 * Buffer 的单元测试。
 * 不依赖任何网络环境，纯逻辑，Windows / Linux 都能直接跑。
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

static void testInitialState()
{
    printf("testInitialState\n");
    Buffer buf;

    EXPECT_TRUE(buf.readableBytes() == 0);
    EXPECT_TRUE(buf.writableBytes() == Buffer::kInitialSize);
    EXPECT_TRUE(buf.prependableBytes() == Buffer::kCheapPrepend);
}

static void testAppendAndRetrieve()
{
    printf("testAppendAndRetrieve\n");
    Buffer buf;

    buf.append("hello");
    EXPECT_TRUE(buf.readableBytes() == 5);
    EXPECT_TRUE(std::string(buf.peek(), buf.readableBytes()) == "hello");

    EXPECT_TRUE(buf.retrieveAsString(2) == "he");
    EXPECT_TRUE(buf.readableBytes() == 3);
    EXPECT_TRUE(buf.prependableBytes() == Buffer::kCheapPrepend + 2);

    buf.append(std::string("!!"));
    EXPECT_TRUE(buf.retrieveAllAsString() == "llo!!");
    EXPECT_TRUE(buf.readableBytes() == 0);
    EXPECT_TRUE(buf.prependableBytes() == Buffer::kCheapPrepend);
}

static void testGrowKeepsData()
{
    printf("testGrowKeepsData\n");
    Buffer buf;

    buf.append("head");
    std::string big(100000, 'x');
    buf.append(big);                    /* 触发扩容 */

    EXPECT_TRUE(buf.readableBytes() == 4 + big.size());
    EXPECT_TRUE(std::string(buf.peek(), 4) == "head");   /* 扩容不能弄丢老数据 */
    EXPECT_TRUE(buf.retrieveAllAsString() == "head" + big);
}

static void testMakeSpaceMoveFront()
{
    printf("testMakeSpaceMoveFront\n");
    Buffer buf;

    buf.append(std::string(1000, 'a'));
    buf.retrieve(1000);                        /* 前面腾出 1000 字节 */
    buf.append(std::string(1000, 'b'));        /* 应该走「数据前移」，而不是扩容 */

    EXPECT_TRUE(buf.readableBytes() == 1000);
    EXPECT_TRUE(buf.retrieveAllAsString() == std::string(1000, 'b'));
}

static void testRetrieveMoreThanReadable()
{
    printf("testRetrieveMoreThanReadable\n");
    Buffer buf;

    buf.append("abc");
    buf.retrieve(100);                  /* 越界读取：应当等价于全部读走 */
    EXPECT_TRUE(buf.readableBytes() == 0);
    EXPECT_TRUE(buf.prependableBytes() == Buffer::kCheapPrepend);
}

int main()
{
    testInitialState();
    testAppendAndRetrieve();
    testGrowKeepsData();
    testMakeSpaceMoveFront();
    testRetrieveMoreThanReadable();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
