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
    src/net/Channel.cpp
    src/net/Poller.cpp
    src/net/EventLoop.cpp
    src/net/TimerQueue.cpp
    src/net/Socket.cpp
    src/net/InetAddress.cpp
    src/net/Acceptor.cpp
    src/net/TcpConnection.cpp
    src/net/EventLoopThread.cpp
    src/net/TcpServer.cpp
    src/net/EventLoopThreadPool.cpp
)

add_library(netlib STATIC ${NET_SOURCES})
target_include_directories(netlib PUBLIC ${CMAKE_SOURCE_DIR}/include)

find_package(Threads QUIET)
if(Threads_FOUND)
    target_link_libraries(netlib PUBLIC Threads::Threads)
endif()

add_executable(test_buffer tests/test_buffer.cpp)
target_link_libraries(test_buffer PRIVATE netlib)

add_executable(test_channel tests/test_channel.cpp)
target_link_libraries(test_channel PRIVATE netlib)

add_executable(test_eventloop tests/test_eventloop.cpp)
target_link_libraries(test_eventloop PRIVATE netlib)

add_executable(test_timer tests/test_timer.cpp)
target_link_libraries(test_timer PRIVATE netlib)

add_executable(test_inetaddress tests/test_inetaddress.cpp)
target_link_libraries(test_inetaddress PRIVATE netlib)

add_executable(test_socket tests/test_socket.cpp)
target_link_libraries(test_socket PRIVATE netlib)

add_executable(test_acceptor tests/test_acceptor.cpp)
target_link_libraries(test_acceptor PRIVATE netlib)

add_executable(test_eventloopthread tests/test_eventloopthread.cpp)
target_link_libraries(test_eventloopthread PRIVATE netlib)

add_executable(echo_server examples/echo_server.cpp)
target_link_libraries(echo_server PRIVATE netlib)

set(CHATPROTO_SOURCES
    src/protocol/Codec.cpp
)

add_library(chatproto STATIC ${CHATPROTO_SOURCES})
target_include_directories(chatproto PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(chatproto PUBLIC netlib)

add_executable(test_codec tests/test_codec.cpp)
target_link_libraries(test_codec PRIVATE chatproto)

add_executable(protocol_echo_server examples/protocol_echo_server.cpp)
target_link_libraries(protocol_echo_server PRIVATE chatproto)

#
# M2/M4 database and business base.
#
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQLCLIENT REQUIRED mysqlclient)

add_library(m2base STATIC
    src/util/Config.cpp
    src/dao/Database.cpp
    src/dao/UserDao.cpp
    src/dao/FriendDao.cpp
    src/dao/MessageDao.cpp
    src/service/UserService.cpp
)
target_include_directories(m2base PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${MYSQLCLIENT_INCLUDE_DIRS})
target_link_directories(m2base PUBLIC ${MYSQLCLIENT_LIBRARY_DIRS})
target_link_libraries(m2base PUBLIC ${MYSQLCLIENT_LIBRARIES})

add_executable(chatserver
    examples/chatserver.cpp
    src/session/SessionManager.cpp
    src/service/FriendService.cpp
    src/service/MessageService.cpp
)
target_link_libraries(chatserver PRIVATE chatproto m2base)
