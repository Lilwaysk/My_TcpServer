#include "net/EventLoopThread.h"

#include "net/EventLoop.h"

/*
 * ============ EventLoopThread 实现清单（README 第四节「第 7 步」）============
 *
 * threadFunc() —— 这个函数运行在新线程里：
 *      EventLoop loop;                     // 建在子线程栈上，
 *                                          // 于是 loop 的 threadId_ 自然就是子线程
 *      {
 *          std::lock_guard<std::mutex> lock(mutex_);
 *          loop_ = &loop;
 *          cond_.notify_one();             // 告诉 startLoop 可以返回了
 *      }
 *      if (callback_) callback_(&loop);
 *      loop.loop();                        // 阻塞在这里，直到 quit()
 *      {
 *          std::lock_guard<std::mutex> lock(mutex_);
 *          loop_ = nullptr;                // 退出后置空，避免外面拿到野指针
 *      }
 *
 * startLoop()：
 *      thread_.reset(new std::thread(&EventLoopThread::threadFunc, this));
 *      {
 *          std::unique_lock<std::mutex> lock(mutex_);
 *          while (loop_ == nullptr)
 *              cond_.wait(lock);           // 必须用 while，不能用 if
 *      }                                   // （虚假唤醒：cond 可能没通知就返回）
 *      return loop_;
 *
 * 析构：
 *      if (thread_.joinable()) {
 *          loop_->quit();                  // 先让 loop 退出
 *          thread_.join();                 // 再等线程结束
 *      }
 *
 *    顺序反了会死锁：直接 join 的话，loop 还在 epoll_wait 里阻塞，
 *    线程永远不会结束。
 *
 * 还有一个容易忽略的点：loop_ 指向子线程栈上的对象。
 * 所以析构里必须保证 thread_ 已经 join，否则 loop_ 就是野指针。
 */

// EventLoopThread::EventLoopThread(const ThreadInitCallback& cb,
//                                  const std::string& name) { /* ... */ }
// EventLoopThread::~EventLoopThread() { /* ... */ }
// EventLoop* EventLoopThread::startLoop() { /* ... */ }
// void EventLoopThread::threadFunc() { /* ... */ }
