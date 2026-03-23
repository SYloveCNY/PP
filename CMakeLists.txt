cmake_minimum_required(VERSION 3.16)
project(PP)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 包含头文件目录
include_directories(${PROJECT_SOURCE_DIR}/include)

# --------------------------
# 1. 编译服务器（纯C++，禁用Qt AutoGen）
# --------------------------
# 关键修复1：先查找OpenSSL库（必须在link之前）
find_package(OpenSSL REQUIRED)
# 关键修复2：添加OpenSSL头文件目录（避免找不到evp.h）
include_directories(${OPENSSL_INCLUDE_DIR})

add_executable(chat_server
    src/server/server.cpp
)

set_target_properties(chat_server PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

# 关键修复3：改用${OPENSSL_LIBRARIES}通用链接方式（兼容所有OpenSSL版本）
target_link_libraries(chat_server
    pthread
    ${OPENSSL_LIBRARIES}  # 替换原OpenSSL::SSL/OpenSSL::Crypto
)

# --------------------------
# 2. 编译客户端（Qt依赖）
# --------------------------
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)
qt_standard_project_setup()

add_executable(chat_client_qt
    src/client_qt/main.cpp
    src/client_qt/LoginWindow.h
    src/client_qt/LoginWindow.cpp
    src/client_qt/LoginWindow.ui
    src/client_qt/ChatWindow.h   # 添加ChatWindow头文件
    src/client_qt/ChatWindow.cpp # 添加ChatWindow源文件
    src/client_qt/ChatWindow.ui # 添加.ui文件
)

# 确保Qt自动生成功能生效（可选，但建议加）
set_target_properties(chat_client_qt PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

target_link_libraries(chat_client_qt
    Qt6::Core
    Qt6::Widgets
    Qt6::Network  # 已正确添加，解决QUdpSocket未定义问题
)