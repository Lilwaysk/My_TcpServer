#include "net/Buffer.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>     /* read */
#include <sys/types.h>  /* ssize_t */

void Buffer::retrieve(size_t len)
{
    if (len < readableBytes())
        readerIndex_ += len;
    else
        retrieveAll();      /* 读走全部，等价于重置 */
}

void Buffer::retrieveAll()
{
    /*
     * 不清空 vector，只是把两个下标挪回起点。
     * 这样内存可以复用，避免频繁 malloc/free —— 高并发下这是很值钱的优化。
     */
    readerIndex_ = kCheapPrepend;
    writerIndex_ = kCheapPrepend;
}

std::string Buffer::retrieveAsString(size_t len)
{
    std::string result(peek(), len);
    retrieve(len);
    return result;
}

std::string Buffer::retrieveAllAsString()
{
    return retrieveAsString(readableBytes());
}

void Buffer::append(const char* data, size_t len)
{
    ensureWritableBytes(len);
    std::copy(data, data + len, beginWrite());
    hasWritten(len);
}

void Buffer::append(const std::string& str)
{
    append(str.data(), str.size());
}

void Buffer::ensureWritableBytes(size_t len)
{
    if (writableBytes() < len)
        makeSpace(len);
}

void Buffer::makeSpace(size_t len)
{
    if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
        /*
         * 前后加起来都不够：只能真的把 vector 变大。
         * 注意是把容量扩到 writerIndex_ + len，而不是 size() + len，
         * 因为 readerIndex_ 前面的空间留着当 prepend 用。
         */
        buffer_.resize(writerIndex_ + len);
    } else {
        /*
         * 前面还有被读走的空间：把可读数据整体前移到 kCheapPrepend 处，
         * 腾出后面的空间，避免扩容。
         */
        size_t readable = readableBytes();
        std::copy(begin() + readerIndex_,
                  begin() + writerIndex_,
                  begin() + kCheapPrepend);
        readerIndex_ = kCheapPrepend;
        writerIndex_ = readerIndex_ + readable;
    }
}

ssize_t Buffer::readFd(int fd, int* savedErrno)
{
    /*
     * 这里是最朴素的写法：先往可写空间里读，读满了再用栈上临时缓冲接一次。
     * muduo 用 readv 一次系统调用同时读进这两个地方，省一次拷贝 ——
     * 等你把这一版跑通了再换过去，别一上来就追这个。
     */
    char extrabuf[65536];
    ssize_t n = 0;
    const size_t writable = writableBytes();

    if (writable > 0) {
        n = ::read(fd, beginWrite(), writable);
        if (n < 0) {
            if (savedErrno)
                *savedErrno = errno;
            return n;
        }
        if (static_cast<size_t>(n) < writable)
            return n;           /* 内核缓冲区已经读空了 */
        hasWritten(static_cast<size_t>(n));
    }

    /* 可写空间已经占满，可能还有数据：先读进栈上，再追加（会自动扩容） */
    ssize_t n2 = ::read(fd, extrabuf, sizeof(extrabuf));
    if (n2 < 0) {
        if (savedErrno)
            *savedErrno = errno;
        return n > 0 ? n : n2;
    }

    append(extrabuf, static_cast<size_t>(n2));
    return n + n2;
}
