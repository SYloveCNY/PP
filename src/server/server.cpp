// 先补充服务端需要的头文件
#include <iostream>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <map>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <chrono>   // 新增：心跳时间戳
#include "protocol_base.h"

// 全局变量定义
std::map<int, UserInfo> gOnlineUsers;  // 键：clientFd 或 userId（保持统一）
std::mutex gMutex;                     // 线程安全锁

// 函数声明（放在所有函数之前）
void sendLoginRsp(int clientFd, bool success, int userId, const std::string& msg);
void broadcastPacket(uint32_t msgType, const std::vector<char>& data, int excludeFd);
void handleLoginReq(int clientFd, const std::vector<char>& data);
void handleHeartbeat(int clientFd);
void heartbeatCheckThread();
bool recvCompletePacket(int fd, PacketHeader& header, std::vector<char>& data);

// 修复广播函数（managePort → dataPort/managePort）
void broadcastPacket(MsgType msgType, const std::vector<char>& data, int excludeFd) {
    std::lock_guard<std::mutex> lock(gMutex);
    for (const auto& pair : gOnlineUsers) {
        int fd = pair.second.managePort;
        if (fd == excludeFd) continue; // 排除自身
        if (!sendPacket(fd, msgType, data)) {
            std::cerr << "广播数据包到fd " << fd << " 失败" << std::endl;
        }
    }
}

// 修复登录请求处理函数
void handleLoginReq(int clientFd, const std::vector<char>& data) {
    try {
        LoginReq req = deserializeLoginReq(data);
        if (req.nickname.empty()) {
            sendLoginRsp(clientFd, false, 0, "昵称不能为空");
            return;
        }

        // 分配用户ID（示例逻辑）
        static int gNextUserId = 1;
        int userId = gNextUserId++;
        
        // 构造UserInfo
        UserInfo user;
        user.userId = userId;
        user.nickname = req.nickname;
        user.avatar = req.avatar;
        user.dataPort = req.dataPort;
        user.managePort = clientFd; // 示例：管理端口为客户端fd
        
        // 加入在线用户列表
        std::lock_guard<std::mutex> lock(gMutex);
        gOnlineUsers[userId] = user;
        
        // 发送登录成功响应
        sendLoginRsp(clientFd, true, userId, "登录成功");
        
        // 广播用户上线通知（现在serializeUserInfo已修复）
        std::vector<char> notifyData = serializeUserInfo(user);
        broadcastPacket(USER_ONLINE_NOTIFY, notifyData, clientFd);
        
    } catch (const std::exception& e) {
        std::cerr << "处理登录请求失败：" << e.what() << std::endl;
        sendLoginRsp(clientFd, false, 0, "请求格式错误：" + std::string(e.what()));
    }
}

// 修复心跳处理函数
void handleHeartbeat(int clientFd) {
    std::vector<char> emptyData;
    sendPacket(clientFd, HEARTBEAT, emptyData); // 回复心跳包
}

// 修复心跳检测线程
void heartbeatCheckThread() {
    while (true) {
        sleep(30); // 30秒检测一次
        std::lock_guard<std::mutex> lock(gMutex);
        // 示例逻辑：检测超时连接，广播下线通知
        for (auto it = gOnlineUsers.begin(); it != gOnlineUsers.end(); ) {
            // 假设检测逻辑...
            bool offline = true; // 示例：标记为下线
            if (offline) {
                UserInfo user = it->second;
                std::vector<char> notifyData = serializeUserInfo(user);
                broadcastPacket(USER_OFFLINE_NOTIFY, notifyData, user.managePort);
                it = gOnlineUsers.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// 补全sendLoginRsp函数（发送登录响应）
void sendLoginRsp(int clientFd, bool success, int userId, const std::string& msg) {
    LoginRsp rsp;
    rsp.success = success;
    rsp.userId = userId;
    rsp.msg = msg;
    rsp.nickname = success ? gOnlineUsers[userId].nickname : "";
    
    // 序列化响应
    std::vector<char> rspData = serializeLoginRsp(rsp);
    // 发送响应
    sendPacket(clientFd, LOGIN_RSP, rspData);
}

// server.cpp 新增：完整读取数据包函数
bool recvCompletePacket(int fd, PacketHeader& header, std::vector<char>& data) {
    // 读取头部
    ssize_t ret = recv(fd, &header, sizeof(PacketHeader), 0);
    if (ret == -1) {
        perror("recv header failed");
        return false;
    } else if (ret != sizeof(PacketHeader)) {
        std::cerr << "recv header incomplete" << std::endl;
        return false;
    }

    // 读取数据
    data.resize(header.dataLen);
    size_t recved = 0;
    while (recved < header.dataLen) {
        ret = recv(fd, data.data() + recved, header.dataLen - recved, 0);
        if (ret == -1) {
            perror("recv data failed");
            return false;
        } else if (ret == 0) {
            std::cerr << "connection closed" << std::endl;
            return false;
        }
        recved += ret;
    }
    return true;
}

// 服务端主函数（程序入口）
int main() {
    // 1. 创建监听Socket
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd == -1) {
        std::cerr << "创建监听Socket失败：" << strerror(errno) << std::endl;
        return 1;
    }

    // 2. 设置端口复用（避免服务器重启时端口占用）
    int opt = 1;
    if (setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
        std::cerr << "setsockopt失败：" << strerror(errno) << std::endl;
        close(listenFd);
        return 1;
    }

    // 3. 绑定端口（例如 8888）
    sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY; // 监听所有网卡
    serverAddr.sin_port = htons(8888);       // 服务端端口

    if (bind(listenFd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == -1) {
        std::cerr << "绑定端口失败：" << strerror(errno) << std::endl;
        close(listenFd);
        return 1;
    }

    // 4. 开始监听（最大等待队列长度 10）
    if (listen(listenFd, 10) == -1) {
        std::cerr << "监听失败：" << strerror(errno) << std::endl;
        close(listenFd);
        return 1;
    }

    std::cout << "服务端启动成功，监听端口：8888" << std::endl;

    // 5. 启动心跳检测线程
    std::thread heartbeatThread(heartbeatCheckThread);
    heartbeatThread.detach(); // 后台运行

    // 6. 使用epoll处理多客户端连接（事件循环）
    int epollFd = epoll_create1(0);
    if (epollFd == -1) {
        std::cerr << "epoll_create失败：" << strerror(errno) << std::endl;
        close(listenFd);
        return 1;
    }

    // 添加监听Socket到epoll
    struct epoll_event ev;
    ev.events = EPOLLIN; // 监听读事件
    ev.data.fd = listenFd;
    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, listenFd, &ev) == -1) {
        std::cerr << "epoll_ctl添加监听Socket失败：" << strerror(errno) << std::endl;
        close(epollFd);
        close(listenFd);
        return 1;
    }

    const int MAX_EVENTS = 10;
    struct epoll_event events[MAX_EVENTS];

    // 7. 事件循环（处理新连接和客户端数据）
    while (true) {
        int nfds = epoll_wait(epollFd, events, MAX_EVENTS, -1); // 阻塞等待事件
        if (nfds == -1) {
            std::cerr << "epoll_wait失败：" << strerror(errno) << std::endl;
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == listenFd) {
                // 处理新连接
                sockaddr_in clientAddr;
                socklen_t clientAddrLen = sizeof(clientAddr);
                int clientFd = accept(listenFd, (struct sockaddr*)&clientAddr, &clientAddrLen);
                if (clientFd == -1) {
                    std::cerr << "接受连接失败：" << strerror(errno) << std::endl;
                    continue;
                }

                // 设置客户端Socket为非阻塞
                fcntl(clientFd, F_SETFL, O_NONBLOCK);

                // 添加客户端Socket到epoll
                ev.events = EPOLLIN | EPOLLET; // 边缘触发（高效处理）
                ev.data.fd = clientFd;
                if (epoll_ctl(epollFd, EPOLL_CTL_ADD, clientFd, &ev) == -1) {
                    std::cerr << "epoll_ctl添加客户端Socket失败：" << strerror(errno) << std::endl;
                    close(clientFd);
                }

                std::cout << "新客户端连接：fd=" << clientFd 
                          << "，IP=" << inet_ntoa(clientAddr.sin_addr) << std::endl;

            } else {
                // 处理客户端数据（简化版：实际需读取完整数据包）
                int clientFd = events[i].data.fd;
                char buf[1024];
                ssize_t n = read(clientFd, buf, sizeof(buf));

                if (n <= 0) {
                    // 连接关闭或错误，移除客户端
                    std::cout << "客户端断开连接：fd=" << clientFd << std::endl;
                    epoll_ctl(epollFd, EPOLL_CTL_DEL, clientFd, nullptr);
                    close(clientFd);
                    // 从在线用户列表移除（需根据实际逻辑调整）
                    std::lock_guard<std::mutex> lock(gMutex);
                    for (auto it = gOnlineUsers.begin(); it != gOnlineUsers.end(); ++it) {
                        if (it->second.managePort == clientFd) {
                            gOnlineUsers.erase(it);
                            break;
                        }
                    }
                } else {
                    // 解析数据包（此处简化，实际需根据协议解析msgType并调用对应处理函数）
                    PacketHeader* header = reinterpret_cast<PacketHeader*>(buf);
                    std::vector<char> payload(buf + sizeof(PacketHeader), buf + n);

                    switch (header->msgType) {
                        case LOGIN_REQ:
                            handleLoginReq(clientFd, payload);
                            break;
                        case HEARTBEAT:
                            handleHeartbeat(clientFd);
                            break;
                        // 其他消息类型（如COMMON_MSG）的处理逻辑
                        default:
                            std::cout << "收到未知消息类型：" << header->msgType << std::endl;
                            break;
                    }
                }
            }
        }
    }

    // 清理资源
    close(epollFd);
    close(listenFd);
    return 0;
}