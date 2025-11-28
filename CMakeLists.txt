# 指定 CMake 最低版本要求（兼容大部分系统）
cmake_minimum_required(VERSION 3.10)

# 项目名称和版本
project(PP VERSION 1.0 LANGUAGES CXX)

# 设置 C++ 标准（必须 C++17，因为客户端用到 filesystem）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 设置编译选项（启用警告，优化等级 O2）
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -O2)
endif()

# 指定头文件搜索路径（让服务端/客户端能找到 include/protocol.h）
include_directories(${PROJECT_SOURCE_DIR}/include)

# 设置编译输出目录（所有可执行文件生成到 build/bin 下）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

# 编译服务端
add_executable(chat_server
    ${PROJECT_SOURCE_DIR}/src/server/server.cpp
)

# 编译客户端
add_executable(chat_client
    ${PROJECT_SOURCE_DIR}/src/client/client.cpp
)

# 链接依赖库（服务端和客户端都需要 pthread 多线程库）
target_link_libraries(chat_server
    PRIVATE pthread
)

target_link_libraries(chat_client
    PRIVATE pthread
)

# 跨平台兼容：Windows 下不需要链接 pthread（系统自带）
if(WIN32)
    target_link_libraries(chat_server PRIVATE ws2_32)  # Windows socket 库
    target_link_libraries(chat_client PRIVATE ws2_32)
endif()
