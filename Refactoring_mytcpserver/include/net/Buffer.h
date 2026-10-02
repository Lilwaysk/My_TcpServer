#ifndef NET_BUFFER_H
#define NET_BUFFER_H

#include <algorithm>
#include <string>
#include <sys/types.h>      /* ssize_t */
#include <vector>

/*
 * 输入 / 输出缓冲区。
 *
 * 它替掉你老代码里的 char buf[4096] + int len + int sendpos 三件套，解决三个问题：
 *   1. 一个包比 4KB 大：可以自动扩容，不会因为一次没收完就丢数据；
 *   2. 粘包：数据先攒在缓冲区里，由上层按协议决定"够不够一个完整包"；
 *   3. 部分写：没发完的数据留在缓冲区里，等 EPOLLOUT 再发。
 *
 * 内存布局（readerIndex_ 和 writerIndex_ 把 vector 切成三块）：
 *
 *   +-------------------+-----------------+------------------+
 *   |  prependable      |     readable    |    writable      |
 *   +-------------------+-----------------+------------------+
 *   0              readerIndex_       writerIndex_       size()
 *
 *   prependable：已经读走、可以复用的空间（还可以用来在前面塞长度头）
 *   readable   ：还没被上层取走的数据，peek() 指向这里
 *   writable   ：剩余可写空间，beginWrite() 指向这里
 */
class Buffer {
public:
    static const size_t kCheapPrepend = 8;      // buffer前8字节留出来不存数据，存"长度头",readerIndex从8开始计算
    static const size_t kInitialSize = 1024;    // 初始缓冲区大小

    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(kCheapPrepend + initialSize),
          readerIndex_(kCheapPrepend),
          writerIndex_(kCheapPrepend) {}

    size_t readableBytes() const { return writerIndex_ - readerIndex_; }
    size_t writableBytes() const { return buffer_.size() - writerIndex_; }
    size_t prependableBytes() const { return readerIndex_; }

    /* 可读数据的起始位置 */
    const char* peek() const { return begin() + readerIndex_; }

    /* 读走 len 字节 */
    void retrieve(size_t len);
    void retrieveAll();

    std::string retrieveAllAsString();
    std::string retrieveAsString(size_t len);

    /* 追加数据，空间不够会自动处理 */
    void append(const char* data, size_t len);
    void append(const std::string& str);

    char* beginWrite() { return begin() + writerIndex_; }
    const char* beginWrite() const { return begin() + writerIndex_; }

    /* 调用方直接往 beginWrite() 写入了 len 字节后，要调它更新下标 */
    void hasWritten(size_t len) { writerIndex_ += len; }

    void ensureWritableBytes(size_t len);

    /*
     * 从 fd 读数据塞进缓冲区。返回读到的字节数，出错返回 -1 并把 errno 存进 savedErrno。
     * 这是 Buffer 唯一碰系统调用的地方。
     */
    ssize_t readFd(int fd, int* savedErrno);

private:
    // 返回缓冲区内存块的起始位置(可修改指针版)
    char* begin() { return &*buffer_.begin(); }
    // const对象版(只读不改版)
    const char* begin() const { return &*buffer_.begin(); }

    /* 空间不够时的扩容 / 数据前移 */
    void makeSpace(size_t len);

    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
};

#endif
