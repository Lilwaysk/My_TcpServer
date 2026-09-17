# cpp_server

WSL（Ubuntu 22.04）下的 C++ 练手工程，用来写 TCP 服务器。

## 目录

```
.
├── CMakeLists.txt      # 自动收集 src/ 下所有 .c/.cpp，新增文件不用改它
├── src/
│   ├── my_tcp.cpp      # 你自己的代码，main() 在这里
│   ├── wrap.h          # 各种系统调用的封装声明
│   └── wrap.c          # 上面这些函数的实现
└── .vscode/            # 构建任务与 gdb 调试配置
```

## 在 WSL 终端里构建运行

```bash
cd ~/projects/cpp-server
cmake -S . -B build -G Ninja
cmake --build build
./build/cpp_server
```

## 在 VS Code 里

- `Ctrl+Shift+B`：构建
- `F5`：调试（launch.json 里已配好 gdb）
- `Ctrl+Shift+P` → `cmake configure` / `cmake build` 也可以手动触发任务

## 注意

- **可执行程序只能有一个 `main()`**。以后如果再加一个带 `main` 的
  `.cpp`，链接时会报 `multiple definition of main`，那时候把旧的那个
  改成别的函数名，或者一次只留一个。
- 头文件（`.h`）不参与独立编译，代码要写在 `.c` / `.cpp` 里。
