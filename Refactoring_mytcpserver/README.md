# mytcpserver —— 从 epoll 单文件到 C++ 网络库

这份 README 是重构期间的**文件地图**：每个文件放什么、依赖谁、按什么顺序写。

当前进度：**第 1、2 步已完成**（Timestamp / Logger / Buffer + 单元测试，可编译可运行）。

---

## 一、目标目录结构

```
mytcpserver/
├── CMakeLists.txt
├── README.md
├── .gitignore
│
├── include/                      # 头文件（对外接口）
│   ├── base/                     # 基础设施：与网络无关，可单独复用
│   │   ├── noncopyable.h         # 禁止拷贝的基类（C++11 后可用 = delete 替代）
│   │   ├── Timestamp.h           # 时间戳（微秒）              [已完成]
│   │   └── Logger.h              # 分级日志（线程安全）        [已完成]
│   └── net/                      # 网络库
│       ├── Callbacks.h           # 回调类型别名，避免头文件互相包含
│       ├── Buffer.h              # 输入/输出缓冲区             [已完成]
│       ├── Channel.h             # 一个 fd + 关注的事件 + 回调（= 你的 myevent_s）
│       ├── Poller.h              # epoll 封装，唯一碰 epoll_ctl / epoll_wait 的地方
│       ├── EventLoop.h           # 每线程一个的事件循环（= 你的 while(1)）
│       ├── Timer.h               # 定时器句柄
│       ├── TimerQueue.h          # 定时器（最小堆）
│       ├── Socket.h              # socket 的 RAII 封装
│       ├── InetAddress.h         # sockaddr_in 的封装
│       ├── Acceptor.h            # 只管 accept（= 你的 acceptconn）
│       ├── TcpConnection.h       # 一条连接（= recvdata / senddata 合体）
│       ├── TcpServer.h           # 服务器门面，用户只用这一个类
│       ├── EventLoopThread.h     # 起一个线程跑 EventLoop          （阶段 3）
│       └── EventLoopThreadPool.h # 主从 Reactor 的 IO 线程池        （阶段 3）
│
├── src/                          # 实现文件，目录与 include 一一对应
│   ├── base/
│   │   ├── Timestamp.cpp         [已完成]
│   │   └── Logger.cpp            [已完成]
│   └── net/
│       ├── Buffer.cpp            [已完成]
│       ├── Channel.cpp
│       ├── Poller.cpp
│       ├── EventLoop.cpp
│       ├── TimerQueue.cpp
│       ├── Socket.cpp
│       ├── InetAddress.cpp
│       ├── Acceptor.cpp
│       ├── TcpConnection.cpp
│       ├── TcpServer.cpp
│       ├── EventLoopThread.cpp
│       └── EventLoopThreadPool.cpp
│
├── examples/                     # 示例：库的使用者
│   └── echo_server.cpp           # 用 TcpServer 复刻你现在这个 echo 服务器
│
├── tests/                        # 单元测试：每个基础类配一个
│   ├── test_buffer.cpp           [已完成]
│   ├── test_timer.cpp
│   └── test_channel.cpp
│
└── build/                        # 编译产物，不进 git
```

---

## 二、三条必须守住的规矩

**1. 依赖单向，绝不反向**

```
examples/  ──►  net/  ──►  base/
```

`base` 不认识 `net`，`net` 不认识 `examples`。`TcpServer` 不该知道谁是 `echo_server`，
这样它才能被别的程序复用。

**2. `<sys/epoll.h>` 只能出现在 Poller.cpp 里**

这是整套设计里最重要的一条。所有 epoll 细节关在 `Poller` 内部，
外面只看到 `poll()` 和 `updateChannel()`。好处：

- 以后换 `poll` / `kqueue` / `io_uring`，只动一个文件；
- 上面所有类都不依赖 Linux 特有头文件，好测试、好移植；
- 你的 `Buffer` 单元测试现在能在 Windows 上直接跑，就是这个规矩带来的。

**3. 一个类一对文件，文件名 = 类名**

`TcpConnection.h` 里只有 `TcpConnection` 一个类。不要 `utils.h` / `common.h`
这种“什么都能塞”的文件 —— 它会变成所有依赖的交叉点。

---

## 三、现有代码 → 新类 的对照

| 你现在的东西 | 搬到哪 | 说明 |
| --- | --- | --- |
| `struct myevent_s` | `Channel` | 去掉 `buf/len`，缓冲区归 `Buffer` |
| `eventadd` / `eventdel` / `eventset` | `Channel::enableReading()` / `enableWriting()` / `disableAll()` | 内部调 `Poller::updateChannel` |
| `g_efd` + `epoll_wait` 主循环 | `Poller` + `EventLoop::loop()` | Poller 管 epoll，EventLoop 管节奏和定时器 |
| `buf` + `len` + `sendpos` | `Buffer` | 输入 + 输出缓冲，可扩容 |
| 最小堆 + `timer_expire_process` | `TimerQueue` | 接口改成“到期回调”，不只是关连接 |
| `acceptconn` | `Acceptor` | 内部仍是“循环 accept 到 EAGAIN” |
| `recvdata` / `senddata` | `TcpConnection::handleRead()` / `handleWrite()` | 一条连接的全部状态和逻辑 |
| `conn_close` | `TcpConnection::handleClose()` + 析构 | 生命周期交给 `shared_ptr` |
| `g_events[MAX_EVENTS+1]` | `TcpServer` 里的 `std::map<int, TcpConnectionPtr>` | 不用再手工找空槽位 |
| `main` 里的 `while(1)` | `EventLoop::loop()` | 主线程也跑一个 EventLoop |
| `printf` | `LOG_INFO` / `LOG_ERROR` | 带级别、带时间戳 |

---

## 四、实现顺序（每步都要能编译、能跑、能验收）

| 步骤 | 写什么 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| 1 | `Timestamp` / `Logger` / `noncopyable` | 日志带时间戳和级别，级别能过滤 | 完成 |
| 2 | **`Buffer`** + 单元测试 | 单测全过（含扩容和黏包场景） | 完成 |
| 3 | `Channel` + `Poller` | 写个 main 直接驱动这俩，收发包能通（先不引入 EventLoop） | 下一步 |
| 4 | `EventLoop` + `TimerQueue` | 把老代码的主循环搬进来，行为一致 | |
| 5 | `Socket` + `InetAddress` + `Acceptor` | 能 accept 新连接并打印对端地址 | |
| 6 | `TcpConnection` + `TcpServer` | **用 TcpServer 重写 echo，跑通且不回退** | |
| 7 | `EventLoopThread` + `EventLoopThreadPool` | 主从 Reactor，QPS 随核数上涨 | |

第 3 步之所以先做 `Channel` + `Poller` 而不是直接上 `EventLoop`，
是为了让你能用一个 30 行的 `main` 单独验证「事件分发」这段逻辑 —— 出问题时
范围小得多。这就是小步走的全部意义。

**第 6 步是分水岭**：跑通之后你就有了自己的网络库，后面加协议、加线程池都只在
`examples/` 层动手，不用再改库本身。

---

## 五、编译与测试

```bash
# 方式一：直接 g++（快速验证单个文件）
g++ -std=c++11 -Wall -Wextra -Iinclude \
    src/base/Timestamp.cpp src/base/Logger.cpp src/net/Buffer.cpp \
    tests/test_buffer.cpp -o build/test_buffer
./build/test_buffer

# 方式二：CMake
mkdir -p build && cd build
cmake .. && make -j
./test_buffer
```

当前测试结果：

```
testInitialState
testAppendAndRetrieve
testGrowKeepsData
testMakeSpaceMoveFront
testRetrieveMoreThanReadable

18 checks, 0 failed
```

---

## 六、暂时不要加的（等用到再说）

- `ThreadPool`：业务线程池，等 echo 改成有耗时业务时再加
- `AsyncLogging`：异步日志，等日志成为瓶颈时再加
- `Connector` / `TcpClient`：要写压测客户端时再加
- 协程 / 无锁队列 / `io_uring`：现在加只会让你更难定位 bug
