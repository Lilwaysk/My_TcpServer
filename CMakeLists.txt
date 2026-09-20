cmake_minimum_required(VERSION 3.16)

project(cpp_server
    VERSION 0.2.0
    DESCRIPTION "WSL 下的 C++ TCP 服务器练习工程"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# 让 clangd / cpptools 能找到编译参数
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
endif()

# ---------------------------------------------------------------------------
# 共享源文件：不含 main() 的公共代码，每个可执行程序都会一起编译。
# ---------------------------------------------------------------------------
set(SHARED_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wrap.c)

# 头文件不参与编译，列在这里只是为了让 IDE 的工程视图里能看到它
set(SHARED_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wrap.h)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# 一个带 main() 的 .cpp 对应一个可执行文件。
#
# 以前是把 src/ 下所有源文件一起编译成单个程序，一旦有两个 main 就会报
# "multiple definition of main"。现在改成逐个登记，fork 版、线程版、select 版、
# epoll 版可以共存，各自单独构建、单独调试。
#
# 以后新增练习程序（比如 poll 版 src/poll.cpp），只要在下面加一行：
#     add_server_program(cpp_server_poll src/poll.cpp)
# 重新配置后就会出现新的构建目标，其他地方都不用改。
# ---------------------------------------------------------------------------
function(add_server_program target main_source)
    set(main_path ${CMAKE_CURRENT_SOURCE_DIR}/${main_source})
    if(NOT EXISTS ${main_path})
        message(FATAL_ERROR "找不到源文件: ${main_path}")
    endif()

    add_executable(${target}
        ${main_path}
        ${SHARED_SOURCES}
        ${SHARED_HEADERS})

    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

    target_compile_options(${target} PRIVATE -Wall -Wextra)

    target_link_libraries(${target} PRIVATE Threads::Threads)
endfunction()

add_server_program(cpp_server        src/my_tcp.cpp)         # 一连接一进程（fork 版）
add_server_program(cpp_server_thread src/my_tcp_thread.cpp)  # 一连接一线程（pthread 版）
add_server_program(cpp_server_select src/select.cpp)         # 单进程 I/O 多路复用（select 版）
add_server_program(cpp_server_epoll  src/epoll.cpp)          # 单进程 I/O 多路复用（epoll 版）

message(STATUS "构建目标:")
message(STATUS "  cpp_server         <- src/my_tcp.cpp        (端口 9999)")
message(STATUS "  cpp_server_thread  <- src/my_tcp_thread.cpp (端口 8000)")
message(STATUS "  cpp_server_select  <- src/select.cpp        (端口 9999)")
message(STATUS "  cpp_server_epoll   <- src/epoll.cpp         (端口 8989)")
