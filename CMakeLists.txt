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
# 自动收集 src/ 下的所有源文件。
# 这样你以后新增 my_tcp2.cpp、wrap.c 之类的文件，不用改这个 CMakeLists，
# 重新构建时会自动一起编译（CONFIGURE_DEPENDS 让 CMake 自己去检测新文件）。
# ---------------------------------------------------------------------------
file(GLOB PROJECT_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

if(NOT PROJECT_SOURCES)
    message(FATAL_ERROR "src/ 下没有找到任何源文件")
endif()

add_executable(cpp_server ${PROJECT_SOURCES})

target_include_directories(cpp_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_options(cpp_server PRIVATE -Wall -Wextra)

find_package(Threads REQUIRED)
target_link_libraries(cpp_server PRIVATE Threads::Threads)

message(STATUS "参与编译的源文件:")
foreach(src ${PROJECT_SOURCES})
    message(STATUS "  ${src}")
endforeach()
