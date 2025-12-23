cmake_minimum_required(VERSION 3.5)

project(chat_server_client)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# 查找Qt6组件（必须包含Core和Widgets）
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

# 启用Qt MOC/UIC/RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# 源文件列表（根据实际文件调整）
set(SERVER_SOURCES
    src/server/server.cpp
    include/protocol_base.h
)

set(CLIENT_SOURCES
    src/client_qt/LoginWindow.cpp
    src/client_qt/ChatWindow.cpp
    src/client_qt/main.cpp
    include/protocol_base.h
    include/protocol_qt.h
)

# 服务端可执行文件
add_executable(chat_server ${SERVER_SOURCES})
target_include_directories(chat_server PRIVATE include)
target_link_libraries(chat_server PRIVATE pthread)

# 客户端可执行文件（链接Qt6模块）
add_executable(chat_client_qt ${CLIENT_SOURCES})
target_include_directories(chat_client_qt PRIVATE include)
# 必须链接Qt6::Core/Qt6::Widgets/Qt6::Network
target_link_libraries(chat_client_qt PRIVATE Qt6::Core Qt6::Widgets Qt6::Network)