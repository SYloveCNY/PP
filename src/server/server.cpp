#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include "protocol_base.h" // 包含之前定义的PacketHeader、UserInfo等结构体

// 全局在线用户列表（key：userId，value：用户信息，与原版一致）
std::map<int, UserInfo> gOnlineUsers;
// 全局互斥锁（线程安全）
std::mutex gMutex;
// 辅助映射：fd → userId（用于心跳处理时快速查找用户）
std::map<int, int> gFdToUserId;
// 心跳超时时间（30秒，3倍客户端心跳间隔）
const int HEARTBEAT_TIMEOUT = 30;
// key: userId, value: 时间戳（秒）
std::map<int, time_t> g_userLastOnlineTime;
// 心跳检测线程间隔（5秒）
const int HEARTBEAT_CHECK_INTERVAL = 5;

// 函数声明
void sendLoginRsp(int clientFd, bool success, int userId, const std::string& msg);
void broadcastPacket(MsgType msgType, const std::vector<char>& data, int excludeFd);
void handleLoginReq(int clientFd, const std::vector<char>& data);
void handleHeartbeat(int clientFd, int userId);
void handleCommonMsg(int clientFd, const std::vector<char>& data); // 补充声明
void heartbeatCheckThread();
bool recvCompletePacket(int fd, PacketHeader& header, std::vector<char>& data);
void updateUserHeartbeat(int userId); // 新增：更新用户心跳时间戳
int getUserIdByManagePort(int managePort); // 新增：通过managePort获取userId

// 广播数据包给所有在线用户（排除发送者自身）
void broadcastPacket(MsgType msgType, const std::vector<char>& data, int excludeFd) {
    std::lock_guard<std::mutex> lock(gMutex);
    for (const auto& [userId, user] : gOnlineUsers) {
        int targetFd = user.managePort; // 使用存储的真实客户端fd
        if (targetFd == excludeFd) continue; // 跳过发送者
        
        // 发送广播包
        if (!sendPacket(targetFd, msgType, data)) {
            std::cerr << "广播给fd=" << targetFd << "失败：" << strerror(errno) << std::endl;
        } else {
            std::cout << "已广播消息给fd=" << targetFd << "（用户：" << user.nickname << "）" << std::endl;
        }
    }
}

// 处理客户端心跳包（更新心跳时间）
void handleHeartbeat(int clientFd, int userId) {
    // 更新用户最后在线时间
    g_userLastOnlineTime[userId] = time(nullptr);
    std::cout << "[心跳处理] 客户端fd=" << clientFd << "，userId=" << userId << " 心跳更新" << std::endl;

    // 可选：服务端回复心跳（客户端可忽略，仅确认收到）
    PacketHeader header;
    header.msgType = htonl(HEARTBEAT);
    header.dataLen = htonl(0);
    sendPacket(clientFd, HEARTBEAT, {}); // sendPacket是你服务端的发送函数
}
// 处理普通消息（点对点/广播）
void handleCommonMsg(int clientFd, const std::vector<char>& payload) {
    try {
        CommonMsg msg = deserializeCommonMsg(payload);
        std::cout << "转发消息：fromUserId=" << msg.fromUserId 
                  << "，toUserId=" << msg.toUserId 
                  << "，content=" << msg.content.substr(0, 10) << "..." << std::endl;

        std::lock_guard<std::mutex> lock(gMutex);
        // 广播消息（toUserId=0）：转发给所有在线用户（排除发送者）
        if (msg.toUserId == 0) {
            std::cout << "开始广播消息，在线用户数：" << gOnlineUsers.size() << std::endl;
            for (const auto& [userId, user] : gOnlineUsers) {
                if (userId == msg.fromUserId) {
                    std::cout << "跳过发送者：userId=" << userId << "，fd=" << user.managePort << std::endl;
                    continue; // 跳过发送者
                }
                int targetFd = user.managePort;
                if (sendPacket(targetFd, COMMON_MSG, payload)) {
                    std::cout << "已转发广播消息给：userId=" << userId << "，fd=" << targetFd << "（昵称：" << user.nickname << "）" << std::endl;
                } else {
                    std::cerr << "转发广播消息失败：userId=" << userId << "，fd=" << targetFd << std::endl;
                }
            }
        } 
        // 点对点消息：转发给指定用户
        else {
            auto it = gOnlineUsers.find(msg.toUserId);
            if (it != gOnlineUsers.end()) {
                int targetFd = it->second.managePort;
                if (sendPacket(targetFd, COMMON_MSG, payload)) {
                    std::cout << "已转发点对点消息给：userId=" << msg.toUserId << "，fd=" << targetFd << "（昵称：" << it->second.nickname << "）" << std::endl;
                }
            } else {
                std::cerr << "转发失败：目标用户userId=" << msg.toUserId << "不存在" << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "处理普通消息失败：" << e.what() << std::endl;
    }
}

// 心跳检测线程（清理超时用户）
void heartbeatCheckThread() {
    while (true) {
        sleep(1); // 每秒检查一次
        time_t now = time(nullptr);

        // 遍历所有在线用户，检查超时
        for (auto it = g_onlineUsers.begin(); it != g_onlineUsers.end(); ) {
            int userId = it->first;
            int clientFd = it->second.fd;

            // 若用户无心跳记录或超时，强制下线
            if (g_userLastOnlineTime.find(userId) == g_userLastOnlineTime.end() 
                || (now - g_userLastOnlineTime[userId]) > HEARTBEAT_TIMEOUT) {
                
                std::cout << "用户超时离线：userId=" << userId << "，nickname=" << it->second.nickname << "，fd=" << clientFd << std::endl;
                // 关闭连接+清理资源
                close(clientFd);
                g_onlineUsers.erase(it++);
                g_userLastOnlineTime.erase(userId);
                // 广播用户下线通知（其他在线用户）
                broadcastUserOffline(it->second);
            } else {
                ++it;
            }
        }
    }
}

// 发送登录响应
void sendLoginRsp(int clientFd, bool success, int userId, const std::string& msg) {
    LoginRsp rsp;
    rsp.success = success;
    rsp.userId = userId;
    rsp.msg = msg;
    rsp.nickname = success ? gOnlineUsers[userId].nickname : "";
    
    std::vector<char> rspData = serializeLoginRsp(rsp);
    sendPacket(clientFd, LOGIN_RSP, rspData);
}

// 处理登陆请求
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
        user.managePort = clientFd; // 关键：存储客户端真实fd（原版正确逻辑）
        user.ip = "127.0.0.1"; // 实际场景可通过clientAddr获取（inet_ntoa）
        user.lastHeartbeatTime = std::chrono::system_clock::now(); // 初始化心跳时间
        

        // 加入在线用户列表（线程安全）
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gOnlineUsers[userId] = user;
            gFdToUserId[clientFd] = userId; // 记录fd与userId的映射
        }

        // 发送登录成功响应
        sendLoginRsp(clientFd, true, userId, "登录成功");
        std::cout << "用户登录：userId=" << userId << "，nickname=" << req.nickname << "，fd=" << clientFd << std::endl;

        // 广播用户上线通知（所有在线用户都会收到）
        std::vector<char> notifyData = serializeUserInfo(user);
        broadcastPacket(USER_ONLINE_NOTIFY, notifyData, clientFd);
        
    } catch (const std::exception& e) {
        std::cerr << "处理登录请求失败：" << e.what() << std::endl;
        sendLoginRsp(clientFd, false, 0, "请求格式错误：" + std::string(e.what()));
    }
}

// 处理客户端的用户列表请求
void handleUserListReq(int clientFd) {
    std::lock_guard<std::mutex> lock(gMutex);
    std::cout << "准备发送用户列表给fd=" << clientFd << "，在线用户数：" << gOnlineUsers.size() << std::endl;
    
    // 序列化用户列表并发送响应
    std::vector<char> userListData = serializeUserList(gOnlineUsers);
    if (!sendPacket(clientFd, USER_LIST_RSP, userListData)) {
        std::cerr << "发送用户列表给fd=" << clientFd << "失败：" << strerror(errno) << std::endl;
    } else {
        std::cout << "已响应客户端fd=" << clientFd << "的用户列表请求" << std::endl;
    }
}

void handleClientData(int clientFd, const std::vector<char>& recvData) {
    // 1. 解析包头（重点：添加ntohl转换）
    PacketHeader header;
    memcpy(&header, recvData.data(), sizeof(PacketHeader));
    
    // 核心修复：网络字节序 → 主机字节序（之前遗漏这步！）
    header.msgType = ntohl(header.msgType);   // 关键：转字节序
    header.dataLen = ntohl(header.dataLen);   // 关键：转字节序

    // 2. 打印解析后的消息类型（验证是否正确）
    std::cout << "[服务端解析] 客户端fd=" << clientFd
              << "，msgType=" << header.msgType
              << "，dataLen=" << header.dataLen << std::endl;

    // 3. 后续按正确的msgType处理消息（USER_LIST_REQ/HEARTBEAT等）
    switch (header.msgType) {
        case USER_LIST_REQ:
            handleUserListReq(clientFd); // 处理用户列表请求
            break;
        case HEARTBEAT:
            handleHeartbeat(clientFd);  // 处理心跳（更新用户在线时间）
            break;
        case COMMON_MSG:
            handleCommonMsg(clientFd, recvData); // 处理聊天消息
            break;
        // ... 其他消息类型 ...
        default:
            std::cout << "未知消息类型：" << header.msgType << "，来自fd=" << clientFd << std::endl;
            break;
    }
}

// 处理单个客户端的消息（主循环调用）
void handleClient(int clientFd) {
    char buf[4096];
    std::vector<char> recvBuffer; // 缓存数据，处理粘包/半包

    while (true) {
        ssize_t n = recv(clientFd, buf, sizeof(buf), 0);
        if (n <= 0) {
            // 客户端断开连接
            std::lock_guard<std::mutex> lock(gMutex);
            int userId = gFdToUserId[clientFd];
            std::cout << "客户端断开连接：fd=" << clientFd << "，userId=" << userId << std::endl;
            
            // 清理资源
            close(clientFd);
            gFdToUserId.erase(clientFd);
            auto userIt = gOnlineUsers.find(userId);
            if (userIt != gOnlineUsers.end()) {
                // 广播下线通知
                std::vector<char> notifyData = serializeUserInfo(userIt->second);
                broadcastPacket(USER_OFFLINE_NOTIFY, notifyData, -1);
                gOnlineUsers.erase(userIt);
            }
            break;
        }

        // 追加数据到缓存
        recvBuffer.insert(recvBuffer.end(), buf, buf + n);

        // 循环解析完整数据包
        while (recvBuffer.size() >= sizeof(PacketHeader)) {
            PacketHeader header;
            memcpy(&header, recvBuffer.data(), sizeof(PacketHeader));

            // 检查数据包是否完整
            if (recvBuffer.size() < sizeof(PacketHeader) + header.dataLen) {
                break; // 数据不完整，等待后续包
            }

            // 提取payload
            std::vector<char> payload(
                recvBuffer.begin() + sizeof(PacketHeader),
                recvBuffer.begin() + sizeof(PacketHeader) + header.dataLen
            );

            // 移除已处理的数据
            recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + sizeof(PacketHeader) + header.dataLen);

            // 处理不同类型的消息
            switch (header.msgType) {
                case LOGIN_REQ:
                    handleLoginReq(clientFd, payload);
                    break;
                case COMMON_MSG:
                    handleCommonMsg(clientFd, payload);
                    break;
                case USER_LIST_REQ:
                    handleUserListReq(clientFd);
                    break;
                case HEARTBEAT:
                    handleHeartbeat(clientFd);
                    break;
                default:
                    std::cerr << "未知消息类型：" << header.msgType << "，来自fd=" << clientFd << std::endl;
                    break;
            }
        }
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

// 序列化在线用户列表（与客户端deserializeUserList对应）
std::vector<char> serializeUserList(const std::map<int, UserInfo>& users) {
    std::vector<char> data;
    // 1. 序列化用户数量（网络字节序）
    uint32_t userCount = static_cast<uint32_t>(users.size());
    uint32_t networkCount = htonl(userCount);
    data.insert(data.end(), reinterpret_cast<char*>(&networkCount),
                reinterpret_cast<char*>(&networkCount) + sizeof(uint32_t));

    // 2. 逐个序列化用户信息
    for (const auto& [userId, user] : users) {
        std::vector<char> userData = serializeUserInfo(user);
        data.insert(data.end(), userData.begin(), userData.end());
    }
    return data;
}

int main() {
    // 启动心跳检测线程
    std::thread heartbeatThread(heartbeatCheckThread);
    heartbeatThread.detach();

    // 创建监听socket（标准TCP服务端流程）
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd == -1) {
        std::cerr << "创建socket失败：" << strerror(errno) << std::endl;
        return -1;
    }

    // 绑定端口（8888）
    sockaddr_in servAddr;
    memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_addr.s_addr = INADDR_ANY;
    servAddr.sin_port = htons(8888);
    if (bind(listenFd, (sockaddr*)&servAddr, sizeof(servAddr)) == -1) {
        std::cerr << "绑定端口失败：" << strerror(errno) << std::endl;
        close(listenFd);
        return -1;
    }

    // 开始监听
    if (listen(listenFd, 10) == -1) {
        std::cerr << "监听失败：" << strerror(errno) << std::endl;
        close(listenFd);
        return -1;
    }

    std::cout << "服务端启动成功，监听端口：8888" << std::endl;

    // 接受客户端连接（主循环）
    while (true) {
        sockaddr_in clientAddr;
        socklen_t clientAddrLen = sizeof(clientAddr);
        int clientFd = accept(listenFd, (sockaddr*)&clientAddr, &clientAddrLen);
        if (clientFd == -1) {
            std::cerr << "接受连接失败：" << strerror(errno) << std::endl;
            continue;
        }

        // 打印客户端连接信息
        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, INET_ADDRSTRLEN);
        uint16_t clientPort = ntohs(clientAddr.sin_port);
        std::cout << "新客户端连接：fd=" << clientFd << "，IP=" << clientIp << "，端口=" << clientPort << std::endl;

        // 启动线程处理该客户端（避免阻塞主循环）
        std::thread clientThread(handleClient, clientFd);
        clientThread.detach();
    }

    close(listenFd);
    return 0;
}