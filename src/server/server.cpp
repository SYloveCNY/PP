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
bool sendPacket(int fd, uint32_t msgType, const std::vector<char>& data);
void sendLoginRsp(int clientFd, bool success, int userId, const std::string& msg);
void broadcastPacket(uint32_t msgType, const std::vector<char>& data, int excludeFd);
void handleLoginReq(int clientFd, const std::vector<char>& data);
void handleHeartbeat(int clientFd);
void heartbeatCheckThread();

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

// 补充：你需要实现的辅助函数（示例）
bool sendPacket(int fd, uint32_t msgType, const std::vector<char>& data) {
    PacketHeader header;
    header.msgType = msgType;
    header.dataLen = static_cast<uint32_t>(data.size());
    
    std::vector<char> sendData;
    sendData.insert(sendData.end(), reinterpret_cast<const char*>(&header),
                    reinterpret_cast<const char*>(&header) + sizeof(PacketHeader));
    sendData.insert(sendData.end(), data.begin(), data.end());
    
    ssize_t sent = send(fd, sendData.data(), sendData.size(), 0);
    if (sent == -1) {
        std::cerr << "发送数据包失败：" << strerror(errno) << std::endl;
        return false;
    }
    return true;
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

LoginReq deserializeLoginReq(const std::vector<char>& data) {
    LoginReq req;
    size_t offset = 0;
    req.nickname = deserializeString(data, offset);
    req.avatar = deserializeVector(data, offset);
    req.dataPort = *(uint16_t*)(data.data() + offset);
    return req;
}

std::vector<char> serializeUserInfo(const UserInfo& user) {
    std::vector<char> data;
    auto nicknameData = serializeString(user.nickname);
    data.insert(data.end(), nicknameData.begin(), nicknameData.end());
    auto avatarData = serializeVector(user.avatar);
    data.insert(data.end(), avatarData.begin(), avatarData.end());
    auto ipData = serializeString(user.ip);
    data.insert(data.end(), ipData.begin(), ipData.end());
    data.insert(data.end(), (char*)&user.userId, sizeof(int));
    data.insert(data.end(), (char*)&user.dataPort, sizeof(uint16_t));
    return data;
}