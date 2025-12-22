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

// 全局变量定义（带 std:: 前缀，规范命名空间）
std::map<int, UserInfo> gOnlineUsers;          // 在线用户列表（key=userId，value=UserInfo）
std::map<int, int> gFdToUserId;                // fd→userId 映射（通过fd查userId）
std::map<int, int> gUserIdToFd;                // userId→fd 映射（通过userId查fd，关键！）
std::map<int, time_t> gUserLastOnlineTime;     // 用户最后在线时间（心跳检测用）
const int HEARTBEAT_TIMEOUT = 10;              // 心跳超时时间（10秒）
// 新增：全局互斥锁（保证多线程操作共享数据的线程安全）
std::mutex gMutex;  // 关键！解决所有 "gMutex 未声明" 错误

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
int getUserIdByFd(int clientFd);

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
    gUserLastOnlineTime[userId] = time(nullptr);
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
            gOnlineUsers[userId] = user;          // 添加到在线用户列表
            gFdToUserId[clientFd] = userId;       // fd → userId 映射
            gUserIdToFd[userId] = clientFd;       // userId → fd 映射（关键！心跳检测需要）
            gUserLastOnlineTime[userId] = time(nullptr); // 初始化心跳时间
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

             // ===================== 新增：字节序转换 + 打印验证 =====================
            uint32_t originalMsgType = header.msgType; // 保存原始网络字节序
            uint32_t originalDataLen = header.dataLen;
            header.msgType = ntohl(header.msgType);    // 网络字节序 → 主机字节序
            header.dataLen = ntohl(header.dataLen);    // 必须转换，否则读取消息体长度错误
            // =====================================================================

            // 检查数据包是否完整
            if (recvBuffer.size() < sizeof(PacketHeader) + header.dataLen) {
                std::cout << "[handleClient解析] fd=" << clientFd << " 数据不完整，等待后续包" << std::endl;
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
            // 处理不同类型的消息（使用转换后的 header.msgType！）
            switch (header.msgType) {
                case LOGIN_REQ:
                    std::cout << "[处理消息] fd=" << clientFd << " 登录请求" << std::endl;
                    handleLoginReq(clientFd, payload);
                    break;
                case COMMON_MSG:
                    std::cout << "[处理消息] fd=" << clientFd << " 聊天消息" << std::endl;
                    handleCommonMsg(clientFd, payload);
                    break;
                case USER_LIST_REQ:
                    std::cout << "[处理消息] fd=" << clientFd << " 用户列表请求" << std::endl;
                    handleUserListReq(clientFd);
                    break;
                case HEARTBEAT: {
                    std::cout << "[处理消息] fd=" << clientFd << " 心跳包" << std::endl;
                    int userId = getUserIdByFd(clientFd);
                    if (userId != -1) {
                        handleHeartbeat(clientFd, userId); // 更新心跳时间，避免超时
                    }
                    break;
                }
                default:
                    std::cerr << "[处理消息] fd=" << clientFd << " 未知消息类型（转换后）：" << header.msgType << std::endl;
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

int getUserIdByFd(int clientFd) {
    auto it = gFdToUserId.find(clientFd);
    if (it != gFdToUserId.end()) {
        return it->second;
    }
    std::cout << "警告：fd=" << clientFd << " 未找到对应的userId" << std::endl;
    return -1;
}

// 心跳检测线程：定期检查用户超时，清理离线用户
void heartbeatCheckThread() {
    while (true) { // 无限循环，持续检测
        sleep(1); // 每秒检查一次（降低CPU占用）
        time_t now = time(nullptr); // 当前时间戳（秒）

        // 遍历所有在线用户（gOnlineUsers：key=userId，value=UserInfo）
        for (auto it = gOnlineUsers.begin(); it != gOnlineUsers.end(); ) {
            int userId = it->first;
            // 通过 userId→fd 映射获取客户端文件描述符
            auto fdIt = gUserIdToFd.find(userId);
            if (fdIt == gUserIdToFd.end()) {
                // 找不到对应的fd，直接清理该用户
                std::cout << "[心跳检测]  userId=" << userId << " 无对应fd，清理离线" << std::endl;
                gUserLastOnlineTime.erase(userId);
                it = gOnlineUsers.erase(it);
                continue;
            }
            int clientFd = fdIt->second;

            // 检查心跳超时：无心跳记录 或 超时（>10秒）
            auto heartIt = gUserLastOnlineTime.find(userId);
            bool isTimeout = (heartIt == gUserLastOnlineTime.end()) 
                           || (now - heartIt->second) > HEARTBEAT_TIMEOUT;

            if (isTimeout) {
                // 处理超时离线：关闭连接+清理资源+广播下线
                std::cout << "用户超时离线：userId=" << userId 
                          << "，nickname=" << it->second.nickname 
                          << "，fd=" << clientFd << std::endl;

                // 1. 关闭客户端连接
                close(clientFd);
                // 2. 清理映射表
                gFdToUserId.erase(clientFd);
                gUserIdToFd.erase(userId);
                gUserLastOnlineTime.erase(userId);
                // 3. 广播用户下线通知（给所有在线客户端）
                std::vector<char> offlineData = serializeUserInfo(it->second);
                broadcastPacket(USER_OFFLINE_NOTIFY, offlineData, -1); // -1=不排除任何客户端
                // 4. 从在线用户列表删除
                it = gOnlineUsers.erase(it);
            } else {
                ++it; // 未超时，继续遍历下一个用户
            }
        }
    }
}

int main() {
    // 新增：打印包头大小（必须是 8 字节！）
    std::cout << "[调试] 服务端 PacketHeader 大小：" << sizeof(PacketHeader) << " 字节" << std::endl;
    
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