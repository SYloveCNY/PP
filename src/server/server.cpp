#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <map>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <cerrno> // 新增：用于错误码打印
#include "../../include/protocol.h"
#include "../../include/json/json.hpp"

// 全局变量
std::map<int, UserInfo> g_onlineUsers;
int g_nextUserId = 1;
std::map<int, int> g_fdToUserId; 
std::mutex g_mutex; // 非递归锁，仅保护全局数据读写

void broadcast(const std::string& data, uint32_t msgType, int excludeFd = -1);
void handleClientThread(int clientFd, const std::string& clientIp);

// 登录请求反序列化（无全局操作，无需锁）
LoginReq deserializeLoginReq(const std::string& jsonStr) {
    nlohmann::json j = nlohmann::json::parse(jsonStr);
    LoginReq req;
    req.nickname = j["nickname"];
    return req;
}

// 登录响应序列化（无全局操作，无需锁）
std::string serializeLoginRsp(const LoginRsp& rsp) {
    nlohmann::json j;
    j["success"] = rsp.success;
    j["msg"] = rsp.msg;
    j["userId"] = rsp.userId;
    return j.dump();
}

// 用户列表响应序列化（无全局操作，无需锁）
std::string serializeUserListRsp(const UserListRsp& rsp) {
    nlohmann::json j;
    j["users"] = rsp.users;
    return j.dump();
}

// 序列化用户列表为JSON（无全局操作，无需锁）
std::vector<char> serializeUserListJson(const std::map<int, UserInfo>& users) {
    nlohmann::json j, data;
    j["msgType"] = static_cast<uint32_t>(MsgType::USER_LIST_RSP);
    data["userCount"] = users.size();
    
    nlohmann::json userArray = nlohmann::json::array();
    for (const auto& [userId, user] : users) {
        nlohmann::json userJson;
        userJson["userId"] = user.userId;
        userJson["nickname"] = user.nickname;
        userJson["isOnline"] = user.isOnline;
        userArray.push_back(userJson);
    }
    data["users"] = userArray;
    j["data"] = data;
    
    std::string jsonStr = j.dump();
    return std::vector<char>(jsonStr.begin(), jsonStr.end());
}

// 处理用户列表请求（仅在读取全局数据时加锁）
// 重构：处理用户列表请求（保证响应格式与登录响应一致）
void handleUserListReq(int clientFd, const std::string& clientIp) {
    (void)clientIp;
    UserListRsp rsp;
    
    // 1. 仅读取全局在线用户时加锁
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& [id, user] : g_onlineUsers) {
            rsp.users.push_back(user);
        }
    }

    // 2. 序列化用户列表响应（与登录响应格式统一）
    nlohmann::json j;
    j["userCount"] = rsp.users.size(); // 客户端可先读数量，再读列表
    nlohmann::json userArray = nlohmann::json::array();
    for (const auto& user : rsp.users) {
        nlohmann::json userJson;
        userJson["userId"] = user.userId;
        userJson["nickname"] = user.nickname;
        userJson["isOnline"] = user.isOnline;
        userArray.push_back(userJson);
    }
    j["users"] = userArray;
    std::string rspJson = j.dump();

    // 3. 构造响应头部（正确转换字节序）
    PacketHeader rspHeader;
    rspHeader.msgType = static_cast<uint32_t>(MsgType::USER_LIST_RSP);
    rspHeader.dataLen = rspJson.size();
    rspHeader.msgId = 0;
    rspHeader.senderId = 0;
    PacketHeader netRspHeader = htonHeader(rspHeader);

    // 4. 发送响应（加MSG_NOSIGNAL，检查返回值）
    ssize_t headerSend = send(clientFd, &netRspHeader, sizeof(PacketHeader), MSG_NOSIGNAL);
    ssize_t dataSend = send(clientFd, rspJson.c_str(), rspJson.size(), MSG_NOSIGNAL);
    std::cout << "用户列表响应发送完成：header=" << headerSend << ", data=" << dataSend << std::endl;
}

// 广播函数（仅在读取全局g_fdToUserId时加锁）
void broadcast(const std::string& data, uint32_t msgType, int excludeFd) {
    std::cout << "broadcast begin" << std::endl;
    
    // 【缩小锁范围】仅拷贝全局map时加锁，遍历/发送均解锁
    std::map<int, int> fdCopy; // 局部拷贝，避免遍历中map被修改
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        fdCopy = g_fdToUserId; // 仅拷贝全局数据，立即解锁
    }
    
    // 构造头部（无需锁）
    PacketHeader header;
    header.msgType = htonl(msgType);
    header.dataLen = htonl(data.size());
    header.msgId = htonl(0);
    header.senderId = htonl(0);

    // 遍历+发送（无需锁，用局部拷贝的fdCopy）
    for (const auto& [clientFd, userId] : fdCopy) {
        if (clientFd == excludeFd) continue;
        // 加MSG_NOSIGNAL避免SIGPIPE崩溃
        send(clientFd, &header, sizeof(PacketHeader), MSG_NOSIGNAL);
        send(clientFd, data.c_str(), data.size(), MSG_NOSIGNAL);
    }
    std::cout << "broadcast end" << std::endl;
}

// 处理客户端连接（核心优化：锁仅包裹全局数据操作）
bool handleClient(int clientFd, const std::string& clientIp) {
    (void)clientIp;
    
    while (true) {
        PacketHeader netHeader;
        ssize_t headerLen = recv(clientFd, &netHeader, sizeof(PacketHeader), 0);
        
        if (headerLen <= 0) {
            std::cout << "客户端断开连接或接收失败" << std::endl;
            // 【缩小锁范围】仅清理全局数据时加锁
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto fdIt = g_fdToUserId.find(clientFd);
                if (fdIt != g_fdToUserId.end()) {
                    int offlineUserId = fdIt->second;
                    auto userIt = g_onlineUsers.find(offlineUserId);
                    if (userIt != g_onlineUsers.end()) {
                        UserStatusNotify notify;
                        notify.userId = offlineUserId;
                        notify.nickname = userIt->second.nickname;
                        notify.isOnline = false;
                        nlohmann::json notifyJson = notify;
                        std::string notifyData = notifyJson.dump();
                        // broadcast内部已做局部拷贝，无需锁
                        broadcast(notifyData, static_cast<uint32_t>(MsgType::USER_STATUS_NOTIFY), clientFd);
                        g_onlineUsers.erase(userIt);
                    }
                    g_fdToUserId.erase(fdIt);
                }
            } // 立即解锁
            return false;
        }
        
        // 转换为主机字节序（无需锁）
        PacketHeader hostHeader = ntohHeader(netHeader);
        MsgType msgType = static_cast<MsgType>(hostHeader.msgType);
        uint32_t dataLen = hostHeader.dataLen;
        
        std::cout << "msgType = " << (int)msgType << ", dataLen = " << dataLen << std::endl;
        
        // 接收数据体（无需锁）
        std::vector<char> dataBuffer;
        if (dataLen > 0) {
            dataBuffer.resize(dataLen);
            ssize_t dataRecvLen = recv(clientFd, dataBuffer.data(), dataLen, 0);
            if (dataRecvLen != dataLen) {
                std::cout << "数据接收不完整" << std::endl;
                return false;
            }
        }
        
        // 处理不同消息类型
        switch (msgType) {
            case MsgType::LOGIN_REQ: {
                std::cout << "LOGIN_REQ" << std::endl;
                // 1. 解析请求（无需锁，局部操作）
                std::string jsonData(dataBuffer.begin(), dataBuffer.end());
                LoginReq req = deserializeLoginReq(jsonData);
                LoginRsp rsp;
                bool nicknameExists = false;
                int newUserId = -1;
                std::string newNickname;
                
                // 2. 【核心优化】仅访问/修改全局数据时加锁，访问完立即解锁
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    // 仅做：检查昵称（读全局）+ 创建用户（写全局）
                    for (const auto& [id, user] : g_onlineUsers) {
                        if (user.nickname == req.nickname) {
                            nicknameExists = true;
                            break;
                        }
                    }
                    if (!nicknameExists) {
                        newUserId = g_nextUserId++;
                        newNickname = req.nickname;
                        UserInfo newUser;
                        newUser.userId = newUserId;
                        newUser.nickname = newNickname;
                        newUser.isOnline = true;
                        g_onlineUsers[newUserId] = newUser;
                        g_fdToUserId[clientFd] = newUserId;
                    }
                } // 锁自动解锁，后续逻辑均无锁
                
                // 3. 处理登录结果（无需锁，局部操作）
                std::cout << "1..." << std::endl;
                if (nicknameExists) {
                    std::cout << "2..." << std::endl;
                    rsp.success = false;
                    rsp.msg = "昵称已存在，请更换昵称";
                    rsp.userId = -1;
                } else {
                    std::cout << "3..." << std::endl;
                    rsp.success = true;
                    rsp.msg = "登录成功";
                    rsp.userId = newUserId;

                    // 构造上线通知（无需锁）
                    std::cout << "4..." << std::endl;
                    UserStatusNotify notify;
                    notify.userId = newUserId;
                    notify.nickname = newNickname;
                    notify.isOnline = true;
                    nlohmann::json notifyJson = notify;
                    std::string notifyData = notifyJson.dump();
                    // broadcast内部已做局部拷贝，无需锁
                    broadcast(notifyData, static_cast<uint32_t>(MsgType::USER_STATUS_NOTIFY), clientFd);
                    std::cout << "5..." << std::endl;
                }
                
                // 4. 发送登录响应（无需锁）
                std::cout << "6..." << std::endl;
                std::string rspJson = serializeLoginRsp(rsp);
                PacketHeader rspHeader;
                rspHeader.msgType = static_cast<uint32_t>(MsgType::LOGIN_RSP);
                rspHeader.dataLen = rspJson.size();
                rspHeader.msgId = 0;
                rspHeader.senderId = rsp.userId;
                
                std::cout << "7..." << std::endl;
                PacketHeader netRspHeader = htonHeader(rspHeader);
                
                std::cout << "8..." << std::endl;
                // 发送响应+检查错误（无需锁）
                ssize_t headerSend = send(clientFd, &netRspHeader, sizeof(PacketHeader), MSG_NOSIGNAL);
                ssize_t dataSend = send(clientFd, rspJson.c_str(), rspJson.size(), MSG_NOSIGNAL);
                std::cout << "登录响应发送完成：header=" << headerSend << ", data=" << dataSend << std::endl;
                break;
            }
            case MsgType::USER_LIST_REQ: {
                std::cout << "USER_LIST_REQ" << std::endl;
                handleUserListReq(clientFd, clientIp);
                break;
            }
            case MsgType::HEARTBEAT: { // 新增：处理心跳包
                std::cout << "收到心跳包（MsgType::HEARTBEAT），保持连接" << std::endl;
                // 可选：回复心跳响应（若客户端需要）
                PacketHeader rspHeader;
                rspHeader.msgType = htonl(static_cast<uint32_t>(MsgType::HEARTBEAT));
                rspHeader.dataLen = htonl(0);
                rspHeader.msgId = htonl(0);
                rspHeader.senderId = htonl(0);
                send(clientFd, &rspHeader, sizeof(PacketHeader), MSG_NOSIGNAL);
                break; // 注意：不要return false，仅break即可保持连接
            }
            default:
                std::cout << "未知消息类型: " << (int)msgType << std::endl;
                break;
        }
    }
    return true;
}

// 线程入口函数
void handleClientThread(int clientFd, const std::string& clientIp) {
    handleClient(clientFd, clientIp);
    close(clientFd);
    std::cout << "客户端 " << clientIp << " 连接已关闭" << std::endl;
}

// 服务器主函数（无修改）
int main() {
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        perror("socket创建失败");
        return -1;
    }
    
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(8888);
    
    if (bind(serverFd, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("绑定失败");
        close(serverFd);
        return -1;
    }
    
    if (listen(serverFd, 10) < 0) {
        perror("监听失败");
        close(serverFd);
        return -1;
    }
    
    std::cout << "服务器启动，监听端口 8888...（多线程版本）" << std::endl;
    
    while (true) {
        sockaddr_in clientAddr{};
        socklen_t clientAddrLen = sizeof(clientAddr);
        int clientFd = accept(serverFd, (sockaddr*)&clientAddr, &clientAddrLen);
        
        if (clientFd < 0) {
            perror("接受连接失败");
            continue;
        }
        
        std::string clientIp = inet_ntoa(clientAddr.sin_addr);
        std::cout << "客户端 " << clientIp << " 连接成功，分配FD：" << clientFd << std::endl;
        
        std::thread t(handleClientThread, clientFd, clientIp);
        t.detach();
    }
    
    close(serverFd);
    return 0;
}