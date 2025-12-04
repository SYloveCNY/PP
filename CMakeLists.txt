cmake_minimum_required(VERSION 3.16)
project(PP VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ====================== 服务端（纯C++，无Qt依赖）======================
add_executable(chat_server
    src/server/server.cpp
    include/protocol.h
)
# 服务端仅依赖pthread（多线程），无Qt依赖
target_include_directories(chat_server PUBLIC include)
target_link_libraries(chat_server PRIVATE pthread)

# ====================== Qt客户端（单独依赖Qt）======================
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)
qt_standard_project_setup()

qt_add_executable(chat_client_qt
    src/client_qt/main.cpp
    src/client_qt/LoginWindow.cpp
    src/client_qt/ChatWindow.cpp
    include/protocol.h
)
target_include_directories(chat_client_qt PUBLIC include)
target_link_libraries(chat_client_qt PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    pthread
)

# 安装路径
install(TARGETS chat_server chat_client_qt
    RUNTIME DESTINATION bin
)