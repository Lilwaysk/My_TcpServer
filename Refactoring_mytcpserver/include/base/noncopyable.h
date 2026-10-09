#ifndef BASE_NONCOPYABLE_H
#define BASE_NONCOPYABLE_H

/*
 * 禁止拷贝的基类。
 *
 * C++11 之后直接在类里写 = delete 就够了，这个基类主要是让
 * 「这个类不可拷贝」这件事在类声明的第一行就看得见。觉得多余可以删掉。
 * = default ：明确要求编译器提供默认实现
 */
class noncopyable {
public:
    noncopyable(const noncopyable&) = delete;
    noncopyable& operator=(const noncopyable&) = delete;

protected:
    noncopyable() = default;
    ~noncopyable() = default;
};

#endif
