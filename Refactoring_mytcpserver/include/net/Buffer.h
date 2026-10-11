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

    // 初始化大小(默认1024)
    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(kCheapPrepend + initialSize),
          readerIndex_(kCheapPrepend),
          writerIndex_(kCheapPrepend) {}

    size_t readableBytes() const { return writerIndex_ - readerIndex_; }        // 可读的比特数
    size_t writableBytes() const { return buffer_.size() - writerIndex_; }      // 可写的比特数
    size_t prependableBytes() const { return readerIndex_; }                    // 前面留白了多少比特，可以拿来放协议头或者长度等

    const char* peek() const { return begin() + readerIndex_; }                 // 可读数据的起始位置

    void retrieve(size_t len);                                                  // 读走 len 字节
    void retrieveAll();                                                         // 读走所有字节

    std::string retrieveAllAsString();                                          // 把“当前所有可读的数据”都取出来，返回成一个字符串。
    std::string retrieveAsString(size_t len);                                   // 把“前 len 个字节”从缓冲区中取出来，并转成字符串。

    void append(const char* data, size_t len);                                  // 追加数据，传入一段原始字节数组，并且知道长度
    void append(const std::string& str);                                        // 追加数据，直接传入字符串

    char* beginWrite() { return begin() + writerIndex_; }                       // 返回当前可以开始写入数据的位置
    const char* beginWrite() const { return begin() + writerIndex_; }

    void refreshWrite(size_t len) { writerIndex_ += len; }                      // 调用方直接往 beginWrite() 写入了 len 字节后，要调它更新下标

    void ensureWritableBytes(size_t len);                                       // 确保当前 Buffer 里有至少 len 个字节的可写空间，否则就先扩容/整理内存。

    /*
     * 从 fd 读数据塞进缓冲区。返回读到的字节数，出错返回 -1 并把 errno 存进 savedErrno。
     * 这是 Buffer 唯一碰系统调用的地方。
     */
    ssize_t readFd(int fd, int* savedErrno);

private:
    char* begin() { return &*buffer_.begin(); }                                 // 返回缓冲区内存块的起始位置(可修改指针版)
    const char* begin() const { return &*buffer_.begin(); }                     // const对象版(只读不改版)

    void makeSpace(size_t len);                                                 // 空间不够时的扩容 / 数据前移

    std::vector<char> buffer_;                                                  // 真实的buffer(vector做底层)
    size_t readerIndex_;                                                        // 读针
    size_t writerIndex_;                                                        // 写针
};

#endif
