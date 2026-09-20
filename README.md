# cpp_server

WSL（Ubuntu 22.04）下的 C++ 练手工程，用来写 TCP 服务器。

## 目录

```
.
├── CMakeLists.txt          # 登记每个可执行程序，新增练习程序在这里加一行
├── src/
│   ├── my_tcp.cpp          # fork 版：一个连接一个子进程
│   ├── my_tcp_thread.cpp   # pthread 版：一个连接一个线程
│   ├── select.cpp          # select 版：单进程 I/O 多路复用
│   ├── wrap.h              # 各种系统调用的封装声明
│   └── wrap.c              # 上面这些函数的实现
└── .vscode/                # 构建任务与 gdb 调试配置
```

三个程序各有自己的 `main()`，对应三个可执行文件，可以同时存在、互不干扰：

| 可执行文件 | 源文件 | 并发模型 | 监听端口 |
| --- | --- | --- | --- |
| `build/cpp_server` | `src/my_tcp.cpp` | 一连接一进程（fork + SIGCHLD 回收） | 9999 |
| `build/cpp_server_thread` | `src/my_tcp_thread.cpp` | 一连接一线程（pthread + detach） | 8000 |
| `build/cpp_server_select` | `src/select.cpp` | 单进程单线程（select I/O 多路复用） | 9999 |

> 注意：select 版和 fork 版都监听 9999，**不能同时运行**，后启动的那个会报
> `Address already in use`。想同时跑，把其中一个的 `SRV_PORT` 改成别的端口。

## 在 WSL 终端里构建运行

```bash
cd ~/projects/cpp-server
cmake -S . -B build -G Ninja
cmake --build build            # 三个程序一起编译

./build/cpp_server             # fork 版，端口 9999
./build/cpp_server_thread      # 线程版，端口 8000
./build/cpp_server_select      # select 版，端口 9999
```

## 在 VS Code 里

- `Ctrl+Shift+B`：构建（三个程序都会编译）
- `F5`：调试，启动时可以选择「调试 cpp_server (fork 版)」「调试 cpp_server_thread (线程版)」
  或「调试 cpp_server_select (select 版)」
- `Ctrl+Shift+P` → `cmake configure` / `cmake build` 也可以手动触发任务

## 注意

- **一个可执行文件只能有一个 `main()`**。所以新增带 `main` 的 `.cpp` 时，要在
  `CMakeLists.txt` 里用 `add_server_program(名字 src/文件.cpp)` 单独登记一个目标，
  不要让它和已有的 `main` 混进同一个目标，否则链接会报
  `multiple definition of main`。
- 头文件（`.h`）不参与独立编译，代码要写在 `.c` / `.cpp` 里。
