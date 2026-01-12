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
#include <chrono> // 新增：心跳时间戳
#include <fcntl.h>
#include "../../include/protocol.h"
#include "../../include/json/json.hpp"

// 全局变量
std::map<int, UserInfo> g_onlineUsers;
int g_nextUserId = 1;
std::map<int, int> g_fdToUserId; 
std::mutex g_mutex; // 非递归锁，仅保护全局数据读写
// 新增：心跳相关
std::map<int, std::chrono::steady_clock::time_point> g_fdLastHeartbeat;
const int HEARTBEAT_TIMEOUT_SEC = 15;
std::thread g_heartbeatCheckThread;
bool g_isRunning = true;

// 函数声明
void broadcast(const std::string& data, uint32_t msgType, int excludeFd = -1);
void heartbeatCheckThread();
void handleClientThread(int clientFd, const std::string& clientIp);
int findFdByUserId(int userId);
std::string serializePrivateMsg(const PrivateMsg& msg);
std::string serializePrivateMsgRsp(const PrivateMsgRsp& rsp);
LoginReq deserializeLoginReq(const std::string& jsonStr);
std::string serializeLoginRsp(const LoginRsp& rsp);
std::string serializeUserListRsp(const UserListRsp& rsp);
std::vector<char> serializeUserListJson(const std::map<int, UserInfo>& users);
void handleUserListReq(int clientFd, const std::string& clientIp);
bool handleClient(int clientFd, const std::string& clientIp);


// 根据用户ID查找对应的客户端FD（加锁读取）
int findFdByUserId(int userId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& [fd, uid] : g_fdToUserId) {
        if (uid == userId) {
            return fd;
        }
    }
    return -1; // 用户不在线
}

// 序列化私聊消息
std::string serializePrivateMsg(const PrivateMsg& msg) {
    nlohmann::json j;
    j["senderId"] = msg.senderId;
    j["receiverId"] = msg.receiverId;
    j["content"] = msg.content;
    j["senderNickname"] = g_onlineUsers[msg.senderId].nickname; // 补充发送方昵称
    return j.dump();
}

// 序列化私聊响应
std::string serializePrivateMsgRsp(const PrivateMsgRsp& rsp) {
    nlohmann::json j;
    j["success"] = rsp.success;
    j["msg"] = rsp.msg;
    j["receiverId"] = rsp.receiverId;
    return j.dump();
}

// 心跳检测线程
void heartbeatCheckThread() {
    while (g_isRunning) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_fdLastHeartbeat.begin(); it != g_fdLastHeartbeat.end();) {
            int clientFd = it->first;
            auto lastTime = it->second;
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
            
            if (duration.count() > HEARTBEAT_TIMEOUT_SEC) {
                std::cout << "客户端FD=" << clientFd << " 心跳超时，判定为意外退出" << std::endl;
                int offlineUserId = -1;
                std::string offlineNickname = "";
                
                // 1. 先获取用户信息（避免erase后丢失）
                auto fdIt = g_fdToUserId.find(clientFd);
                if (fdIt != g_fdToUserId.end()) {
                    offlineUserId = fdIt->second;
                    auto userIt = g_onlineUsers.find(offlineUserId);
                    if (userIt != g_onlineUsers.end()) {
                        offlineNickname = userIt->second.nickname;
                        // 广播下线通知
                        UserStatusNotify notify;
                        notify.userId = offlineUserId;
                        notify.nickname = offlineNickname;
                        notify.isOnline = false;
                        std::string notifyData = serialize(notify);
                        broadcast(notifyData, static_cast<uint32_t>(MsgType::USER_STATUS_NOTIFY), clientFd);
                        
                        // 2. 彻底清理用户数据（关键）
                        g_onlineUsers.erase(userIt); // 删除用户信息
                        std::cout << "清理离线用户：ID=" << offlineUserId << "，昵称=" << offlineNickname << std::endl;
                    }
                    g_fdToUserId.erase(fdIt); // 删除FD映射
                }
                
                // 3. 关闭连接+清理心跳记录
                close(clientFd);
                it = g_fdLastHeartbeat.erase(it);
                std::cout << "心跳超时用户清理完成：FD=" << clientFd << std::endl;
            } else {
                ++it;
            }
        }
    }
}

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
    j["userCount"] = rsp.users.size(); // 补充client期望的userCount字段
    nlohmann::json userArray = nlohmann::json::array();
    for (const auto& user : rsp.users) {
        nlohmann::json userJson;
        userJson["userId"] = user.userId;
        userJson["nickname"] = user.nickname;
        userJson["isOnline"] = user.isOnline;
        userJson["ip"] = user.ip.empty() ? "" : user.ip; // 避免null
        userJson["dataPort"] = user.dataPort <= 0 ? 0 : user.dataPort; // 避免null
        userArray.push_back(userJson);
    }
    j["users"] = userArray;
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
    std::cout << "【用户列表请求】客户端IP：" << clientIp << "，FD：" << clientFd << std::endl;
    UserListRsp rsp;
    
    // 1. 读取在线用户
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& [id, user] : g_onlineUsers) {
            rsp.users.push_back(user);
            std::cout << "在线用户：ID=" << id << "，昵称=" << user.nickname << std::endl;
        }
    }

    // 2. 统一序列化（使用protocol.h的工具函数）
    std::string rspJson = serialize(rsp);
    std::cout << "用户列表响应JSON：" << rspJson << std::endl;

    // 3. 构造响应头部
    PacketHeader rspHeader;
    rspHeader.msgType = static_cast<uint32_t>(MsgType::USER_LIST_RSP);
    rspHeader.dataLen = rspJson.size();
    rspHeader.msgId = 0;
    rspHeader.senderId = 0;
    PacketHeader netRspHeader = htonHeader(rspHeader);

    // 4. 发送响应（检查错误）
    ssize_t headerSend = send(clientFd, &netRspHeader, sizeof(PacketHeader), MSG_NOSIGNAL);
    ssize_t dataSend = send(clientFd, rspJson.c_str(), rspJson.size(), MSG_NOSIGNAL);
    
    if (headerSend < 0 || dataSend < 0) {
        std::cout << "用户列表响应发送失败：header=" << headerSend << ", data=" << dataSend << "，错误：" << strerror(errno) << std::endl;
    } else {
        std::cout << "用户列表响应发送完成：header=" << headerSend << ", data=" << dataSend << "，在线用户数：" << rsp.users.size() << std::endl;
    }
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
        if (clientFd == excludeFd) {
            std::cout << "跳过排除的FD=" << clientFd << std::endl;
            continue;
        }
        // 设置FD为非阻塞，避免send卡住
        int flags = fcntl(clientFd, F_GETFL, 0);
        fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);

        // 发送头部（非阻塞）
        ssize_t headerSend = send(clientFd, &header, sizeof(PacketHeader), MSG_NOSIGNAL | MSG_DONTWAIT);
        // 发送数据（非阻塞）
        ssize_t dataSend = send(clientFd, data.c_str(), data.size(), MSG_NOSIGNAL | MSG_DONTWAIT);

        // 恢复FD为阻塞（不影响后续正常通信）
        fcntl(clientFd, F_SETFL, flags);

        // 日志优化
        if (headerSend == sizeof(PacketHeader) && dataSend == (ssize_t)data.size()) {
            std::cout << "广播成功：FD=" << clientFd << "，发送字节数：header=" << headerSend << "，data=" << dataSend << std::endl;
        } else {
            std::cout << "广播失败/部分发送：FD=" << clientFd << "，header=" << headerSend << "，data=" << dataSend << "，错误：" << (errno ? strerror(errno) : "无") << std::endl;
        }
    }
    std::cout << "broadcast end" << std::endl;
}

// 处理客户端连接（核心优化：锁仅包裹全局数据操作）
bool handleClient(int clientFd, const std::string& clientIp) {
    (void)clientIp;
    // 初始化心跳时间
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_fdLastHeartbeat[clientFd] = std::chrono::steady_clock::now();
    }

    while (true) {
        PacketHeader netHeader;
        ssize_t headerLen = recv(clientFd, &netHeader, sizeof(PacketHeader), 0);
        
        if (headerLen <= 0) {
            std::cout << "客户端FD=" << clientFd << " 断开连接或接收失败" << std::endl;
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
                        std::string notifyData = serialize(notify);
                        // broadcast内部已做局部拷贝，无需锁
                        broadcast(notifyData, static_cast<uint32_t>(MsgType::USER_STATUS_NOTIFY), clientFd);
                        g_onlineUsers.erase(userIt);
                    }
                    g_fdToUserId.erase(fdIt);
                }
                g_fdLastHeartbeat.erase(clientFd); // 清理心跳记录
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
                // 1. 解析请求（补充ip和dataPort的解析）
                std::string jsonData(dataBuffer.begin(), dataBuffer.end());
                nlohmann::json j = nlohmann::json::parse(jsonData);
                LoginReq req;
                req.nickname = j["nickname"];
                // 提取客户端ip和dataPort（客户端登录请求会携带）
                std::string clientIpStr = clientIp;
                int clientDataPort = j.contains("dataPort") ? j["dataPort"].get<int>() : -1;

                LoginRsp rsp;
                bool nicknameExists = false;
                int newUserId = -1;
                std::string newNickname = req.nickname;
                
                // 2. 加锁操作全局数据（补充ip/dataPort）
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    // 移除心跳超时但未清理的僵尸用户
                    auto now = std::chrono::steady_clock::now();
                    for (auto it = g_onlineUsers.begin(); it != g_onlineUsers.end();) {
                        int uid = it->first;
                        int fd = findFdByUserId(uid);
                        if (fd == -1) { // 无对应FD，判定为僵尸用户
                            std::cout << "清理僵尸用户：ID=" << uid << "，昵称=" << it->second.nickname << std::endl;
                            it = g_onlineUsers.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    
                    // 重新判定昵称（仅检查有效在线用户
                    for (const auto& [id, user] : g_onlineUsers) {
                        if (user.nickname == req.nickname && user.isOnline) {
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
                        newUser.ip = clientIpStr;       // 填充ip
                        newUser.dataPort = clientDataPort; // 填充dataPort
                        g_onlineUsers[newUserId] = newUser;
                        g_fdToUserId[clientFd] = newUserId;
                        // 初始化心跳时间
                        g_fdLastHeartbeat[clientFd] = std::chrono::steady_clock::now();
                    }
                }

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
                    std::string notifyData = serialize(notify);
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
                // 更新心跳时间
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_fdLastHeartbeat[clientFd] = std::chrono::steady_clock::now();
                }
                // 回复心跳响应
                PacketHeader rspHeader;
                rspHeader.msgType = htonl(static_cast<uint32_t>(MsgType::HEARTBEAT));
                rspHeader.dataLen = htonl(0);
                rspHeader.msgId = htonl(0);
                rspHeader.senderId = htonl(0);
                send(clientFd, &rspHeader, sizeof(PacketHeader), MSG_NOSIGNAL);
                break; // 注意：不要return false，仅break即可保持连接
            }
            case MsgType::COMMON_MSG: {
                std::cout << "收到普通消息（MsgType::COMMON_MSG），dataLen=" << dataLen << std::endl;
                
                // 1. 读取客户端发送的JSON，提取content字段（关键修复：避免二次封装）
                std::string clientMsgJson(dataBuffer.begin(), dataBuffer.end());
                std::string realContent = "";
                try {
                    nlohmann::json j = nlohmann::json::parse(clientMsgJson);
                    realContent = j["content"]; // 提取纯文本内容
                } catch (...) {
                    realContent = clientMsgJson; // 解析失败则用原始内容
                }

                // 2. 获取发送方的userId和nickname（加锁读取全局数据）
                int senderUserId = -1;
                std::string senderNickname = "未知用户";
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    auto fdIt = g_fdToUserId.find(clientFd);
                    if (fdIt != g_fdToUserId.end()) {
                        senderUserId = fdIt->second;
                        auto userIt = g_onlineUsers.find(senderUserId);
                        if (userIt != g_onlineUsers.end()) {
                            senderNickname = userIt->second.nickname;
                        }
                    }
                }
                
                // 3. 封装转发消息（仅包含发送方+纯文本，而非嵌套JSON）
                nlohmann::json forwardJson;
                forwardJson["senderNickname"] = senderNickname;
                forwardJson["content"] = realContent; // 纯文本，不是JSON字符串
                std::string forwardData = forwardJson.dump();
                
                // 4. 广播转发（排除发送方自己）
                broadcast(forwardData, static_cast<uint32_t>(MsgType::COMMON_MSG), clientFd);
                break;
            }
            case MsgType::PRIVATE_MSG: {
                std::cout << "收到私聊消息（MsgType::PRIVATE_MSG），dataLen=" << dataLen << std::endl;
                
                // 1. 解析私聊消息
                std::string clientMsgJson(dataBuffer.begin(), dataBuffer.end());
                PrivateMsg msg;
                try {
                    nlohmann::json j = nlohmann::json::parse(clientMsgJson);
                    msg.senderId = j["senderId"];
                    msg.receiverId = j["receiverId"];
                    msg.content = j["content"];
                } catch (...) {
                    std::cout << "私聊消息解析失败" << std::endl;
                    break;
                }

                // 2. 查找接收方FD（加锁）
                int receiverFd = findFdByUserId(msg.receiverId);
                PrivateMsgRsp rsp;
                rsp.receiverId = msg.receiverId;

                if (receiverFd == -1) {
                    // 接收方不在线，返回失败响应
                    rsp.success = false;
                    rsp.msg = "用户不在线，无法发送私聊消息";
                    std::string rspJson = serializePrivateMsgRsp(rsp);
                    
                    // 构造响应头部
                    PacketHeader rspHeader;
                    rspHeader.msgType = static_cast<uint32_t>(MsgType::PRIVATE_MSG_RSP);
                    rspHeader.dataLen = rspJson.size();
                    rspHeader.msgId = 0;
                    rspHeader.senderId = 0;
                    PacketHeader netRspHeader = htonHeader(rspHeader);
                    
                    // 发送响应给发送方
                    send(clientFd, &netRspHeader, sizeof(PacketHeader), MSG_NOSIGNAL);
                    send(clientFd, rspJson.c_str(), rspJson.size(), MSG_NOSIGNAL);
                    std::cout << "私聊失败：接收方ID=" << msg.receiverId << " 不在线" << std::endl;
                } else {
                    // 接收方在线，发送私聊消息
                    std::string forwardData = serializePrivateMsg(msg);
                    
                    // 构造私聊消息头部
                    PacketHeader header;
                    header.msgType = static_cast<uint32_t>(MsgType::PRIVATE_MSG);
                    header.dataLen = forwardData.size();
                    header.msgId = 0;
                    header.senderId = msg.senderId;
                    PacketHeader netHeader = htonHeader(header);
                    
                    // 仅向接收方FD发送
                    send(receiverFd, &netHeader, sizeof(PacketHeader), MSG_NOSIGNAL);
                    send(receiverFd, forwardData.c_str(), forwardData.size(), MSG_NOSIGNAL);
                    
                    // 返回成功响应给发送方
                    rsp.success = true;
                    rsp.msg = "私聊消息发送成功";
                    std::string rspJson = serializePrivateMsgRsp(rsp);
                    PacketHeader rspHeader;
                    rspHeader.msgType = static_cast<uint32_t>(MsgType::PRIVATE_MSG_RSP);
                    rspHeader.dataLen = rspJson.size();
                    rspHeader.msgId = 0;
                    rspHeader.senderId = 0;
                    PacketHeader netRspHeader = htonHeader(rspHeader);
                    send(clientFd, &netRspHeader, sizeof(PacketHeader), MSG_NOSIGNAL);
                    send(clientFd, rspJson.c_str(), rspJson.size(), MSG_NOSIGNAL);
                    
                    std::cout << "私聊成功：发送方ID=" << msg.senderId << " → 接收方ID=" << msg.receiverId << std::endl;
                }
                break;
            }
            // 新增：处理客户端主动下线通知
            case MsgType::USER_STATUS_NOTIFY: {
                std::cout << "收到客户端主动下线通知（FD=" << clientFd << "）" << std::endl;
                
                // 1. 先解析通知（解锁状态下）
                UserStatusNotify notify;
                try {
                    notify = deserialize<UserStatusNotify>(std::string(dataBuffer.begin(), dataBuffer.end()));
                    std::cout << "主动下线用户：ID=" << notify.userId << "，昵称=" << notify.nickname << std::endl;
                } catch (...) {
                    std::cout << "解析主动下线通知失败" << std::endl;
                    break;
                }

                // 2. 加锁仅处理必要操作，移除sleep
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    // 先广播（非阻塞）
                    std::string notifyData = serialize(notify);
                    std::cout << "开始广播主动下线通知：" << notifyData << std::endl;
                    broadcast(notifyData, static_cast<uint32_t>(MsgType::USER_STATUS_NOTIFY), clientFd);
                    
                    // 清理数据（精简）
                    g_fdToUserId.erase(clientFd);
                    g_onlineUsers.erase(notify.userId);
                    g_fdLastHeartbeat.erase(clientFd);
                } // 立即解锁，避免阻塞

                // 3. 关闭FD（非阻塞）
                shutdown(clientFd, SHUT_RDWR);
                close(clientFd);
                
                std::cout << "主动下线用户处理完成：ID=" << notify.userId << std::endl;
                return false;
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
 
    // 启动心跳检测线程
    g_isRunning = true;
    g_heartbeatCheckThread = std::thread(heartbeatCheckThread);
    g_heartbeatCheckThread.detach();
    std::cout << "心跳检测线程已启动，超时阈值：" << HEARTBEAT_TIMEOUT_SEC << "秒" << std::endl;

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

    // 退出时停止心跳线程
    g_isRunning = false;
    if (g_heartbeatCheckThread.joinable()) {
        g_heartbeatCheckThread.join();
    }
    
    close(serverFd);
    return 0;
}