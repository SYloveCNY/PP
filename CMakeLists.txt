cmake_minimum_required(VERSION 3.16)  # Qt6 要求 CMake 最低 3.16
project(ChatServerClient)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -------------------------- 服务端配置（不变）--------------------------
add_executable(chat_server
    src/server/server.cpp
    include/protocol_base.h
)

target_link_libraries(chat_server PUBLIC pthread)

target_include_directories(chat_server PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/json
)

# -------------------------- 客户端配置（适配 Qt6）--------------------------
# 1. 查找 Qt6 组件（Qt6 组件名称与 Qt5 基本一致，部分调整）
find_package(Qt6 COMPONENTS Widgets Network REQUIRED)

# 2. 启用 Qt 自动工具（UI/ moc/ 资源文件）
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 3. 客户端源文件（包含入口文件 main.cpp）
add_executable(chat_client_qt
    src/client_qt/ChatWindow.h
    src/client_qt/ChatWindow.cpp
    src/client_qt/LoginWindow.h
    src/client_qt/LoginWindow.cpp
    src/client_qt/main.cpp  # 客户端入口，必须包含
    include/protocol_base.h
)

# 4. 链接 Qt6 库（Qt6::Widgets 对应界面，Qt6::Network 对应网络）
target_link_libraries(chat_client_qt PUBLIC 
    Qt6::Widgets 
    Qt6::Network
)

# 5. 包含头文件目录
target_include_directories(chat_client_qt PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)