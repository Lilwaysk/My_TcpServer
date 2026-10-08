cmake_minimum_required(VERSION 3.10)
project(mytcpserver CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)     # 生成 compile_commands.json，给 clangd / VS Code 用

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)           # 先 Debug：压测出问题要靠 -g 才能看火焰图
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

#
# 网络库本体。
# 每写完一个类，就往 NET_SOURCES 里加一行 —— 这是唯一需要改的构建配置。
#
set(NET_SOURCES
    src/base/Timestamp.cpp
    src/base/Logger.cpp
    src/net/Buffer.cpp
    src/net/Channel.cpp             # 第 3 步
    src/net/Poller.cpp              # 第 3 步
    src/net/EventLoop.cpp           # 第 4 步
    src/net/TimerQueue.cpp          # 第 4 步
    src/net/Socket.cpp              # 第 5 步
    src/net/InetAddress.cpp         # 第 5 步
    src/net/Acceptor.cpp            # 第 5 步
    # src/net/TcpConnection.cpp      # 第 6 步
    # src/net/TcpServer.cpp          # 第 6 步
    # src/net/EventLoopThread.cpp    # 第 7 步
    # src/net/EventLoopThreadPool.cpp# 第 7 步
)

add_library(netlib STATIC ${NET_SOURCES})
target_include_directories(netlib PUBLIC ${CMAKE_SOURCE_DIR}/include)

# Linux 下需要线程库
find_package(Threads QUIET)
if(Threads_FOUND)
    target_link_libraries(netlib PUBLIC Threads::Threads)
endif()

#
# 测试：一个类一个可执行文件
#
add_executable(test_buffer tests/test_buffer.cpp)
target_link_libraries(test_buffer PRIVATE netlib)

#
# 第 3 步的测试：Channel + Poller。
#
add_executable(test_channel tests/test_channel.cpp)
target_link_libraries(test_channel PRIVATE netlib)

#
# 第 4 步的测试：EventLoop + TimerQueue。
#
add_executable(test_eventloop tests/test_eventloop.cpp)
target_link_libraries(test_eventloop PRIVATE netlib)

add_executable(test_timer tests/test_timer.cpp)
target_link_libraries(test_timer PRIVATE netlib)

#
# 第 5 步的测试：Socket + InetAddress + Acceptor。
#
add_executable(test_inetaddress tests/test_inetaddress.cpp)
target_link_libraries(test_inetaddress PRIVATE netlib)

add_executable(test_socket tests/test_socket.cpp)
target_link_libraries(test_socket PRIVATE netlib)

add_executable(test_acceptor tests/test_acceptor.cpp)
target_link_libraries(test_acceptor PRIVATE netlib)

#
# 示例：等第 6 步 TcpServer 写完再打开这个注释
#
# add_executable(echo_server examples/echo_server.cpp)
# target_link_libraries(echo_server PRIVATE netlib)
