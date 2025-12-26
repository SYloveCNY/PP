#include "protocol_base.h"
#include "json/json.hpp"  // 引入nlohmann/json
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <map>
#include <iostream>
//#include <thread>

using json = nlohmann::json;  // 简化命名空间

// 全局在线用户列表（服务端维护，key：userId，value：UserInfo）
std::map<int, UserInfo> g_onlineUsers;
int g_nextUserId = 1;  // 自增用户ID

// 工具函数：将PacketHeader转换为网络字节序（htonl）
PacketHeader htonHeader(const PacketHeader& header) {
    PacketHeader netHeader;
    netHeader.msgType = htonl(static_cast<uint32_t>(header.msgType));
    netHeader.dataLen = htonl(header.dataLen);
    return netHeader;
}

// 工具函数：将网络字节序的PacketHeader转换为主机字节序（ntohl）
PacketHeader ntohHeader(const PacketHeader& netHeader) {
    PacketHeader hostHeader;
    hostHeader.msgType = ntohl(netHeader.msgType);
    hostHeader.dataLen = ntohl(netHeader.dataLen);
    return hostHeader;
}

// 1. 登录请求反序列化（JSON→LoginReq）
LoginReq deserializeLoginReq(const std::string& jsonStr) {
    json j = json::parse(jsonStr);
    LoginReq req;
    req.nickname = j["nickname"];
    req.avatar = j["avatar"];
    req.dataPort = j["dataPort"];
    return req;
}

// 2. 登录响应序列化（LoginRsp→JSON字符串）
std::string serializeLoginRsp(const LoginRsp& rsp) {
    json j;
    j["success"] = rsp.success;
    j["msg"] = rsp.msg;
    j["userId"] = rsp.userId;
    return j.dump();  // 转换为JSON字符串
}

// 3. 用户列表响应序列化（UserListRsp→JSON字符串）
std::string serializeUserListRsp(const UserListRsp& rsp) {
    json j;
    json usersArr = json::array();  // JSON数组存储用户列表
    for (const auto& user : rsp.users) {
        json userObj;
        userObj["userId"] = user.userId;
        userObj["nickname"] = user.nickname;
        userObj["ip"] = user.ip;
        userObj["dataPort"] = user.dataPort;
        usersArr.push_back(userObj);
    }
    j["users"] = usersArr;
    return j.dump();
}

std::vector<char> serializeUserListJson(const std::map<int, UserInfo>& users) {
    json j;
    j["msgType"] = static_cast<uint32_t>(MsgType::USER_LIST_RSP); // 加MsgType::前缀
    json data;
    data["userCount"] = users.size();
    json usersArray = json::array();
    for (const auto& [userId, user] : users) {
        json userObj;
        userObj["userId"] = user.userId;
        userObj["nickname"] = user.nickname;
        userObj["ip"] = user.ip;
        userObj["dataPort"] = user.dataPort;
        usersArray.push_back(userObj);
    }
    data["users"] = usersArray;
    j["data"] = data;

    std::string jsonStr = j.dump();
    return std::vector<char>(jsonStr.begin(), jsonStr.end());
}

// 服务端处理USER_LIST_REQ的逻辑（替换原二进制发送）
void handleUserListReq(int clientFd, const std::map<int, UserInfo>& onlineUsers) {
    std::vector<char> jsonData = serializeUserListJson(onlineUsers);
    PacketHeader header;
    // 加MsgType::前缀，转网络字节序
    header.msgType = htonl(static_cast<uint32_t>(MsgType::USER_LIST_RSP));
    header.dataLen = htonl(static_cast<uint32_t>(jsonData.size()));
    send(clientFd, &header, sizeof(header), 0);
    send(clientFd, jsonData.data(), jsonData.size(), 0);
}

bool handleClient(int clientFd, const std::string& clientIp) {
    std::cout << "New client connected: " << clientIp << std::endl;

//    while (true) {
        // 步骤1：读取协议头（8字节）
        PacketHeader netHeader;
        ssize_t headerLen = recv(clientFd, &netHeader, sizeof(PacketHeader), 0);
        if (headerLen <= 0) {
            std::cout << "Client disconnected: " << clientIp << std::endl;
            return false;
        }
        PacketHeader hostHeader = ntohHeader(netHeader);
        MsgType msgType = static_cast<MsgType>(hostHeader.msgType);
        uint32_t dataLen = hostHeader.dataLen;
		std::cout << "msgType = " << (int)msgType << ", dataLen = " << dataLen << std::endl;

		std::string jsonData(dataLen, '\0');
		if (dataLen)
		{
			// 步骤2：读取JSON数据
			ssize_t dataLenRecv = recv(clientFd, jsonData.data(), dataLen, 0);
			std::cout << "dataLenRecv = " << dataLenRecv << ", dataLen = " << dataLen << std::endl;
			if (dataLenRecv != dataLen) {
				std::cerr << "Failed to read full data from client" << std::endl;
				return false;//break;
			}
		}

        // 步骤3：根据消息类型处理
        switch (msgType) {
            case MsgType::LOGIN_REQ: {
                // 解析登录请求
                LoginReq req = deserializeLoginReq(jsonData);
                LoginRsp rsp;

                // 检查昵称是否已存在（简单逻辑）
                bool nicknameExists = false;
                for (const auto& [id, user] : g_onlineUsers) {
                    if (user.nickname == req.nickname) {
                        nicknameExists = true;
                        break;
                    }
                }
                if (nicknameExists) {
                    rsp.success = false;
                    rsp.msg = "Nickname already exists";
                    rsp.userId = -1;
                } else {
                    // 登录成功：添加到在线用户列表
                    UserInfo newUser;
                    newUser.userId = g_nextUserId++;
                    newUser.nickname = req.nickname;
                    newUser.ip = clientIp;
                    newUser.dataPort = req.dataPort;
                    g_onlineUsers[newUser.userId] = newUser;

                    rsp.success = true;
                    rsp.msg = "Login success";
                    rsp.userId = newUser.userId;
                    std::cout << "User logged in: " << newUser.nickname << " (ID: " << newUser.userId << ")" << std::endl;
                }

                // 发送登录响应（JSON格式）
                std::string rspJson = serializeLoginRsp(rsp);
                PacketHeader rspHeader;
                rspHeader.msgType = static_cast<uint32_t>(MsgType::LOGIN_RSP);
                rspHeader.dataLen = rspJson.size();
                PacketHeader netRspHeader = htonHeader(rspHeader);

                send(clientFd, &netRspHeader, sizeof(PacketHeader), 0);
                send(clientFd, rspJson.data(), rspJson.size(), 0);
                break;
            }

            case MsgType::USER_LIST_REQ: {
                // 处理用户列表请求：返回所有在线用户
                UserListRsp rsp;
                for (const auto& [id, user] : g_onlineUsers) {
                    rsp.users.push_back(user);
                }

                // 序列化并发送响应
                std::string rspJson = serializeUserListRsp(rsp);
                PacketHeader rspHeader;
                rspHeader.msgType = static_cast<uint32_t>(MsgType::USER_LIST_RSP);
                rspHeader.dataLen = rspJson.size();
                PacketHeader netRspHeader = htonHeader(rspHeader);

                send(clientFd, &netRspHeader, sizeof(PacketHeader), 0);
                send(clientFd, rspJson.data(), rspJson.size(), 0);
				std::cout << "rspJson = " << rspJson << std::endl;
                std::cout << "Sent user list to client: " << clientIp << " (count: " << rsp.users.size() << ")" << std::endl;
                break;
            }

            default:
                std::cerr << "Unknown msg type: " << static_cast<uint32_t>(msgType) << std::endl;
                break;
        }
    //}

	return true;
    // 客户端断开连接：从在线列表移除（这里简化，实际需要存储clientFd和userId的映射）
    // （优化：可以在登录成功时记录clientFd→userId的映射，断开时根据clientFd删除）
    // 临时方案：这里省略，测试时可重启服务端重置列表
 //   close(clientFd);
}

void handleClientMsg(int clientFd, MsgType msgType, const std::vector<char>& data) {
    // 加MsgType::前缀
    if (msgType == MsgType::USER_LIST_REQ) {
        // 修正变量名：g_onlineUsers（服务端全局在线用户map）
        handleUserListReq(clientFd, g_onlineUsers); 
    }
    // 其他消息类型（LOGIN_REQ等）保持不变...
}

int make_socket_non_blocking(int sfd) {
    int flags, s;

    flags = fcntl(sfd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl");
        return -1;
    }

    flags |= SOCK_NONBLOCK;
    s = fcntl(sfd, F_SETFL, flags);
    if (s == -1) {
        perror("fcntl");
        return -1;
    }
    return 0;
}

constexpr int MAX_EVENTS = 10;

int main() {
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return -1;
    }

	std::map<int, sockaddr_in> client_infos;
    // 端口复用
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
//	make_socket_non_blocking(serverFd);

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(8888);  // 服务端端口8888

    if (bind(serverFd, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Failed to bind socket" << std::endl;
        close(serverFd);
        return -1;
    }

    if (listen(serverFd, 10) < 0) {
        std::cerr << "Failed to listen" << std::endl;
        close(serverFd);
        return -1;
    }

    std::cout << "Server started on port 8888" << std::endl;

    // 创建epoll实例
    int epoll_fd = epoll_create1(0);
	epoll_event ev, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = serverFd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, serverFd, &ev);

    while (true) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == serverFd) {
                // 新连接
				sockaddr_in clientAddr;
				socklen_t clientAddrLen = sizeof(clientAddr);
				int clientFd = accept(serverFd, (sockaddr*)&clientAddr, &clientAddrLen);
                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = clientFd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, clientFd, &ev);
				client_infos.emplace(clientFd, clientAddr);
            } else {
				int clientFd = events[i].data.fd;
				auto it = client_infos.find(clientFd);
				if (!handleClient(clientFd, inet_ntoa(it->second.sin_addr)))
				{
					close(clientFd);
					epoll_ctl(epoll_fd, EPOLL_CTL_DEL, clientFd, NULL);
				}
			}
		}
    }

    close(serverFd);
    return 0;
}



// #include <iostream>
// #include <vector>
// #include <string>
// #include <map>
// #include <mutex>
// #include <atomic>
// #include <thread>
// #include <chrono>
// #include <stdexcept>
// #include <cstring>
// #include <arpa/inet.h>
// #include <sys/socket.h>
// #include <unistd.h>
// #include <fcntl.h>  // 新增：用于 fcntl 函数和 F_GETFL 宏
// #include <errno.h>  // 确保已包含（errno 定义）
// #include "protocol_base.h" // 包含之前定义的PacketHeader、UserInfo等结构体
// #include "json/json.hpp"

// // 全局变量定义（带 std:: 前缀，规范命名空间）
// std::map<int, UserInfo> gOnlineUsers;          // 在线用户列表（key=userId，value=UserInfo）
// std::map<int, int> gFdToUserId;                // fd→userId 映射（通过fd查userId）
// std::map<int, int> gUserIdToFd;                // userId→fd 映射（通过userId查fd，关键！）
// std::map<int, time_t> gUserLastOnlineTime;     // 用户最后在线时间（心跳检测用）
// const int HEARTBEAT_TIMEOUT = 10;              // 心跳超时时间（10秒）
// // 新增：全局互斥锁（保证多线程操作共享数据的线程安全）
// std::mutex gMutex;  // 关键！解决所有 "gMutex 未声明" 错误
// std::mutex gSendMutex;  

// // 函数声明
// void sendLoginRsp(int clientFd, bool success, int userId, const std::string& msg);
// void broadcastPacket(MsgType msgType, const std::vector<char>& data, int excludeFd);
// void handleLoginReq(int clientFd, const std::vector<char>& data);
// void handleHeartbeat(int clientFd, int userId);
// void handleCommonMsg(int clientFd, const std::vector<char>& data); // 补充声明
// void heartbeatCheckThread();
// bool recvCompletePacket(int fd, PacketHeader& header, std::vector<char>& data);
// void updateUserHeartbeat(int userId); // 新增：更新用户心跳时间戳
// int getUserIdByManagePort(int managePort); // 新增：通过managePort获取userId
// int getUserIdByFd(int clientFd);
// bool isValidFd(int fd); 
// bool sendPacket(int fd, uint32_t msgType, const std::vector<char>& data);

// // 广播数据包给所有在线用户（排除发送者自身）
// void broadcastPacket(uint32_t msgType, const std::vector<char>& data, int excludeFd) {
//     std::lock_guard<std::mutex> lock(gMutex);
//     for (const auto& [userId, user] : gOnlineUsers) {
//         int targetFd = user.managePort;
//         if (targetFd == -1 || targetFd == excludeFd) continue;
//         if (!sendPacket(targetFd, msgType, data)) {
//             std::cerr << "广播消息给fd=" << targetFd << "失败" << std::endl;
//         }
//     }
// }


// // 处理客户端心跳包（更新心跳时间）
// void handleHeartbeat(int clientFd, int userId) {
//     // 更新用户最后在线时间
//     gUserLastOnlineTime[userId] = time(nullptr);
//     std::cout << "[心跳处理] 客户端fd=" << clientFd << "，userId=" << userId << " 心跳更新" << std::endl;

//     PacketHeader header;
//     header.msgType = htonl(static_cast<uint32_t>(MsgType::HEARTBEAT)); // 枚举类转 uint32_t
//     header.dataLen = htonl(0);
//     send(clientFd, &header, sizeof(PacketHeader), MSG_NOSIGNAL);
// }

// // 处理普通消息（点对点/广播）
// void handleCommonMsg(int clientFd, const std::vector<char>& payload) {
//     try {
//         CommonMsg msg = deserializeCommonMsg(payload);
//         std::cout << "转发消息：fromUserId=" << msg.fromUserId 
//                   << "，toUserId=" << msg.toUserId 
//                   << "，content=" << msg.content.substr(0, 10) << "..." << std::endl;

//         std::lock_guard<std::mutex> lock(gMutex);
//         // 广播消息（toUserId=0）：转发给所有在线用户（排除发送者）
//         if (msg.toUserId == 0) {
//             std::cout << "开始广播消息，在线用户数：" << gOnlineUsers.size() << std::endl;
//             for (const auto& [userId, user] : gOnlineUsers) {
//                 if (userId == msg.fromUserId) {
//                     std::cout << "跳过发送者：userId=" << userId << "，fd=" << user.managePort << std::endl;
//                     continue; // 跳过发送者
//                 }
//                 int targetFd = user.managePort;
//                 if (sendPacket(targetFd, static_cast<uint32_t>(MsgType::COMMON_MSG), payload)) {
//                     std::cout << "已转发广播消息给：userId=" << userId << "，fd=" << targetFd << "（昵称：" << user.nickname << "）" << std::endl;
//                 } else {
//                     std::cerr << "转发广播消息失败：userId=" << userId << "，fd=" << targetFd << std::endl;
//                 }
//             }
//         } 
//         // 点对点消息：转发给指定用户
//         else {
//             auto it = gOnlineUsers.find(msg.toUserId);
//             if (it != gOnlineUsers.end()) {
//                 int targetFd = it->second.managePort;
//                 if (sendPacket(targetFd, static_cast<uint32_t>(MsgType::COMMON_MSG), payload)) {
//                     std::cout << "已转发点对点消息给：userId=" << msg.toUserId << "，fd=" << targetFd << "（昵称：" << it->second.nickname << "）" << std::endl;
//                 }
//             } else {
//                 std::cerr << "转发失败：目标用户userId=" << msg.toUserId << "不存在" << std::endl;
//             }
//         }
//     } catch (const std::exception& e) {
//         std::cerr << "处理普通消息失败：" << e.what() << std::endl;
//     }
// }

// // 心跳检测线程（清理超时用户）

// // 发送登录响应
// void sendLoginRsp(int clientFd, bool success, int userId, const std::string& msg) {
//     LoginRsp rsp;
//     rsp.success = success;
//     rsp.userId = userId;
//     rsp.msg = msg;
//     rsp.nickname = success ? [&]() {
//         std::lock_guard<std::mutex> lock(gMutex); // 仅在需要时加锁读取昵称
//         return gOnlineUsers[userId].nickname;
//     }() : "";
    
//     std::vector<char> rspData = serializeLoginRsp(rsp);
//     sendPacket(clientFd, static_cast<uint32_t>(MsgType::LOGIN_RSP), rspData); // 发送操作无锁冲突
// }

// // 处理登陆请求
// void handleLoginReq(int clientFd, const std::vector<char>& data) {
//     try {
//         // 第一步：检查重复登录（临界区：读取 gFdToUserId）
//         {
//             std::lock_guard<std::mutex> lock(gMutex);
//             if (gFdToUserId.count(clientFd) > 0) {
//                 int existingUserId = gFdToUserId[clientFd];
//                 std::cerr << "[重复登录拒绝] fd=" << clientFd 
//                           << " 已绑定userId=" << existingUserId << "，拒绝再次登录" << std::endl;
//                 sendLoginRsp(clientFd, false, 0, "已登录，请勿重复登录");
//                 return;
//             }
//         }

//         // 第二步：解析登录请求（无共享数据操作，无需锁）
//         LoginReq req = deserializeLoginReq(data);
//         if (req.nickname.empty()) {
//             sendLoginRsp(clientFd, false, 0, "昵称不能为空");
//             return;
//         }
//         static std::atomic<int> gNextUserId(1);
//         int userId = gNextUserId++;
        
//         // 第三步：构造用户信息（无共享数据操作，无需锁）
//         UserInfo user;
//         user.userId = userId;
//         user.nickname = req.nickname;
//         user.avatar = req.avatar;
//         user.dataPort = req.dataPort;
//         user.managePort = clientFd;
//         user.ip = "127.0.0.1";
//         // server.cpp handleLoginReq 函数中修正：
//         user.lastHeartbeatTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

//         // 第四步：添加到在线列表（临界区：写入共享数据）
//         std::vector<char> notifyData;
//         {
//             std::lock_guard<std::mutex> lock(gMutex);
//             gOnlineUsers[userId] = user;
//             gFdToUserId[clientFd] = userId;
//             gUserIdToFd[userId] = clientFd;
//             gUserLastOnlineTime[userId] = time(nullptr);
//             notifyData = serializeUserInfo(user); // 序列化用户信息（依赖共享数据）
//         } // 退出临界区，释放 gMutex

//         // 第五步：发送登录响应 + 广播上线通知（移出临界区）
//         sendLoginRsp(clientFd, true, userId, "登录成功");
//         std::cout << "用户登录：userId=" << userId << "，nickname=" << req.nickname << "，fd=" << clientFd << std::endl;
//         broadcastPacket(static_cast<uint32_t>(MsgType::USER_ONLINE_NOTIFY), notifyData, clientFd);
        
//     } catch (const std::exception& e) {
//         std::cerr << "处理登录请求失败：" << e.what() << std::endl;
//         sendLoginRsp(clientFd, false, 0, "请求格式错误：" + std::string(e.what()));
//     }
// }

// // 处理客户端的用户列表请求
// void handleUserListReq(int clientFd) {
//     std::vector<char> userListData;
//     {
//         // 临界区：仅读取在线用户列表（共享数据）
//         std::lock_guard<std::mutex> lock(gMutex);
//         std::cout << "准备发送用户列表给fd=" << clientFd << "，在线用户数：" << gOnlineUsers.size() << std::endl;
//         // 调用上面的序列化函数，生成用户列表数据
//         userListData = serializeUserList(gOnlineUsers);
//     } // 释放 gMutex，避免发送时阻塞

//     // 发送用户列表响应（USER_LIST_RSP=4，确保 msgType 枚举值正确）
//     if (!sendPacket(clientFd, static_cast<uint32_t>(MsgType::USER_LIST_RSP), userListData)) {
//         std::cerr << "发送用户列表给fd=" << clientFd << "失败：" << strerror(errno) << std::endl;
//     } else {
//         std::cout << "已发送用户列表给fd=" << clientFd << "，数据长度：" << userListData.size() << "字节" << std::endl;
//     }
// }

// // 处理单个客户端的消息（主循环调用）
// void handleClient(int clientFd) {
//    std::vector<char> recvBuffer;
// while (true) {
//     // 读取包头（先探知是否有完整包头）
//     if (recvBuffer.size() < sizeof(PacketHeader)) {
//         char buf[1024] = {0};
//         ssize_t n = recv(clientFd, buf, sizeof(buf), 0);
//         if (n <= 0) {
//             // 客户端断开连接
//             break;
//         }
//         recvBuffer.insert(recvBuffer.end(), buf, buf + n);
//         continue;
//     }

//     // 解析包头（全部用 uint32_t，避免枚举类类型冲突）
//     PacketHeader header;
//     memcpy(&header, recvBuffer.data(), sizeof(PacketHeader));
//     uint32_t originalMsgType = header.msgType;  // 网络字节序（uint32_t）
//     uint32_t msgType = ntohl(header.msgType);   // 主机字节序（uint32_t）
//     uint32_t dataLen = ntohl(header.dataLen);   // 主机字节序（uint32_t）

//     // 输出日志（无类型冲突）
//     std::cout << "[解析包头] fd=" << clientFd
//               << "，msgType（原始→转换）=" << originalMsgType << "→" << msgType
//               << "，dataLen（原始→转换）=" << header.dataLen << "→" << dataLen
//               << "，缓冲当前大小：" << recvBuffer.size() << " 字节" << std::endl;

//     // 检查数据包完整性
//     uint32_t totalLen = sizeof(PacketHeader) + dataLen;
//     if (recvBuffer.size() < totalLen) {
//         std::cout << "[数据包不完整] 等待后续数据..." << std::endl;
//         continue;
//     }

//     // 提取数据体
//     std::vector<char> payload(recvBuffer.begin() + sizeof(PacketHeader), recvBuffer.begin() + totalLen);
//     recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + totalLen);

//     // 处理不同消息类型（用 static_cast<uint32_t> 匹配枚举类值）
//     if (msgType == static_cast<uint32_t>(MsgType::LOGIN_REQ)) {
//         handleLoginReq(clientFd, payload);
//     } else if (msgType == static_cast<uint32_t>(MsgType::USER_LIST_REQ)) {
//         handleUserListReq(clientFd);
//     } else if (msgType == static_cast<uint32_t>(MsgType::COMMON_MSG)) {
//         handleCommonMsg(clientFd, payload);
//     } else if (msgType == static_cast<uint32_t>(MsgType::HEARTBEAT)) {
//         int userId = gFdToUserId[clientFd];
//         handleHeartbeat(clientFd, userId);
//     } else {
//         std::cout << "[未知消息类型] fd=" << clientFd << "，类型值：" << msgType << std::endl;
//     }
// }
// }

// // 完整读取数据包（确保不截断）
// bool recvCompletePacket(int fd, PacketHeader& header, std::vector<char>& data) {
//     // 读取头部（必须完整读取sizeof(PacketHeader)字节）
//     ssize_t ret = recv(fd, &header, sizeof(PacketHeader), 0);
//     if (ret == -1) {
//         perror("recv header failed");
//         return false;
//     } else if (ret == 0) {
//         std::cerr << "客户端断开连接：fd=" << fd << "（读取头部时）" << std::endl;
//         return false;
//     } else if (ret != sizeof(PacketHeader)) {
//         std::cerr << "头部读取不完整：fd=" << fd << "，实际读取=" << ret << "，需要=" << sizeof(PacketHeader) << std::endl;
//         return false;
//     }

//     // 读取数据部分（按header.dataLen读取完整）
//     data.resize(header.dataLen);
//     size_t recved = 0;
//     while (recved < header.dataLen) {
//         ret = recv(fd, data.data() + recved, header.dataLen - recved, 0);
//         if (ret == -1) {
//             perror("recv data failed");
//             return false;
//         } else if (ret == 0) {
//             std::cerr << "客户端断开连接：fd=" << fd << "（读取数据时）" << std::endl;
//             return false;
//         }
//         recved += ret;
//     }
//     return true;
// }

// // 更新用户心跳时间戳（线程安全）
// void updateUserHeartbeat(int userId) {
//     std::lock_guard<std::mutex> lock(gMutex);
//     auto it = gOnlineUsers.find(userId);
//     if (it != gOnlineUsers.end()) {
//         it->second.lastHeartbeatTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
//     }
// }

// // 通过managePort获取userId（线程安全）
// int getUserIdByManagePort(int managePort) {
//     std::lock_guard<std::mutex> lock(gMutex);
//     for (const auto& pair : gOnlineUsers) {
//         if (pair.second.managePort == managePort) {
//             return pair.first;
//         }
//     }
//     return -1; // 未找到
// }

// int getUserIdByFd(int clientFd) {
//     std::lock_guard<std::mutex> lock(gMutex);
//     auto it = gFdToUserId.find(clientFd);
//     if (it != gFdToUserId.end()) {
//         return it->second;
//     }
//     std::cout << "警告：fd=" << clientFd << " 未找到对应的userId" << std::endl;
//     return -1;
// }

// // 心跳检测线程：定期检查用户超时，清理离线用户
// void heartbeatCheckThread() {
//     while (true) {
//         sleep(1); // 每秒检查一次
//         time_t now = time(nullptr);

//         std::lock_guard<std::mutex> lock(gMutex); // 全局锁：避免数据竞争
//         for (auto it = gOnlineUsers.begin(); it != gOnlineUsers.end(); ) {
//             int userId = it->first;
//             auto fdIt = gUserIdToFd.find(userId);
//             if (fdIt == gUserIdToFd.end()) {
//                 std::cout << "[心跳检测] userId=" << userId << " 无对应FD，清理离线" << std::endl;
//                 gUserLastOnlineTime.erase(userId);
//                 it = gOnlineUsers.erase(it);
//                 continue;
//             }
//             int clientFd = fdIt->second;

//             // 检查超时
//             auto heartIt = gUserLastOnlineTime.find(userId);
//             bool isTimeout = (heartIt == gUserLastOnlineTime.end()) 
//                            || (now - heartIt->second) > HEARTBEAT_TIMEOUT;

//             if (isTimeout) {
//                 std::cout << "用户超时离线：userId=" << userId 
//                           << "，nickname=" << it->second.nickname 
//                           << "，fd=" << clientFd << std::endl;

//                 // 1. 安全关闭FD（先检查有效性）
//                 if (isValidFd(clientFd)) {
//                     close(clientFd);
//                 }

//                 // 2. 彻底清理所有映射
//                 gFdToUserId.erase(clientFd);
//                 gUserIdToFd.erase(userId);
//                 gUserLastOnlineTime.erase(userId);

//                 // 3. 广播下线通知（直接遍历，避免重复加锁）
//                 std::vector<char> offlineData = serializeUserInfo(it->second);
//                 for (const auto& [targetUserId, targetUser] : gOnlineUsers) {
//                     int targetFd = targetUser.managePort;
//                     if (isValidFd(targetFd)) {
//                         sendPacket(targetFd, static_cast<uint32_t>(MsgType::USER_OFFLINE_NOTIFY), offlineData);
//                     }
//                 }

//                 // 4. 移除在线用户
//                 it = gOnlineUsers.erase(it);
//             } else {
//                 ++it;
//             }
//         }
//     }
// }

// // 新增：检查FD是否有效（避免向已关闭的FD发送数据）
// bool isValidFd(int fd) {
//     return fcntl(fd, F_GETFL) != -1 || errno != EBADF;
// }

// // 完整实现sendPacket（服务端发送数据包核心函数）
// bool sendPacket(int fd, uint32_t msgType, const std::vector<char>& data) {
//     std::lock_guard<std::mutex> lock(gSendMutex); // 改用发送独立锁

//     // 1. 检查FD是否有效
//     if (!isValidFd(fd)) {
//         std::cerr << "[sendPacket] 失败：fd=" << fd << " 已失效（已关闭或无效）" << std::endl;
//         return false;
//     }

//     // 2. 构造包头（网络字节序）
//     PacketHeader header;
//     header.msgType = htonl(msgType);
//     header.dataLen = htonl(static_cast<uint32_t>(data.size()));

//     // 3. 发送包头（确保完整发送）
//     ssize_t sent = send(fd, &header, sizeof(PacketHeader), MSG_NOSIGNAL);
//     if (sent != sizeof(PacketHeader)) {
//         std::cerr << "[sendPacket] 发送包头失败：fd=" << fd << "，错误：" << strerror(errno) << std::endl;
//         return false;
//     }

//     // 4. 发送数据体（仅当数据非空时）
//     if (!data.empty()) {
//         sent = send(fd, data.data(), data.size(), MSG_NOSIGNAL);
//         if (sent != static_cast<ssize_t>(data.size())) {
//             std::cerr << "[sendPacket] 发送数据失败：fd=" << fd << "，错误：" << strerror(errno) << std::endl;
//             return false;
//         }
//     }

//     return true;
// }

// int main() {
//     // 新增：打印包头大小（必须是 8 字节！）
//     std::cout << "[调试] 服务端 PacketHeader 大小：" << sizeof(PacketHeader) << " 字节" << std::endl;
    
//     // 启动心跳检测线程
//     std::thread heartbeatThread(heartbeatCheckThread);
//     heartbeatThread.detach();

//     // 创建监听socket（标准TCP服务端流程）
//     int listenFd = socket(AF_INET, SOCK_STREAM, 0);
//     if (listenFd == -1) {
//         std::cerr << "创建socket失败：" << strerror(errno) << std::endl;
//         return -1;
//     }

//     // 绑定端口（8888）
//     sockaddr_in servAddr;
//     memset(&servAddr, 0, sizeof(servAddr));
//     servAddr.sin_family = AF_INET;
//     servAddr.sin_addr.s_addr = INADDR_ANY;
//     servAddr.sin_port = htons(8888);
//     if (bind(listenFd, (sockaddr*)&servAddr, sizeof(servAddr)) == -1) {
//         std::cerr << "绑定端口失败：" << strerror(errno) << std::endl;
//         close(listenFd);
//         return -1;
//     }

//     // 开始监听
//     if (listen(listenFd, 10) == -1) {
//         std::cerr << "监听失败：" << strerror(errno) << std::endl;
//         close(listenFd);
//         return -1;
//     }

//     std::cout << "服务端启动成功，监听端口：8888" << std::endl;

//     // 接受客户端连接（主循环）
//     while (true) {
//         sockaddr_in clientAddr;
//         socklen_t clientAddrLen = sizeof(clientAddr);
//         int clientFd = accept(listenFd, (sockaddr*)&clientAddr, &clientAddrLen);
//         if (clientFd == -1) {
//             std::cerr << "接受连接失败：" << strerror(errno) << std::endl;
//             continue;
//         }

//         // 打印客户端连接信息
//         char clientIp[INET_ADDRSTRLEN];
//         inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, INET_ADDRSTRLEN);
//         uint16_t clientPort = ntohs(clientAddr.sin_port);
//         std::cout << "新客户端连接：fd=" << clientFd << "，IP=" << clientIp << "，端口=" << clientPort << std::endl;

//         // 启动线程处理该客户端（避免阻塞主循环）
//         std::thread clientThread(handleClient, clientFd);
//         clientThread.detach();
//     }

//     close(listenFd);
//     return 0;
// }
