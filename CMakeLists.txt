cmake_minimum_required(VERSION 3.16)
project(PP)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 包含头文件目录
include_directories(${PROJECT_SOURCE_DIR}/include)

# --------------------------
# 1. 编译服务器（纯C++，禁用Qt AutoGen）
# --------------------------
add_executable(chat_server
    src/server/server.cpp
)

set_target_properties(chat_server PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_link_libraries(chat_server
    pthread
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

target_link_libraries(chat_client_qt
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)