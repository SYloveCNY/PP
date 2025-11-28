#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <mutex>
#include <errno.h>
#include <chrono>
#include <thread>
#include "protocol.h"

using namespace std;
using namespace chrono;

// 服务端配置常量
const int MAX_EVENTS = 1024;        // epoll最大监听事件数
const int BUF_SIZE = 4096;          // 缓冲区大小
const int SERVER_PORT = 8888;       // 服务端监听端口
const int HEARTBEAT_TIMEOUT = 30;   // 心跳超时时间（秒）
const int EPOLL_WAIT_TIMEOUT = 5000;// epoll_wait超时（毫秒）

// 全局共享数据
mutex gMutex;                          // 全局互斥锁
map<int, UserInfo> gOnlineUsers;      // 在线用户列表（user_id -> UserInfo）
map<int, string> gClientIps;          // 客户端FD -> IP地址
map<int, int> gFdToUserId;            // 客户端FD -> 用户ID
int gNextUserId = 1;                  // 下一个分配的用户ID

// 设置socket为非阻塞模式
void setNonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL failed");
        return;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL failed");
    }
}

// 发送完整数据包
bool sendPacket(int fd, MsgType msgType, const vector<char>& data) {
    PacketHeader header;
    header.msgType = msgType;
    header.dataLen = data.size();

    if (send(fd, &header, sizeof(PacketHeader), 0) != sizeof(PacketHeader)) {
        perror("send header failed");
        return false;
    }

    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t ret = send(fd, data.data() + sent, data.size() - sent, 0);
        if (ret <= 0) {
            perror("send data failed");
            return false;
        }
        sent += ret;
    }
    return true;
}

// 接收完整数据包
bool recvCompletePacket(int fd, PacketHeader& header, std::vector<char>& data) {
    ssize_t ret = recv(fd, &header, sizeof(PacketHeader), 0);
    if (ret == -1) {
        perror("recv header failed");
        return false;
    } else if (ret != sizeof(PacketHeader)) {
        std::cerr << "recv header incomplete: 期望" << sizeof(PacketHeader) << "字节，实际" << ret << "字节" << std::endl;
        return false;
    }

    data.resize(header.dataLen);
    size_t recved = 0;
    while (recved < header.dataLen) {
        ret = recv(fd, data.data() + recved, header.dataLen - recved, 0);
        if (ret == -1) {
            perror("recv data failed");
            return false;
        } else if (ret == 0) {
            std::cerr << "connection closed while receiving data" << std::endl;
            return false;
        }
        recved += ret;
    }
    return true;
}

// 广播消息给所有在线用户
void broadcastPacket(MsgType msgType, const vector<char>& data, int excludeFd = -1) {
    lock_guard<mutex> lock(gMutex);
    for (auto& [userId, user] : gOnlineUsers) {
        if (user.managePort != excludeFd) {
            if (!sendPacket(user.managePort, msgType, data)) {
                cerr << "广播消息给用户 [" << user.nickname << "]（ID:" << userId << "）失败" << endl;
            }
        }
    }
}

// 处理客户端登录请求
void handleLoginReq(int clientFd, const vector<char>& data) {
    LoginReq req;
    size_t offset = 0;

    req.nickname = deserializeString(data, offset);
    req.avatar = deserializeVector(data, offset);
    
    if (offset + sizeof(uint16_t) > data.size()) {
        cerr << "handleLoginReq: 数据长度不足，无法读取dataPort" << endl;
        return;
    }
    memcpy(&req.dataPort, data.data() + offset, sizeof(uint16_t));
    offset += sizeof(uint16_t);

    UserInfo user;
    user.userId = gNextUserId++;
    user.nickname = req.nickname;
    user.avatar = req.avatar;
    user.managePort = clientFd;
    user.dataPort = req.dataPort;
    user.lastHeartbeat = system_clock::now();

    {
        lock_guard<mutex> lock(gMutex);
        auto ipIt = gClientIps.find(clientFd);
        if (ipIt != gClientIps.end()) {
            user.ip = ipIt->second;
        } else {
            user.ip = "unknown";
            cerr << "警告：未找到客户端FD=" << clientFd << "的IP地址" << endl;
        }
    }

    {
        lock_guard<mutex> lock(gMutex);
        gOnlineUsers[user.userId] = user;
        gFdToUserId[clientFd] = user.userId;
    }

    LoginRsp rsp;
    rsp.success = true;
    rsp.userId = user.userId;
    rsp.msg = "登录成功！欢迎使用聊天系统～";
    vector<char> rspData = serializeLoginRsp(rsp);
    if (!sendPacket(clientFd, MsgType::LOGIN_RSP, rspData)) {
        cerr << "发送登录响应给用户 [" << user.nickname << "] 失败" << endl;
    } else {
        cout << "用户 [" << user.nickname << "]（ID:" << user.userId << "）登录成功，IP:" << user.ip << endl;
    }

    vector<char> notifyData;
    auto nicknameData = serializeString(user.nickname);
    auto avatarData = serializeVector(user.avatar);
    auto ipData = serializeString(user.ip);
    
    notifyData.insert(notifyData.end(), nicknameData.begin(), nicknameData.end());
    notifyData.insert(notifyData.end(), avatarData.begin(), avatarData.end());
    notifyData.insert(notifyData.end(), ipData.begin(), ipData.end());
    notifyData.insert(notifyData.end(), (char*)&user.userId, (char*)&user.userId + sizeof(int));
    notifyData.insert(notifyData.end(), (char*)&user.dataPort, (char*)&user.dataPort + sizeof(uint16_t));
    
    broadcastPacket(MsgType::USER_ONLINE_NOTIFY, notifyData, clientFd);
}

// 处理客户端的在线用户列表请求
void handleUserListReq(int clientFd) {
    cout << "收到客户端FD=" << clientFd << "的在线用户列表请求" << endl;
    lock_guard<mutex> lock(gMutex);
    vector<char> listData;

    size_t userCount = gOnlineUsers.size();
    listData.resize(sizeof(size_t));
    memcpy(listData.data(), &userCount, sizeof(size_t));

    for (auto& [userId, user] : gOnlineUsers) {
        auto nicknameData = serializeString(user.nickname);
        auto avatarData = serializeVector(user.avatar);
        auto ipData = serializeString(user.ip);
        
        listData.insert(listData.end(), nicknameData.begin(), nicknameData.end());
        listData.insert(listData.end(), avatarData.begin(), avatarData.end());
        listData.insert(listData.end(), ipData.begin(), ipData.end());
        listData.insert(listData.end(), (char*)&user.userId, (char*)&user.userId + sizeof(int));
        listData.insert(listData.end(), (char*)&user.dataPort, (char*)&user.dataPort + sizeof(uint16_t));
    }

    if (!sendPacket(clientFd, MsgType::USER_LIST_RSP, listData)) {
        cerr << "发送在线用户列表给FD=" << clientFd << "失败" << endl;
    } else {
        cout << "已发送在线用户列表给FD=" << clientFd << "，共" << userCount << "个用户" << endl;
    }
}

// 处理客户端断开连接
void handleClientDisconnect(int clientFd) {
    int offlineUserId = -1;
    string offlineNickname = "unknown";

    {
        lock_guard<mutex> lock(gMutex);
        auto fdIt = gFdToUserId.find(clientFd);
        if (fdIt != gFdToUserId.end()) {
            offlineUserId = fdIt->second;
            gFdToUserId.erase(fdIt);
        }
        gClientIps.erase(clientFd);
        auto userIt = gOnlineUsers.find(offlineUserId);
        if (userIt != gOnlineUsers.end()) {
            offlineNickname = userIt->second.nickname;
            gOnlineUsers.erase(userIt);
        }
    }

    if (offlineUserId != -1) {
        vector<char> notifyData;
        notifyData.resize(sizeof(int) + offlineNickname.size());
        memcpy(notifyData.data(), &offlineUserId, sizeof(int));
        memcpy(notifyData.data() + sizeof(int), offlineNickname.data(), offlineNickname.size());
        broadcastPacket(MsgType::USER_OFFLINE_NOTIFY, notifyData, clientFd);

        cout << "用户 [" << offlineNickname << "]（ID:" << offlineUserId << "）下线" << endl;
    } else {
        cout << "客户端FD=" << clientFd << "断开连接（未找到对应用户）" << endl;
    }

    close(clientFd);
}

// 处理客户端发送的心跳包
void handleHeartbeat(int clientFd) {
    lock_guard<mutex> lock(gMutex);
    auto fdIt = gFdToUserId.find(clientFd);
    if (fdIt != gFdToUserId.end()) {
        int userId = fdIt->second;
        auto userIt = gOnlineUsers.find(userId);
        if (userIt != gOnlineUsers.end()) {
            userIt->second.lastHeartbeat = system_clock::now();
        }
    }
}

// 心跳检测线程
void heartbeatCheckThread() {
    while (true) {
        this_thread::sleep_for(seconds(10));
        lock_guard<mutex> lock(gMutex);

        auto now = system_clock::now();
        vector<int> offlineUserFds;

        for (auto& [userId, user] : gOnlineUsers) {
            duration<double> elapsed = now - user.lastHeartbeat;
            if (elapsed.count() > HEARTBEAT_TIMEOUT) {
                offlineUserFds.push_back(user.managePort);
                cout << "用户 [" << user.nickname << "]（ID:" << userId << "）心跳超时，强制下线" << endl;
            }
        }

        for (int fd : offlineUserFds) {
            handleClientDisconnect(fd);
        }
    }
}

int main() {
    cout << "=====================================" << endl;
    cout << "          聊天服务端 - 启动中" << endl;
    cout << "=====================================" << endl;

    // 创建监听Socket
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        perror("❌ 创建监听Socket失败");
        return 1;
    }

    // 设置端口复用
    int opt = 1;
    if (setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("❌ setsockopt失败");
        close(listenFd);
        return 1;
    }

    // 绑定端口
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SERVER_PORT);
    if (bind(listenFd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("❌ 绑定端口失败");
        close(listenFd);
        return 1;
    }

    // 开始监听
    if (listen(listenFd, 10) < 0) {
        perror("❌ 监听失败");
        close(listenFd);
        return 1;
    }
    cout << "✅ 服务端已启动，监听端口：" << SERVER_PORT << endl;

    // 初始化epoll
    int epollFd = epoll_create1(0);
    if (epollFd < 0) {
        perror("❌ epoll_create1失败");
        close(listenFd);
        return 1;
    }

    // 添加监听Socket到epoll
    epoll_event event;
    event.data.fd = listenFd;
    event.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, listenFd, &event) < 0) {
        perror("❌ epoll_ctl添加监听Socket失败");
        close(listenFd);
        close(epollFd);
        return 1;
    }

    // 启动心跳检测线程
    thread heartbeatThread(heartbeatCheckThread);
    heartbeatThread.detach();

    // epoll事件循环
    epoll_event events[MAX_EVENTS];
    while (true) {
        int nfds = epoll_wait(epollFd, events, MAX_EVENTS, EPOLL_WAIT_TIMEOUT);
        if (nfds < 0) {
            perror("❌ epoll_wait失败");
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == listenFd) {
                // 处理新连接
                sockaddr_in clientAddr;
                socklen_t clientAddrLen = sizeof(clientAddr);
                int clientFd = accept(listenFd, (sockaddr*)&clientAddr, &clientAddrLen);
                if (clientFd < 0) {
                    perror("❌ accept失败");
                    continue;
                }

                // 保存客户端IP
                string clientIp = inet_ntoa(clientAddr.sin_addr);
                {
                    lock_guard<mutex> lock(gMutex);
                    gClientIps[clientFd] = clientIp;
                }

                cout << "📥 新客户端连接：IP=" << clientIp << "，FD=" << clientFd << endl;

                // 设置非阻塞并添加到epoll
                setNonblocking(clientFd);
                event.data.fd = clientFd;
                event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                if (epoll_ctl(epollFd, EPOLL_CTL_ADD, clientFd, &event) < 0) {
                    perror("❌ epoll_ctl添加客户端失败");
                    close(clientFd);
                }
            } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 客户端断开连接
                handleClientDisconnect(events[i].data.fd);
                epoll_ctl(epollFd, EPOLL_CTL_DEL, events[i].data.fd, nullptr);
            } else if (events[i].events & EPOLLIN) {
                // 处理客户端消息
                int clientFd = events[i].data.fd;
                PacketHeader header;
                vector<char> data;

                if (!recvCompletePacket(clientFd, header, data)) {
                    handleClientDisconnect(clientFd);
                    epoll_ctl(epollFd, EPOLL_CTL_DEL, clientFd, nullptr);
                    continue;
                }

                switch (header.msgType) {
                    case MsgType::LOGIN_REQ:
                        handleLoginReq(clientFd, data);
                        break;
                    case MsgType::USER_LIST_REQ:
                        handleUserListReq(clientFd);
                        break;
                    case MsgType::HEARTBEAT:
                        handleHeartbeat(clientFd);
                        break;
                    default:
                        cerr << "收到未知消息类型：" << (int)header.msgType << "，客户端FD=" << clientFd << endl;
                }
            }
        }
    }

    // 清理资源
    close(listenFd);
    close(epollFd);
    return 0;
}