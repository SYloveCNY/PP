#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/epoll.h>  // epoll 核心头文件
#include <errno.h>      // errno 头文件
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
std::map<int, std::chrono::steady_clock::time_point> g_fdLastHeartbeat;
const int HEARTBEAT_TIMEOUT_SEC = 15;
std::thread g_heartbeatCheckThread;
bool g_isRunning = true;
int m_udpRelaySocket = -1;                          // UDP中继套接字（原生fd）
int m_tcpRelayServer = -1;                          // TCP中继服务端套接字
std::map<int, UserRelayInfo> m_userRelayMap;        // 用户ID -> 中继信息

// ==================== 核心广播/线程函数声明 ====================
// 对外接口（带默认参数，注意：默认参数只能在声明中出现一次）
void broadcast(const std::string& data, uint32_t msgType, int excludeFd = -1);
// 内部重载函数（无默认参数，避免冲突）
void broadcast(const std::string& data, uint32_t msgType, int excludeFd, const std::map<int, int>& fdCopy);

// 心跳检测线程
void heartbeatCheckThread();
// 客户端处理线程（每个客户端一个线程）
void handleClientThread(int clientFd, const std::string& clientIp);

// ==================== FD-UID映射相关 ====================
// 根据用户ID查找对应的文件描述符
int findFdByUserId(int userId);
// 安全获取FD-UID映射的拷贝（避免多线程竞争）
std::map<int, int> getFdToUserIdCopy();

// ==================== 序列化/反序列化函数声明 ====================
// 私有消息序列化
std::string serializePrivateMsg(const PrivateMsg& msg);
std::string serializePrivateMsgRsp(const PrivateMsgRsp& rsp);

// 登录相关反序列化/序列化
LoginReq deserializeLoginReq(const std::string& jsonStr);
std::string serializeLoginRsp(const LoginRsp& rsp);

// 用户列表相关序列化
std::string serializeUserListRsp(const UserListRsp& rsp);
std::vector<char> serializeUserListJson(const std::map<int, UserInfo>& users);

// ==================== 补充之前缺失的反序列化函数声明（关键） ====================
// 图片消息反序列化（解决编译错误）
ImageMsg deserializeImageMsg(const std::string& data);
// 文件消息反序列化（解决编译错误）
FileMsg deserializeFileMsg(const std::string& data);
// P2P地址通知反序列化（解决编译错误）
P2PAddrNotify deserializeP2PAddrNotify(const std::string& data);

// ==================== 业务处理函数 ====================
// 处理用户列表请求
void handleUserListReq(int clientFd, const std::string& clientIp);
// 核心客户端处理逻辑（主函数）
bool handleClient(int clientFd, const std::string& clientIp);
// 检查FD是否有效（文件描述符合法性）
bool isFdValid(int fd);
// 清理超时的僵尸用户（移到锁外执行，优化性能）
void cleanZombieUsers();

// 内网穿透
// 服务端新增UDP中继处理
void onUdpReadyRead();
// 服务端新增TCP中继处理
void onTcpRelayNewConnection();

// 检查FD是否有效（核心：判断旧FD是否真的失效）
bool isFdValid(int fd) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        return false;
    }
    return err == 0;
}

// 优化：一次性拷贝FD-UID映射，避免多次加锁
std::map<int, int> getFdToUserIdCopy() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_fdToUserId;
}

// 优化：根据UID找FD（使用拷贝的映射，避免锁内循环调用）
int findFdByUserId(int userId) {
    auto fdCopy = getFdToUserIdCopy();
    for (const auto& [fd, uid] : fdCopy) {
        if (uid == userId) {
            return fd;
        }
    }
    return -1; // 用户不在线
}

// 优化：僵尸用户清理（锁外遍历，批量删除）
void cleanZombieUsers() {
    auto fdCopy = getFdToUserIdCopy();
    std::vector<int> invalidFds;
    
    // 1. 锁外检查FD有效性
    for (const auto& [fd, uid] : fdCopy) {
        if (!isFdValid(fd)) {
            invalidFds.push_back(fd);
        }
    }
    
    // 2. 批量加锁删除无效FD和用户
    if (!invalidFds.empty()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (int fd : invalidFds) {
            auto uidIt = g_fdToUserId.find(fd);
            if (uidIt != g_fdToUserId.end()) {
                int uid = uidIt->second;
                g_onlineUsers.erase(uid);
                g_fdToUserId.erase(uidIt);
                g_fdLastHeartbeat.erase(fd);
                std::cout << "清理僵尸用户：FD=" << fd << "，UID=" << uid << std::endl;
            }
        }
    }
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
        cleanZombieUsers(); // 复用清理逻辑
        
        std::lock_guard<std::mutex> lock(g_mutex);
        
        // 先收集超时FD，避免遍历中修改map
        std::vector<int> timeoutFds;
        auto now = std::chrono::steady_clock::now();
        
        for (const auto& [fd, lastTime] : g_fdLastHeartbeat) {
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
            
            if (duration.count() > HEARTBEAT_TIMEOUT_SEC) {
                timeoutFds.push_back(fd);
            }
        }
        
        // 批量清理超时FD
        for (int clientFd : timeoutFds) {
            std::cout << "客户端FD=" << clientFd << " 心跳超时，判定为意外退出" << std::endl;
            int offlineUserId = -1;
            std::string offlineNickname = "";
            
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
                    std::string notifyData = nlohmann::json(notify).dump(); // 修复序列化
                    broadcast(notifyData, static_cast<uint32_t>(MsgType::USER_STATUS_NOTIFY), clientFd);
                    g_onlineUsers.erase(userIt);
                    std::cout << "清理离线用户：ID=" << offlineUserId << "，昵称=" << offlineNickname << std::endl;
                }
                g_fdToUserId.erase(fdIt);
            }
            
            if (isFdValid(clientFd)) {
                close(clientFd);
            }
            g_fdLastHeartbeat.erase(clientFd);
            std::cout << "心跳超时用户清理完成：FD=" << clientFd << std::endl;
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

// 处理用户列表请求
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
    std::string rspJson = serializeUserListRsp(rsp);
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

// 广播函数：仅接收已拷贝的FD映射，不在内部加锁
void broadcast(const std::string& data, uint32_t msgType, int excludeFd, const std::map<int, int>& fdCopy) {
    std::cout << "broadcast begin" << std::endl;
    
    // 构造主机字节序头部，再转换为网络字节序（符合协议规范）
    PacketHeader hostHeader;
    hostHeader.msgType = msgType;
    hostHeader.dataLen = data.size();
    hostHeader.msgId = 0;
    hostHeader.senderId = 0;
    PacketHeader netHeader = htonHeader(hostHeader); // 使用封装的转换函数

    // 遍历FD发送（无锁）
    for (const auto& [clientFd, userId] : fdCopy) {
        if (clientFd == excludeFd) {
            std::cout << "跳过排除的FD=" << clientFd << std::endl;
            continue;
        }

        if (!isFdValid(clientFd)) {
            std::cout << "FD=" << clientFd << " 已失效，跳过广播" << std::endl;
            continue;
        }

        // 非阻塞发送，处理EAGAIN错误
        ssize_t headerSend = send(clientFd, &netHeader, sizeof(PacketHeader), MSG_NOSIGNAL | MSG_DONTWAIT);
        ssize_t dataSend = send(clientFd, data.c_str(), data.size(), MSG_NOSIGNAL | MSG_DONTWAIT);

        if (headerSend < 0 || dataSend < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) { // 忽略非阻塞无数据的错误
                std::cout << "广播失败：FD=" << clientFd << "，错误：" << strerror(errno) << std::endl;
            }
        }
    }
    std::cout << "broadcast end" << std::endl;
}

// 对外暴露的广播接口（先拷贝FD，再调用无锁广播）
void broadcast(const std::string& data, uint32_t msgType, int excludeFd) {
    auto fdCopy = getFdToUserIdCopy(); // 这里加锁拷贝，仅一次
    broadcast(data, msgType, excludeFd, fdCopy); // 无锁广播
}

// ========== 修复：非阻塞接收数据体（重试接收，避免直接断开） ==========
bool recvAll(int fd, char* buf, size_t len) {
    size_t totalRecv = 0;
    while (totalRecv < len) {
        ssize_t n = recv(fd, buf + totalRecv, len - totalRecv, 0); // 改为阻塞接收
        if (n < 0) {
            return false;
        } else if (n == 0) {
            return false;
        }
        totalRecv += n;
    }
    return true;
}

// 处理客户端连接
bool handleClient(int clientFd, const std::string& clientIp) {
    (void)clientIp;

    PacketHeader netHeader;
    // 非阻塞recv（MSG_DONTWAIT）
    ssize_t headerLen = recv(clientFd, &netHeader, sizeof(PacketHeader), MSG_DONTWAIT);
    
    // 明确区分正常断开/异常断开/非阻塞无数据
    if (headerLen == 0) {
        std::cout << "客户端FD=" << clientFd << " 正常断开" << std::endl;
        // 清理用户：先锁内读取+清理数据，再锁外广播
        int offlineUserId = -1;
        std::string offlineNickname = "";
        std::string notifyData; // 锁内构造通知数据，锁外广播
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto fdIt = g_fdToUserId.find(clientFd);
            if (fdIt != g_fdToUserId.end()) {
                offlineUserId = fdIt->second;
                auto userIt = g_onlineUsers.find(offlineUserId);
                if (userIt != g_onlineUsers.end()) {
                    offlineNickname = userIt->second.nickname;
                    std::cout << "准备广播下线通知：UID=" << offlineUserId << "，昵称=" << offlineNickname << std::endl;
                    // 构造下线通知（锁内完成，避免重复加锁）
                    UserStatusNotify notify;
                    notify.userId = offlineUserId;
                    notify.nickname = offlineNickname;
                    notify.isOnline = false;
                    notifyData = nlohmann::json(notify).dump();
                    
                    // 立即清理用户数据（核心：避免残留导致重复登录）
                    g_onlineUsers.erase(userIt);
                    std::cout << "清理正常断开用户：ID=" << offlineUserId << "，昵称=" << offlineNickname << std::endl;
                }
                // 清理FD映射和心跳记录
                g_fdToUserId.erase(fdIt);
                g_fdLastHeartbeat.erase(clientFd);
            }
        }

        // 锁外执行广播（关键：避免嵌套加锁死锁）
        if (!notifyData.empty()) {
            broadcast(notifyData, static_cast<uint32_t>(MsgType::USER_STATUS_NOTIFY), clientFd);
        }

        close(clientFd);
        return false;
    } else if (headerLen < 0) {
    // 非阻塞无数据（EAGAIN/EWOULDBLOCK）：直接返回，不关闭FD，后续epoll会再次触发
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return true; // 关键：返回true，保留FD，不阻塞
        }
        // 真正的异常断开
        std::cout << "客户端FD=" << clientFd << " 异常断开，错误：" << strerror(errno) << std::endl;
        int offlineUserId = -1;
        std::string offlineNickname = "";
        std::string notifyData;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto fdIt = g_fdToUserId.find(clientFd);
            if (fdIt != g_fdToUserId.end()) {
                // 修复：移除重复定义的offlineUserId，避免外部变量未赋值
                offlineUserId = fdIt->second;
                auto userIt = g_onlineUsers.find(offlineUserId);
                if (userIt != g_onlineUsers.end()) {
                    offlineNickname = userIt->second.nickname;
                    UserStatusNotify notify;
                    notify.userId = offlineUserId;
                    notify.nickname = offlineNickname;
                    notify.isOnline = false;
                    notifyData = nlohmann::json(notify).dump();
                    g_onlineUsers.erase(userIt);
                }
                g_fdToUserId.erase(fdIt);
                g_fdLastHeartbeat.erase(clientFd);
            }
        }

        // 锁外广播异常下线通知
        if (!notifyData.empty()) {
            broadcast(notifyData, static_cast<uint32_t>(MsgType::USER_STATUS_NOTIFY), clientFd);
        }

        close(clientFd);
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
        // 重试接收数据体，避免单次接收失败就断开
        if (!recvAll(clientFd, dataBuffer.data(), dataLen)) {
            std::cout << "数据接收失败，客户端FD=" << clientFd << std::endl;
            close(clientFd);
            return false;
        }
    }
    
    std::string dataBufferStr(dataBuffer.begin(), dataBuffer.end());
    
    // 处理不同消息类型
    switch (msgType) {
        case MsgType::LOGIN_REQ: {
            std::cout << "LOGIN_REQ" << std::endl;
            // 1. 解析请求（补充：udpPort的解析，优化端口初始值）
            std::string jsonData(dataBuffer.begin(), dataBuffer.end());
            nlohmann::json j = nlohmann::json::parse(jsonData);
            LoginReq req;
            req.nickname = j["nickname"];
            
            // 提取客户端携带的端口（补充：udpPort解析，初始值改为0更合理）
            std::string clientIpStr = clientIp;
            uint16_t clientDataPort = j.contains("dataPort") ? j["dataPort"].get<uint16_t>() : 0;  // TCP端口（P2P服务端）
            uint16_t clientUdpPort = j.contains("udpPort") ? j["udpPort"].get<uint16_t>() : 0;    // 新增：解析UDP端口（客户端真实绑定）

            std::cout << "解析到客户端端口：dataPort=" << clientDataPort << "，udpPort=" << clientUdpPort << std::endl;
            
            LoginRsp rsp;
            bool nicknameExists = false;
            int newUserId = -1;
            std::string newNickname = req.nickname;
            
            // 1. 锁外清理僵尸用户
            cleanZombieUsers();
            
            // 2. 最小化锁范围：仅检查昵称+添加用户
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                // 检查昵称是否存在
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
                    newUser.isOnline = true;
                    newUser.ip = clientIpStr;       
                    newUser.dataPort = clientDataPort; // 原有：填充TCP端口
                    newUser.udpPort = clientUdpPort;   // 新增：填充真实UDP端口（关键修改）
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
                std::string notifyData = nlohmann::json(notify).dump();
                // 锁外广播上线通知
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
        case MsgType::HEARTBEAT: {
            std::cout << "收到心跳包（FD=" << clientFd << "）" << std::endl;
            // 新增：解析心跳包中携带的最新P2P端口（dataBufferStr是心跳包的data体）
            uint16_t newDataPort = 0;
            uint16_t newUdpPort = 0;
            if (!dataBufferStr.empty()) { // 心跳包有数据（携带端口）
                try {
                    nlohmann::json j = nlohmann::json::parse(dataBufferStr);
                    newDataPort = j.contains("dataPort") ? j["dataPort"].get<uint16_t>() : 0;
                    newUdpPort = j.contains("udpPort") ? j["udpPort"].get<uint16_t>() : 0;
                } catch (...) {
                    std::cout << "心跳包端口解析失败，忽略" << std::endl;
                }
            }

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                // 1. 更新心跳时间戳（原有逻辑保留）
                g_fdLastHeartbeat[clientFd] = std::chrono::steady_clock::now();
                
                // 2. 新增核心逻辑：更新全局存储的端口（如果客户端携带了新端口）
                if (newDataPort > 0) { // 仅当端口有效时更新
                    auto fdIt = g_fdToUserId.find(clientFd);
                    if (fdIt != g_fdToUserId.end()) {
                        int userId = fdIt->second;
                        auto userIt = g_onlineUsers.find(userId);
                        if (userIt != g_onlineUsers.end()) {
                            // 替换旧端口为心跳包中的最新端口
                            userIt->second.dataPort = newDataPort;
                            userIt->second.udpPort = newUdpPort;
                            std::cout << "更新用户端口：UID=" << userId 
                                    << "，旧端口=" << userIt->second.dataPort 
                                    << "→新端口=" << newDataPort << std::endl;
                        }
                    }
                }
            }

            // 心跳响应逻辑（原有逻辑保留）
            PacketHeader rspHeader;
            rspHeader.msgType = static_cast<uint32_t>(MsgType::HEARTBEAT);
            rspHeader.dataLen = 0;
            rspHeader.msgId = 0;
            rspHeader.senderId = 0;
            PacketHeader netRspHeader = htonHeader(rspHeader);
            send(clientFd, &netRspHeader, sizeof(PacketHeader), MSG_NOSIGNAL);
            break;
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
        // 新增：处理图片消息
        case MsgType::IMAGE_MSG: {
            std::cout << "收到图片消息（MsgType::IMAGE_MSG），dataLen=" << dataLen << std::endl;
            ImageMsg imgMsg = deserializeImageMsg(dataBufferStr);
            
            // 1. 查找接收方FD和地址信息
            int receiverFd = findFdByUserId(imgMsg.receiverId);
            ImageMsgRsp rsp;
            rsp.success = false;
            rsp.msg = "";
            rsp.receiverId = imgMsg.receiverId;
            
            if (receiverFd == -1) {
                rsp.msg = "接收方不在线，无法发送图片";
            } else {
                // 2. 获取接收方的IP和端口（从g_onlineUsers中读取）
                std::string receiverIp;
                uint16_t receiverTcpPort = 0;
                uint16_t receiverUdpPort = 0;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    auto userIt = g_onlineUsers.find(imgMsg.receiverId);
                    if (userIt != g_onlineUsers.end()) {
                        receiverIp = userIt->second.ip.empty() ? "127.0.0.1" : userIt->second.ip; // 兜底本地IP
                        receiverTcpPort = userIt->second.dataPort;
                        receiverUdpPort = userIt->second.udpPort;
                        
                        // 端口兜底：若接收方端口无效，使用默认值（仅调试）
                        if (receiverTcpPort == 0) {
                            std::cout << "警告：接收方" << imgMsg.receiverId << "TCP端口无效，使用默认值" << std::endl;
                        }
                        if (receiverUdpPort == 0) {
                            std::cout << "警告：接收方" << imgMsg.receiverId << "UDP端口无效，使用默认值" << std::endl;
                        }
                    }
                }
                
                // 3. 向发送方发送点对点地址通知
                P2PAddrNotify addrNotify;
                addrNotify.targetUserId = imgMsg.receiverId;
                addrNotify.targetIp = receiverIp;
                addrNotify.targetTcpPort = receiverTcpPort;
                addrNotify.targetUdpPort = receiverUdpPort;
                addrNotify.senderId = imgMsg.senderId;
                
                std::string notifyData = serialize(addrNotify);
                PacketHeader notifyHeader;
                notifyHeader.msgType = static_cast<uint32_t>(MsgType::P2P_ADDR_NOTIFY);
                notifyHeader.dataLen = notifyData.size();
                notifyHeader.msgId = 0;
                notifyHeader.senderId = 0;
                PacketHeader netNotifyHeader = htonHeader(notifyHeader);
                
                // 发送地址通知给发送方
                send(clientFd, &netNotifyHeader, sizeof(PacketHeader), MSG_NOSIGNAL);
                send(clientFd, notifyData.c_str(), notifyData.size(), MSG_NOSIGNAL);
                std::cout << "发送P2P地址通知给发送方" << imgMsg.senderId << "：IP=" << receiverIp << "，TCP=" << receiverTcpPort << "，UDP=" << receiverUdpPort << std::endl;
                
                // 4. 向接收方转发图片元信息（不含数据，仅通知准备接收）
                std::string forwardData = serialize(imgMsg);
                PacketHeader imgHeader;
                imgHeader.msgType = static_cast<uint32_t>(MsgType::IMAGE_MSG);
                imgHeader.dataLen = forwardData.size();
                imgHeader.msgId = 0;
                imgHeader.senderId = imgMsg.senderId;
                PacketHeader netImgHeader = htonHeader(imgHeader);
                
                send(receiverFd, &netImgHeader, sizeof(PacketHeader), MSG_NOSIGNAL);
                send(receiverFd, forwardData.c_str(), forwardData.size(), MSG_NOSIGNAL);
                
                rsp.success = true;
                rsp.msg = "图片消息元信息发送成功，已获取对方点对点地址";
            }
            
            // 发送图片响应给发送方
            std::string rspData = serialize(rsp);
            PacketHeader rspHeader;
            rspHeader.msgType = static_cast<uint32_t>(MsgType::IMAGE_MSG_RSP);
            rspHeader.dataLen = rspData.size();
            rspHeader.msgId = 0;
            rspHeader.senderId = 0;
            PacketHeader netRspHeader = htonHeader(rspHeader);
            
            send(clientFd, &netRspHeader, sizeof(PacketHeader), MSG_NOSIGNAL);
            send(clientFd, rspData.c_str(), rspData.size(), MSG_NOSIGNAL);
            break;
        }

        // 新增：处理文件消息（逻辑与图片一致，仅结构体不同）
        case MsgType::FILE_MSG: {
            std::cout << "收到文件消息（MsgType::FILE_MSG），dataLen=" << dataLen << std::endl;
            FileMsg fileMsg = deserializeFileMsg(dataBufferStr);
            
            int receiverFd = findFdByUserId(fileMsg.receiverId);
            FileMsgRsp rsp;
            rsp.success = false;
            rsp.msg = "";
            rsp.receiverId = fileMsg.receiverId;
            
            if (receiverFd == -1) {
                rsp.msg = "接收方不在线，无法发送文件";
            } else {
                // 获取接收方地址
                std::string receiverIp;
                uint16_t receiverTcpPort = 0;
                uint16_t receiverUdpPort = 0;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    auto userIt = g_onlineUsers.find(fileMsg.receiverId);
                    if (userIt != g_onlineUsers.end()) {
                        receiverIp = userIt->second.ip;
                        receiverTcpPort = userIt->second.dataPort;
                        receiverUdpPort = userIt->second.udpPort;
                    }
                }
                
                // 发送地址通知给发送方
                P2PAddrNotify addrNotify;
                addrNotify.targetUserId = fileMsg.receiverId;
                addrNotify.targetIp = receiverIp;
                addrNotify.targetTcpPort = receiverTcpPort;
                addrNotify.targetUdpPort = receiverUdpPort;
                addrNotify.senderId = fileMsg.senderId;
                
                std::string notifyData = serialize(addrNotify);
                PacketHeader notifyHeader;
                notifyHeader.msgType = static_cast<uint32_t>(MsgType::P2P_ADDR_NOTIFY);
                notifyHeader.dataLen = notifyData.size();
                notifyHeader.msgId = 0;
                notifyHeader.senderId = 0;
                PacketHeader netNotifyHeader = htonHeader(notifyHeader);
                
                send(clientFd, &netNotifyHeader, sizeof(PacketHeader), MSG_NOSIGNAL);
                send(clientFd, notifyData.c_str(), notifyData.size(), MSG_NOSIGNAL);
                
                // 转发文件元信息给接收方
                std::string forwardData = serialize(fileMsg);
                PacketHeader fileHeader;
                fileHeader.msgType = static_cast<uint32_t>(MsgType::FILE_MSG);
                fileHeader.dataLen = forwardData.size();
                fileHeader.msgId = 0;
                fileHeader.senderId = fileMsg.senderId;
                PacketHeader netFileHeader = htonHeader(fileHeader);
                
                send(receiverFd, &netFileHeader, sizeof(PacketHeader), MSG_NOSIGNAL);
                send(receiverFd, forwardData.c_str(), forwardData.size(), MSG_NOSIGNAL);
                
                rsp.success = true;
                rsp.msg = "文件消息元信息发送成功，已获取对方点对点地址";
            }
            
            // 发送文件响应
            std::string rspData = serialize(rsp);
            PacketHeader rspHeader;
            rspHeader.msgType = static_cast<uint32_t>(MsgType::FILE_MSG_RSP);
            rspHeader.dataLen = rspData.size();
            rspHeader.msgId = 0;
            rspHeader.senderId = 0;
            PacketHeader netRspHeader = htonHeader(rspHeader);
            
            send(clientFd, &netRspHeader, sizeof(PacketHeader), MSG_NOSIGNAL);
            send(clientFd, rspData.c_str(), rspData.size(), MSG_NOSIGNAL);
            break;
        }

        // 新增：处理点对点地址通知（透传，无需额外逻辑）
        case MsgType::P2P_ADDR_NOTIFY: {
            std::cout << "转发点对点地址通知（MsgType::P2P_ADDR_NOTIFY）" << std::endl;
            break;
        }

        // 新增：处理客户端主动下线请求（LOGOUT_REQ）
        case MsgType::LOGOUT_REQ: {
            std::cout << "收到客户端主动下线通知（FD=" << clientFd << "）" << std::endl;
            
            std::string offlineNickname = "";
            int offlineUserId = -1;
            std::string notifyData;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto fdIt = g_fdToUserId.find(clientFd);
                if (fdIt != g_fdToUserId.end()) {
                    offlineUserId = fdIt->second;
                    auto userIt = g_onlineUsers.find(offlineUserId);
                    if (userIt != g_onlineUsers.end()) {
                        offlineNickname = userIt->second.nickname;
                        // 构造下线通知（统一序列化方式）
                        UserStatusNotify notify;
                        notify.userId = offlineUserId;
                        notify.nickname = offlineNickname;
                        notify.isOnline = false;
                        notifyData = nlohmann::json(notify).dump(); // 替换serialize，保持统一
                        
                        // 立即清理用户
                        g_onlineUsers.erase(userIt);
                    }
                    // 清理FD映射和心跳记录
                    g_fdToUserId.erase(fdIt);
                    g_fdLastHeartbeat.erase(clientFd);
                }
            }

            // 锁外广播主动下线通知（避免死锁）
            if (!notifyData.empty()) {
                broadcast(notifyData, static_cast<uint32_t>(MsgType::USER_STATUS_NOTIFY), clientFd);
            }
            
            shutdown(clientFd, SHUT_RDWR);
            close(clientFd);
            std::cout << "主动下线用户处理完成：ID=" << offlineUserId << "，昵称=" << offlineNickname << std::endl;
            return false;
        }
        default:
            std::cout << "未知消息类型: " << (int)msgType << std::endl;
            break;
    }
    return true;
}

// 线程入口函数
void handleClientThread(int clientFd, const std::string& clientIp) {
    // 设置客户端FD为非阻塞（核心：避免recv卡住）
    int flags = fcntl(clientFd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);
    }

    handleClient(clientFd, clientIp);
    std::cout << "客户端 " << clientIp << " 连接处理线程结束" << std::endl;
}

// 服务端新增UDP中继处理
void onUdpReadyRead() {
    if (m_udpRelaySocket < 0) return;

    char buffer[4096] = {0};
    struct sockaddr_in senderAddr;
    socklen_t addrLen = sizeof(senderAddr);
    
    // 接收UDP数据报
    ssize_t readLen = recvfrom(m_udpRelaySocket, buffer, sizeof(buffer), 0, 
                               (struct sockaddr*)&senderAddr, &addrLen);
    if (readLen <= 0) return;

    // 解析转发请求（格式：senderId|receiverId|data）
    std::string dataStr(buffer, readLen);
    std::vector<std::string> parts;
    size_t pos = 0;
    while ((pos = dataStr.find("|")) != std::string::npos) {
        parts.push_back(dataStr.substr(0, pos));
        dataStr.erase(0, pos + 1);
    }
    parts.push_back(dataStr);

    if (parts.size() < 3) return;

    int senderId = std::stoi(parts[0]);
    int receiverId = std::stoi(parts[1]);
    std::string realData = parts[2];

    // 查找接收方的中继连接
    if (m_userRelayMap.count(receiverId)) {
        UserRelayInfo info = m_userRelayMap[receiverId];
        // 转发数据到接收方
        struct sockaddr_in targetAddr;
        memset(&targetAddr, 0, sizeof(targetAddr));
        targetAddr.sin_family = AF_INET;
        targetAddr.sin_port = htons(info.udpPort);
        inet_pton(AF_INET, info.addr.c_str(), &targetAddr.sin_addr);
        
        sendto(m_udpRelaySocket, realData.c_str(), realData.size(), 0,
               (struct sockaddr*)&targetAddr, sizeof(targetAddr));
    }
}

// TCP中继认证函数
int getUserIdFromAuth(int clientSocket) {
    // 临时实现：返回默认用户ID（后续可根据实际认证逻辑修改，比如从客户端发送的认证数据中解析）
    // 示例逻辑：读取客户端发送的第一个数据包，解析出userId
    char authBuffer[128] = {0};
    ssize_t len = recv(clientSocket, authBuffer, sizeof(authBuffer), MSG_PEEK); // 只查看不读取
    if (len > 0) {
        try {
            return std::stoi(std::string(authBuffer));
        } catch (...) {
            return 0;
        }
    }
    return 0; // 默认返回0，表示未认证
}

// 服务端新增TCP中继处理
void onTcpRelayNewConnection() {
    if (m_tcpRelayServer < 0) return;

    // 接收新TCP连接
    struct sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);
    int clientSocket = accept(m_tcpRelayServer, (struct sockaddr*)&clientAddr, &addrLen);
    if (clientSocket < 0) return;

    // 验证客户端身份（占位函数）
    int userId = getUserIdFromAuth(clientSocket);
    
    // 保存客户端中继信息
    m_userRelayMap[userId].tcpSocket = clientSocket;
    m_userRelayMap[userId].addr = inet_ntoa(clientAddr.sin_addr);
    m_userRelayMap[userId].tcpPort = ntohs(clientAddr.sin_port);

    // 设置非阻塞
    int flags = fcntl(clientSocket, F_GETFL, 0);
    fcntl(clientSocket, F_SETFL, flags | O_NONBLOCK);

    // 处理客户端数据（模拟Qt的readyRead信号）
    char buffer[4096] = {0};
    ssize_t readLen = recv(clientSocket, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (readLen > 0) {
        std::string data(buffer, readLen);
        std::vector<std::string> parts;
        size_t pos = 0;
        while ((pos = data.find("|")) != std::string::npos) {
            parts.push_back(data.substr(0, pos));
            data.erase(0, pos + 1);
        }
        parts.push_back(data);

        if (parts.size() < 3) return;
        int receiverId = std::stoi(parts[1]);
        std::string realData = parts[2];

        // 转发数据到接收方
        if (m_userRelayMap.count(receiverId)) {
            int receiverSocket = m_userRelayMap[receiverId].tcpSocket;
            if (receiverSocket > 0) {
                send(receiverSocket, realData.c_str(), realData.size(), MSG_NOSIGNAL);
            }
        }
    }
}

// 服务器主函数
int main() {
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        perror("socket创建失败");
        return -1;
    }
    
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // 设置非阻塞
    int flags = fcntl(serverFd, F_GETFL, 0);
    fcntl(serverFd, F_SETFL, flags | O_NONBLOCK);
    
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

    // ========== epoll初始化 ==========
    int epollFd = epoll_create1(0);
    if (epollFd < 0) {
        perror("epoll_create失败");
        close(serverFd);
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = serverFd;
    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, serverFd, &ev) < 0) {
        perror("epoll_ctl add serverFd失败");
        close(serverFd);
        close(epollFd);
        return -1;
    }

    // 启动心跳检测线程
    g_isRunning = true;
    g_heartbeatCheckThread = std::thread(heartbeatCheckThread);
    g_heartbeatCheckThread.detach();
    std::cout << "心跳检测线程已启动，超时阈值：" << HEARTBEAT_TIMEOUT_SEC << "秒" << std::endl;

    std::cout << "服务器启动，监听端口 8888...（epoll版本）" << std::endl;

    // epoll事件循环
    struct epoll_event events[1024];
    while (true) {
        int nfds = epoll_wait(epollFd, events, 1024, -1);
        if (nfds < 0) {
            perror("epoll_wait失败");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            // 新客户端连接
            if (fd == serverFd) {
                while (true) {
                    sockaddr_in clientAddr{};
                    socklen_t clientAddrLen = sizeof(clientAddr);
                    int clientFd = accept(serverFd, (sockaddr*)&clientAddr, &clientAddrLen);
                    if (clientFd < 0) {
                        // 非阻塞accept，无新连接则退出循环
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        } else {
                            perror("accept失败");
                            break;
                        }
                    }

                    // 设置客户端FD为非阻塞
                    flags = fcntl(clientFd, F_GETFL, 0);
                    fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);

                    // 添加到epoll
                    ev.events = EPOLLIN | EPOLLRDHUP;
                    ev.data.fd = clientFd;
                    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, clientFd, &ev) < 0) {
                        perror("epoll_ctl add clientFd失败");
                        close(clientFd);
                        continue;
                    }

                    std::string clientIp = inet_ntoa(clientAddr.sin_addr);
                    std::cout << "客户端 " << clientIp << " 连接成功，分配FD：" << clientFd << std::endl;

                    // 初始化心跳时间
                    {
                        std::lock_guard<std::mutex> lock(g_mutex);
                        g_fdLastHeartbeat[clientFd] = std::chrono::steady_clock::now();
                    }
                }
            } 
            // 客户端断开连接
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                std::cout << "客户端FD=" << fd << " 断开连接" << std::endl;
                // 清理用户
                int offlineUserId = -1;
                std::string offlineNickname = "";
                std::string notifyData;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    auto fdIt = g_fdToUserId.find(fd);
                    if (fdIt != g_fdToUserId.end()) {
                        offlineUserId = fdIt->second;
                        auto userIt = g_onlineUsers.find(offlineUserId);
                        if (userIt != g_onlineUsers.end()) {
                            offlineNickname = userIt->second.nickname;
                            UserStatusNotify notify;
                            notify.userId = offlineUserId;
                            notify.nickname = offlineNickname;
                            notify.isOnline = false;
                            notifyData = nlohmann::json(notify).dump();
                            g_onlineUsers.erase(userIt);
                        }
                        g_fdToUserId.erase(fdIt);
                        g_fdLastHeartbeat.erase(fd);
                    }
                }
                // 广播下线通知
                if (!notifyData.empty()) {
                    broadcast(notifyData, static_cast<uint32_t>(MsgType::USER_STATUS_NOTIFY), fd);
                }
                // 移除epoll监听并关闭FD
                epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
            }
            // 客户端有数据可读
            else if (events[i].events & EPOLLIN) {
                // 获取客户端IP（从FD反向查找，需补充FD到IP的映射）
                std::string clientIp = "127.0.0.1"; // 简化处理，实际可通过getsockname获取
                // 处理客户端消息（复用原有handleClient逻辑，去掉线程）
                handleClient(fd, clientIp);
            }
        }
    }

    // 清理资源
    g_isRunning = false;
    if (g_heartbeatCheckThread.joinable()) {
        g_heartbeatCheckThread.join();
    }
    close(serverFd);
    close(epollFd);
    return 0;
}