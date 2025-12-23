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
#include <fcntl.h>  // 新增：用于 fcntl 函数和 F_GETFL 宏
#include <errno.h>  // 确保已包含（errno 定义）
#include "protocol_base.h" // 包含之前定义的PacketHeader、UserInfo等结构体

// 全局变量定义（带 std:: 前缀，规范命名空间）
std::map<int, UserInfo> gOnlineUsers;          // 在线用户列表（key=userId，value=UserInfo）
std::map<int, int> gFdToUserId;                // fd→userId 映射（通过fd查userId）
std::map<int, int> gUserIdToFd;                // userId→fd 映射（通过userId查fd，关键！）
std::map<int, time_t> gUserLastOnlineTime;     // 用户最后在线时间（心跳检测用）
const int HEARTBEAT_TIMEOUT = 10;              // 心跳超时时间（10秒）
// 新增：全局互斥锁（保证多线程操作共享数据的线程安全）
std::mutex gMutex;  // 关键！解决所有 "gMutex 未声明" 错误
std::mutex gSendMutex;  

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
bool isValidFd(int fd); 
bool sendPacket(int fd, uint32_t msgType, const std::vector<char>& data);

// 广播数据包给所有在线用户（排除发送者自身）
void broadcastPacket(MsgType msgType, const std::vector<char>& data, int excludeFd) {
    std::lock_guard<std::mutex> lock(gMutex);
    for (const auto& [userId, user] : gOnlineUsers) {
        int targetFd = user.managePort;
        if (targetFd == excludeFd || !isValidFd(targetFd)) {
            continue; // 跳过发送者或无效FD
        }

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
    rsp.nickname = success ? [&]() {
        std::lock_guard<std::mutex> lock(gMutex); // 仅在需要时加锁读取昵称
        return gOnlineUsers[userId].nickname;
    }() : "";
    
    std::vector<char> rspData = serializeLoginRsp(rsp);
    sendPacket(clientFd, LOGIN_RSP, rspData); // 发送操作无锁冲突
}

// 处理登陆请求
void handleLoginReq(int clientFd, const std::vector<char>& data) {
    try {
        // 第一步：检查重复登录（临界区：读取 gFdToUserId）
        {
            std::lock_guard<std::mutex> lock(gMutex);
            if (gFdToUserId.count(clientFd) > 0) {
                int existingUserId = gFdToUserId[clientFd];
                std::cerr << "[重复登录拒绝] fd=" << clientFd 
                          << " 已绑定userId=" << existingUserId << "，拒绝再次登录" << std::endl;
                sendLoginRsp(clientFd, false, 0, "已登录，请勿重复登录");
                return;
            }
        }

        // 第二步：解析登录请求（无共享数据操作，无需锁）
        LoginReq req = deserializeLoginReq(data);
        if (req.nickname.empty()) {
            sendLoginRsp(clientFd, false, 0, "昵称不能为空");
            return;
        }
        static std::atomic<int> gNextUserId(1);
        int userId = gNextUserId++;
        
        // 第三步：构造用户信息（无共享数据操作，无需锁）
        UserInfo user;
        user.userId = userId;
        user.nickname = req.nickname;
        user.avatar = req.avatar;
        user.dataPort = req.dataPort;
        user.managePort = clientFd;
        user.ip = "127.0.0.1";
        user.lastHeartbeatTime = std::chrono::system_clock::now();

        // 第四步：添加到在线列表（临界区：写入共享数据）
        std::vector<char> notifyData;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gOnlineUsers[userId] = user;
            gFdToUserId[clientFd] = userId;
            gUserIdToFd[userId] = clientFd;
            gUserLastOnlineTime[userId] = time(nullptr);
            notifyData = serializeUserInfo(user); // 序列化用户信息（依赖共享数据）
        } // 退出临界区，释放 gMutex

        // 第五步：发送登录响应 + 广播上线通知（移出临界区）
        sendLoginRsp(clientFd, true, userId, "登录成功");
        std::cout << "用户登录：userId=" << userId << "，nickname=" << req.nickname << "，fd=" << clientFd << std::endl;
        broadcastPacket(USER_ONLINE_NOTIFY, notifyData, clientFd);
        
    } catch (const std::exception& e) {
        std::cerr << "处理登录请求失败：" << e.what() << std::endl;
        sendLoginRsp(clientFd, false, 0, "请求格式错误：" + std::string(e.what()));
    }
}

// 处理客户端的用户列表请求
void handleUserListReq(int clientFd) {
    std::vector<char> userListData;
    {
        // 临界区：仅保护「读取在线用户列表」（共享数据）
        std::lock_guard<std::mutex> lock(gMutex);
        std::cout << "准备发送用户列表给fd=" << clientFd << "，在线用户数：" << gOnlineUsers.size() << std::endl;
        userListData = serializeUserList(gOnlineUsers); // 读取共享数据
    } // 退出临界区，释放 gMutex

    // 发送操作移出临界区（使用 gSendMutex，无冲突）
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

    std::cout << "[客户端线程启动] 开始处理 fd=" << clientFd << " 的数据" << std::endl;

    while (true) {
        // 1. 接收数据（阻塞模式，直到有数据或连接断开）
        ssize_t n = recv(clientFd, buf, sizeof(buf), 0);
        if (n <= 0) {
            // 客户端断开连接或接收错误
            std::lock_guard<std::mutex> lock(gMutex);
            int userId = (gFdToUserId.count(clientFd) > 0) ? gFdToUserId[clientFd] : 0;
            if (n == 0) {
                std::cout << "[客户端断开] fd=" << clientFd << "，userId=" << userId << "（正常断开）" << std::endl;
            } else {
                std::cerr << "[接收错误] fd=" << clientFd << "，错误：" << strerror(errno) << std::endl;
            }
            
            // 清理资源（补充：删除 userId→fd 映射，避免残留）
            close(clientFd);
            gFdToUserId.erase(clientFd);
            auto userIt = gOnlineUsers.find(userId);
            if (userIt != gOnlineUsers.end()) {
                std::vector<char> notifyData = serializeUserInfo(userIt->second);
                broadcastPacket(USER_OFFLINE_NOTIFY, notifyData, -1);
                gOnlineUsers.erase(userIt);
                gUserIdToFd.erase(userId); // 补充：清理 userId→fd 映射
                gUserLastOnlineTime.erase(userId); // 清理心跳记录
            }
            break;
        }

        // 2. 打印接收详情（关键日志：确认是否收到数据）
        std::cout << "[接收数据] fd=" << clientFd << "，本次收到 " << n << " 字节，缓冲总大小：" << recvBuffer.size() + n << " 字节" << std::endl;
        recvBuffer.insert(recvBuffer.end(), buf, buf + n);

        // 3. 循环解析完整数据包
        while (recvBuffer.size() >= sizeof(PacketHeader)) {
            PacketHeader header;
            memcpy(&header, recvBuffer.data(), sizeof(PacketHeader));

            // 4. 字节序转换（核心步骤，必须执行）
            uint32_t originalMsgType = header.msgType;
            uint32_t originalDataLen = header.dataLen;
            header.msgType = ntohl(header.msgType);
            header.dataLen = ntohl(header.dataLen);

            // 5. 打印解析详情（关键日志：判断字节序是否正确）
            std::cout << "[解析包头] fd=" << clientFd
                      << "，msgType（原始→转换）=" << originalMsgType << "→" << header.msgType
                      << "，dataLen（原始→转换）=" << originalDataLen << "→" << header.dataLen
                      << "，缓冲当前大小：" << recvBuffer.size() << " 字节" << std::endl;

            // 6. 关键：判断 dataLen 是否合理（避免异常值导致无限等待）
            if (header.dataLen > 1024 * 1024) { // 限制最大消息体 1MB，防止恶意数据
                std::cerr << "[异常警告] fd=" << clientFd << " 收到无效 dataLen=" << header.dataLen << "，断开连接" << std::endl;
                // 清理资源并退出
                close(clientFd);
                gFdToUserId.erase(clientFd);
                return;
            }

            // 7. 检查数据包是否完整
            uint32_t totalPacketLen = sizeof(PacketHeader) + header.dataLen;
            if (recvBuffer.size() < totalPacketLen) {
                std::cout << "[等待后续包] fd=" << clientFd << "，需要总字节数：" << totalPacketLen 
                          << "，当前缓冲：" << recvBuffer.size() << " 字节" << std::endl;
                break;
            }

            // 8. 提取 payload 并处理
            std::vector<char> payload(
                recvBuffer.begin() + sizeof(PacketHeader),
                recvBuffer.begin() + totalPacketLen
            );

            // 9. 移除已处理数据
            recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + totalPacketLen);
            std::cout << "[处理数据包] fd=" << clientFd << "，消息类型：" << header.msgType 
                      << "，消息体长度：" << payload.size() << " 字节" << std::endl;

            // 10. 分发消息（原有逻辑不变）
            switch (header.msgType) {
                case LOGIN_REQ:
                    std::cout << "[处理消息] fd=" << clientFd << " → 登录请求" << std::endl;
                    handleLoginReq(clientFd, payload);
                    break;
                case COMMON_MSG:
                    std::cout << "[处理消息] fd=" << clientFd << " → 聊天消息" << std::endl;
                    handleCommonMsg(clientFd, payload);
                    break;
                case USER_LIST_REQ:
                    std::cout << "[处理消息] fd=" << clientFd << " → 用户列表请求" << std::endl;
                    handleUserListReq(clientFd);
                    break;
                case HEARTBEAT: {
                    std::cout << "[处理消息] fd=" << clientFd << " → 心跳包" << std::endl;
                    int userId = getUserIdByFd(clientFd);
                    if (userId != -1) {
                        handleHeartbeat(clientFd, userId);
                    }
                    break;
                }
                default:
                    std::cerr << "[未知消息] fd=" << clientFd << "，转换后类型：" << header.msgType << std::endl;
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
    std::lock_guard<std::mutex> lock(gMutex);
    auto it = gFdToUserId.find(clientFd);
    if (it != gFdToUserId.end()) {
        return it->second;
    }
    std::cout << "警告：fd=" << clientFd << " 未找到对应的userId" << std::endl;
    return -1;
}

// 心跳检测线程：定期检查用户超时，清理离线用户
void heartbeatCheckThread() {
    while (true) {
        sleep(1); // 每秒检查一次
        time_t now = time(nullptr);

        std::lock_guard<std::mutex> lock(gMutex); // 全局锁：避免数据竞争
        for (auto it = gOnlineUsers.begin(); it != gOnlineUsers.end(); ) {
            int userId = it->first;
            auto fdIt = gUserIdToFd.find(userId);
            if (fdIt == gUserIdToFd.end()) {
                std::cout << "[心跳检测] userId=" << userId << " 无对应FD，清理离线" << std::endl;
                gUserLastOnlineTime.erase(userId);
                it = gOnlineUsers.erase(it);
                continue;
            }
            int clientFd = fdIt->second;

            // 检查超时
            auto heartIt = gUserLastOnlineTime.find(userId);
            bool isTimeout = (heartIt == gUserLastOnlineTime.end()) 
                           || (now - heartIt->second) > HEARTBEAT_TIMEOUT;

            if (isTimeout) {
                std::cout << "用户超时离线：userId=" << userId 
                          << "，nickname=" << it->second.nickname 
                          << "，fd=" << clientFd << std::endl;

                // 1. 安全关闭FD（先检查有效性）
                if (isValidFd(clientFd)) {
                    close(clientFd);
                }

                // 2. 彻底清理所有映射
                gFdToUserId.erase(clientFd);
                gUserIdToFd.erase(userId);
                gUserLastOnlineTime.erase(userId);

                // 3. 广播下线通知（直接遍历，避免重复加锁）
                std::vector<char> offlineData = serializeUserInfo(it->second);
                for (const auto& [targetUserId, targetUser] : gOnlineUsers) {
                    int targetFd = targetUser.managePort;
                    if (isValidFd(targetFd)) {
                        sendPacket(targetFd, USER_OFFLINE_NOTIFY, offlineData);
                    }
                }

                // 4. 移除在线用户
                it = gOnlineUsers.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// 新增：检查FD是否有效（避免向已关闭的FD发送数据）
bool isValidFd(int fd) {
    return fcntl(fd, F_GETFL) != -1 || errno != EBADF;
}

// 完整实现sendPacket（服务端发送数据包核心函数）
bool sendPacket(int fd, uint32_t msgType, const std::vector<char>& data) {
    std::lock_guard<std::mutex> lock(gSendMutex); // 改用发送独立锁

    // 1. 检查FD是否有效
    if (!isValidFd(fd)) {
        std::cerr << "[sendPacket] 失败：fd=" << fd << " 已失效（已关闭或无效）" << std::endl;
        return false;
    }

    // 2. 构造包头（网络字节序）
    PacketHeader header;
    header.msgType = htonl(msgType);
    header.dataLen = htonl(static_cast<uint32_t>(data.size()));

    // 3. 发送包头（确保完整发送）
    ssize_t sent = send(fd, &header, sizeof(PacketHeader), MSG_NOSIGNAL);
    if (sent != sizeof(PacketHeader)) {
        std::cerr << "[sendPacket] 发送包头失败：fd=" << fd << "，错误：" << strerror(errno) << std::endl;
        return false;
    }

    // 4. 发送数据体（仅当数据非空时）
    if (!data.empty()) {
        sent = send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (sent != static_cast<ssize_t>(data.size())) {
            std::cerr << "[sendPacket] 发送数据失败：fd=" << fd << "，错误：" << strerror(errno) << std::endl;
            return false;
        }
    }

    return true;
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