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
#include <chrono>
#include <stdexcept>
#include <atomic>
#include "protocol_base.h"

// 全局变量定义
std::map<int, UserInfo> gOnlineUsers;  // 键：userId（统一用用户ID作为键）
std::mutex gMutex;                     // 线程安全锁

// 函数声明
void sendLoginRsp(int clientFd, bool success, int userId, const std::string& msg);
void broadcastPacket(uint32_t msgType, const std::vector<char>& data, int excludeFd);
void handleLoginReq(int clientFd, const std::vector<char>& data);
void handleHeartbeat(int clientFd);
void handleCommonMsg(int clientFd, const std::vector<char>& data); // 补充声明
void heartbeatCheckThread();
bool recvCompletePacket(int fd, PacketHeader& header, std::vector<char>& data);
void updateUserHeartbeat(int userId); // 新增：更新用户心跳时间戳
int getUserIdByManagePort(int managePort); // 新增：通过managePort获取userId

// 广播数据包（参数类型统一为uint32_t，匹配PacketHeader的msgType）
void broadcastPacket(uint32_t msgType, const std::vector<char>& data, int excludeFd) {
    std::lock_guard<std::mutex> lock(gMutex);
    for (const auto& pair : gOnlineUsers) {
        int targetFd = pair.second.managePort;
        if (targetFd == excludeFd) continue; // 排除发送者
        if (!sendPacket(targetFd, msgType, data)) {
            std::cerr << "广播数据包到fd=" << targetFd << " 失败" << std::endl;
        }
    }
}

// 处理登录请求（修复心跳时间戳初始化）
void handleLoginReq(int clientFd, const std::vector<char>& data) {
    try {
        LoginReq req = deserializeLoginReq(data);
        if (req.nickname.empty()) {
            sendLoginRsp(clientFd, false, 0, "昵称不能为空");
            return;
        }

        // 分配用户ID（原子递增，避免多线程冲突）
        static std::atomic<int> gNextUserId(1);
        int userId = gNextUserId++;
        
        // 构造用户信息（初始化心跳时间戳）
        UserInfo user;
        user.userId = userId;
        user.nickname = req.nickname;
        user.avatar = req.avatar;
        user.dataPort = req.dataPort;
        user.managePort = clientFd;
        user.ip = "127.0.0.1"; // 实际场景需从clientAddr获取
        user.lastHeartbeatTime = std::chrono::system_clock::now(); // 初始化心跳时间

        // 加入在线用户列表（线程安全）
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gOnlineUsers[userId] = user;
        }

        // 发送登录成功响应
        sendLoginRsp(clientFd, true, userId, "登录成功");
        std::cout << "用户登录：userId=" << userId << "，nickname=" << req.nickname << "，fd=" << clientFd << std::endl;

        // 广播用户上线通知
        std::vector<char> notifyData = serializeUserInfo(user);
        broadcastPacket(USER_ONLINE_NOTIFY, notifyData, clientFd);
        
    } catch (const std::exception& e) {
        std::cerr << "处理登录请求失败：" << e.what() << std::endl;
        sendLoginRsp(clientFd, false, 0, "请求格式错误：" + std::string(e.what()));
    }
}

// 处理心跳包（更新心跳时间戳）
void handleHeartbeat(int clientFd) {
    int userId = getUserIdByManagePort(clientFd);
    if (userId == -1) {
        std::cerr << "心跳包来自未知客户端：fd=" << clientFd << std::endl;
        return;
    }

    // 更新心跳时间戳（线程安全）
    updateUserHeartbeat(userId);
    std::cout << "收到心跳：userId=" << userId << "，fd=" << clientFd << std::endl;

    // 回复心跳响应
    std::vector<char> emptyData;
    if (!sendPacket(clientFd, HEARTBEAT, emptyData)) {
        std::cerr << "回复心跳包失败：fd=" << clientFd << std::endl;
    }
}

// 处理普通消息（核心修复：转发消息，解决发送闪退）
void handleCommonMsg(int clientFd, const std::vector<char>& data) {
    try {
        CommonMsg msg = deserializeCommonMsg(data);
        std::cout << "转发消息：fromUserId=" << msg.fromUserId << "，content=" << msg.content.substr(0, 10) << "..." << std::endl;

        std::lock_guard<std::mutex> lock(gMutex);
        // 遍历所有在线用户，转发消息（广播：toUserId=0）
        for (const auto& [targetFd, targetUser] : gOnlineUsers) {
            // 跳过发送者自己
            if (targetFd == clientFd) continue;

            // 构造转发的数据包（复用原msg，无需修改）
            std::vector<char> msgData = serializeCommonMsg(msg);
            PacketHeader header;
            header.msgType = COMMON_MSG;
            header.dataLen = static_cast<uint32_t>(msgData.size());

            std::vector<char> sendData;
            sendData.insert(sendData.end(), reinterpret_cast<char*>(&header),
                            reinterpret_cast<char*>(&header) + sizeof(PacketHeader));
            sendData.insert(sendData.end(), msgData.begin(), msgData.end());

            // 发送给目标用户
            ssize_t sent = send(targetFd, sendData.data(), sendData.size(), 0);
            if (sent == -1) {
                std::cerr << "转发消息给fd=" << targetFd << "失败：" << strerror(errno) << std::endl;
            } else {
                std::cout << "已转发消息给fd=" << targetFd << "（" << targetUser.nickname << "）" << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "处理普通消息失败：" << e.what() << std::endl;
    }
}

// 心跳检测线程（修复：基于时间戳判断超时，默认60秒超时）
void heartbeatCheckThread() {
    const int TIMEOUT_SECONDS = 60; // 超时时间：60秒
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(30)); // 每30秒检测一次
        std::vector<int> offlineUserIds; // 存储离线用户ID

        // 1. 检测超时用户
        {
            std::lock_guard<std::mutex> lock(gMutex);
            auto now = std::chrono::system_clock::now();
            for (const auto& pair : gOnlineUsers) {
                int userId = pair.first;
                const auto& user = pair.second;
                // 计算时间差
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - user.lastHeartbeatTime);
                if (duration.count() > TIMEOUT_SECONDS) {
                    offlineUserIds.push_back(userId);
                    std::cout << "用户超时离线：userId=" << userId << "，nickname=" << user.nickname << std::endl;
                }
            }
        }

        // 2. 处理离线用户（广播通知+移除列表）
        for (int userId : offlineUserIds) {
            UserInfo offlineUser;
            {
                std::lock_guard<std::mutex> lock(gMutex);
                auto it = gOnlineUsers.find(userId);
                if (it == gOnlineUsers.end()) continue;
                offlineUser = it->second;
                gOnlineUsers.erase(it); // 从在线列表移除
            }

            // 广播下线通知
            std::vector<char> notifyData = serializeUserInfo(offlineUser);
            broadcastPacket(USER_OFFLINE_NOTIFY, notifyData, offlineUser.managePort);

            // 关闭客户端连接（若未关闭）
            close(offlineUser.managePort);
            std::cout << "已清理离线用户资源：userId=" << userId << "，fd=" << offlineUser.managePort << std::endl;
        }
    }
}

// 发送登录响应
void sendLoginRsp(int clientFd, bool success, int userId, const std::string& msg) {
    try {
        LoginRsp rsp;
        rsp.success = success;
        rsp.userId = userId;
        rsp.msg = msg;
        rsp.nickname = success ? gOnlineUsers[userId].nickname : "";

        // 序列化响应
        std::vector<char> rspData = serializeLoginRsp(rsp);
        // 发送响应（检查socket状态）
        if (!sendPacket(clientFd, LOGIN_RSP, rspData)) {
            std::cerr << "发送登录响应失败：fd=" << clientFd << "，success=" << success << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "序列化登录响应失败：" << e.what() << std::endl;
    }
}

// 完整读取数据包（确保不截断）
bool recvCompletePacket(int fd, PacketHeader& header, std::vector<char>& data) {
    // 读取头部（必须完整读取sizeof(PacketHeader)字节）
    ssize_t ret = recv(fd, &header, sizeof(PacketHeader), 0);
    if (ret == -1) {
        perror("recv header failed");
        return false;
    } else if (ret == 0) {
        std::cerr << "客户端断开连接：fd=" << fd << "（读取头部时）" << std::endl;
        return false;
    } else if (ret != sizeof(PacketHeader)) {
        std::cerr << "头部读取不完整：fd=" << fd << "，实际读取=" << ret << "，需要=" << sizeof(PacketHeader) << std::endl;
        return false;
    }

    // 读取数据部分（按header.dataLen读取完整）
    data.resize(header.dataLen);
    size_t recved = 0;
    while (recved < header.dataLen) {
        ret = recv(fd, data.data() + recved, header.dataLen - recved, 0);
        if (ret == -1) {
            perror("recv data failed");
            return false;
        } else if (ret == 0) {
            std::cerr << "客户端断开连接：fd=" << fd << "（读取数据时）" << std::endl;
            return false;
        }
        recved += ret;
    }
    return true;
}

// 更新用户心跳时间戳（线程安全）
void updateUserHeartbeat(int userId) {
    std::lock_guard<std::mutex> lock(gMutex);
    auto it = gOnlineUsers.find(userId);
    if (it != gOnlineUsers.end()) {
        it->second.lastHeartbeatTime = std::chrono::system_clock::now();
    }
}

// 通过managePort获取userId（线程安全）
int getUserIdByManagePort(int managePort) {
    std::lock_guard<std::mutex> lock(gMutex);
    for (const auto& pair : gOnlineUsers) {
        if (pair.second.managePort == managePort) {
            return pair.first;
        }
    }
    return -1; // 未找到
}

std::vector<char> serializeUserList(const std::map<int, UserInfo>& users) {
    std::vector<char> data;
    // 1. 序列化用户数量（注意网络字节序转换，与客户端deserializeUserList对应）
    uint32_t userCount = static_cast<uint32_t>(users.size());
    uint32_t networkCount = htonl(userCount); // 转为网络字节序
    data.insert(data.end(), reinterpret_cast<char*>(&networkCount), 
                reinterpret_cast<char*>(&networkCount) + sizeof(uint32_t));
    
    // 2. 逐个序列化用户信息（调用serializeUserInfo，需确保该函数已实现）
    for (const auto& [userId, user] : users) {
        std::vector<char> userData = serializeUserInfo(user);
        data.insert(data.end(), userData.begin(), userData.end());
    }
    return data;
}

// 服务端主函数（程序入口）
int main() {
    // 1. 创建监听Socket
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd == -1) {
        std::cerr << "创建监听Socket失败：" << strerror(errno) << std::endl;
        return 1;
    }

    // 2. 设置端口复用（避免重启时端口占用）
    int opt = 1;
    if (setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
        std::cerr << "setsockopt失败：" << strerror(errno) << std::endl;
        close(listenFd);
        return 1;
    }

    // 3. 绑定端口（8888）
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

    // 4. 开始监听（最大等待队列10）
    if (listen(listenFd, 10) == -1) {
        std::cerr << "监听失败：" << strerror(errno) << std::endl;
        close(listenFd);
        return 1;
    }

    std::cout << "服务端启动成功，监听端口：8888" << std::endl;

    // 5. 启动心跳检测线程
    std::thread heartbeatThread(heartbeatCheckThread);
    heartbeatThread.detach(); // 后台运行

    // 6. 初始化epoll（处理多客户端）
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
        std::cerr << "epoll添加监听Socket失败：" << strerror(errno) << std::endl;
        close(epollFd);
        close(listenFd);
        return 1;
    }

    const int MAX_EVENTS = 10;
    struct epoll_event events[MAX_EVENTS];

    // 7. 事件循环（核心修复：使用recvCompletePacket读取完整数据包）
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

                // 设置客户端Socket为非阻塞（适配边缘触发）
                int flags = fcntl(clientFd, F_GETFL, 0);
                if (fcntl(clientFd, F_SETFL, flags | O_NONBLOCK) == -1) {
                    std::cerr << "设置非阻塞失败：fd=" << clientFd << std::endl;
                    close(clientFd);
                    continue;
                }

                // 添加客户端Socket到epoll（边缘触发+ET模式）
                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = clientFd;
                if (epoll_ctl(epollFd, EPOLL_CTL_ADD, clientFd, &ev) == -1) {
                    std::cerr << "epoll添加客户端失败：fd=" << clientFd << std::endl;
                    close(clientFd);
                    continue;
                }

                std::cout << "新客户端连接：fd=" << clientFd 
                          << "，IP=" << inet_ntoa(clientAddr.sin_addr) 
                          << "，端口=" << ntohs(clientAddr.sin_port) << std::endl;

            } else {
                // 处理客户端数据（核心修复：使用recvCompletePacket读取完整数据包）
                int clientFd = events[i].data.fd;
                PacketHeader header;
                std::vector<char> payload;

                // 读取完整数据包
                if (!recvCompletePacket(clientFd, header, payload)) {
                    // 读取失败，清理资源
                    std::cerr << "清理异常客户端：fd=" << clientFd << std::endl;
                    epoll_ctl(epollFd, EPOLL_CTL_DEL, clientFd, nullptr);
                    close(clientFd);

                    // 从在线列表移除
                    int userId = getUserIdByManagePort(clientFd);
                    if (userId != -1) {
                        std::lock_guard<std::mutex> lock(gMutex);
                        gOnlineUsers.erase(userId);
                        // 广播下线通知
                        UserInfo offlineUser = gOnlineUsers[userId];
                        std::vector<char> notifyData = serializeUserInfo(offlineUser);
                        broadcastPacket(USER_OFFLINE_NOTIFY, notifyData, clientFd);
                    }
                    continue;
                }

                // 分发消息到对应处理函数
                switch (header.msgType) {
                    case LOGIN_REQ:
                        handleLoginReq(clientFd, payload);
                        break;
                    case HEARTBEAT: {
                        std::lock_guard<std::mutex> lock(gMutex);
                        // 根据clientFd找到对应的用户（假设gOnlineUsers是fd→UserInfo的映射，或需维护fd与userId的关联）
                        auto it = gOnlineUsers.find(clientFd);
                        if (it != gOnlineUsers.end()) {
                            it->second.lastHeartbeatTime = std::chrono::system_clock::now();
                            std::cout << "收到fd=" << clientFd << "的心跳，更新时间" << std::endl;
                        }
                        break;
                    }
                    case COMMON_MSG:
                        handleCommonMsg(clientFd, payload); // 新增：处理普通消息
                        break;
                    case USER_LIST_REQ: {
                        std::lock_guard<std::mutex> lock(gMutex);
                        std::cout << "准备发送用户列表给fd=" << clientFd << "，在线用户数：" << gOnlineUsers.size() << std::endl;
                        // 序列化在线用户列表（需实现serializeUserList函数）
                        std::vector<char> userListData = serializeUserList(gOnlineUsers);
                        // 发送用户列表响应
                        sendPacket(clientFd, USER_LIST_RSP, userListData);
                        std::cout << "已响应客户端 " << clientFd << " 的用户列表请求" << std::endl;
                        break;
                    }
                    default:
                        std::cout << "收到未知消息类型：" << header.msgType << "，fd=" << clientFd << std::endl;
                        break;
                }
            }
        }
    }

    // 清理资源
    close(epollFd);
    close(listenFd);
    std::cout << "服务端退出" << std::endl;
    return 0;
}