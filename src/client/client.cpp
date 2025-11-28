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
#include <fstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <limits>
#include <chrono>
#include <termios.h>
#include "protocol.h"

using namespace std;
namespace fs = filesystem;

// ====================== 新增：函数原型声明 ======================
// 提前声明 recvCompletePacket，让 handlePeerMsg 能调用
bool recvCompletePacket(int fd, PacketHeader& header, vector<char>& data);
// （可选：如果还有其他函数调用顺序问题，可在此处添加其他声明，如 sendPacket）
void setNonblocking(int fd);
bool sendPacket(int fd, MsgType msgType, const vector<char>& data);
// ===============================================================

// 客户端配置常量
const int BUF_SIZE = 4096;              // 缓冲区大小
const int SERVER_PORT = 8888;           // 服务端通信端口
const int DATA_PORT = 9999;             // 点对点数据传输端口
const int FILE_CHUNK_SIZE = 4096;       // 文件分片大小
const int HEARTBEAT_INTERVAL = 10;      // 心跳发送间隔（秒）

// 全局共享数据
mutex gMutex;                          // 全局互斥锁
atomic<bool> gRunning = true;          // 程序运行标志
int gServerFd = -1;                   // 与服务端通信的Socket FD
int gUserId = -1;                     // 自身用户ID
string gNickname;                      // 自身昵称
vector<char> gAvatar;                  // 自身头像二进制数据
map<int, UserInfo> gOnlineUsers;      // 在线用户列表（user_id -> UserInfo）
map<int, int> gPeerFds;               // 点对点连接缓存（对方user_id -> 连接FD）

// 处理点对点消息
void handlePeerMsg(int peerFd) {
    while (gRunning) {
        PacketHeader header;
        vector<char> data;
        if (!recvCompletePacket(peerFd, header, data)) {
            cerr << "点对点连接读取失败，FD=" << peerFd << endl;
            break;
        }

        switch (header.msgType) {
            case MsgType::COMMON_MSG: {
                CommonMsg msg;
                size_t offset = 0;
                memcpy(&msg.fromUserId, data.data() + offset, sizeof(int));
                offset += sizeof(int);
                msg.fromNickname = deserializeString(data, offset);
                msg.content = deserializeString(data, offset);
                
                lock_guard<mutex> lock(gMutex);
                cout << "\n📩 收到来自 " << msg.fromNickname << " 的消息：" << endl;
                cout << msg.content << endl;
                break;
            }
            default:
                cerr << "未知的点对点消息类型：" << (int)header.msgType << endl;
        }
    }

    // 清理连接
    lock_guard<mutex> lock(gMutex);
    for (auto it = gPeerFds.begin(); it != gPeerFds.end();) {
        if (it->second == peerFd) {
            it = gPeerFds.erase(it);
        } else {
            ++it;
        }
    }
    close(peerFd);
    cout << "点对点连接已关闭，FD=" << peerFd << endl;
}

// 设置Socket为非阻塞模式
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
bool recvCompletePacket(int fd, PacketHeader& header, vector<char>& data) {
    ssize_t ret = recv(fd, &header, sizeof(PacketHeader), 0);
    if (ret == -1) {
        perror("recv header failed");
        return false;
    } else if (ret != sizeof(PacketHeader)) {
        cerr << "recv header incomplete: 期望" << sizeof(PacketHeader) << "字节，实际" << ret << "字节" << endl;
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
            cerr << "connection closed while receiving data" << endl;
            return false;
        }
        recved += ret;
    }
    return true;
}

// 读取文件为二进制数据
vector<char> readFile(const string& filePath) {
    ifstream file(filePath, ios::binary | ios::ate);
    if (!file.is_open()) {
        cout << "❌ 文件打开失败：" << filePath << endl;
        return {};
    }

    streampos fileSize = file.tellg();
    vector<char> data(fileSize);
    file.seekg(0, ios::beg);
    file.read(data.data(), fileSize);
    file.close();

    cout << "✅ 文件读取成功：" << filePath << "，大小：" << fileSize << "字节" << endl;
    return data;
}

// 保存二进制数据为文件
bool saveFile(const string& filePath, const vector<char>& data) {
    ofstream file(filePath, ios::binary);
    if (!file.is_open()) {
        cout << "❌ 文件保存失败：" << filePath << endl;
        return false;
    }

    file.write(data.data(), data.size());
    file.close();

    cout << "✅ 文件已保存至：" << filePath << "，大小：" << data.size() << "字节" << endl;
    return true;
}

// 心跳发送线程
void heartbeatSendThread() {
    while (gRunning && gServerFd != -1) {
        this_thread::sleep_for(chrono::seconds(HEARTBEAT_INTERVAL));
        if (!sendPacket(gServerFd, MsgType::HEARTBEAT, {})) {
            cout << "❌ 心跳包发送失败，与服务端连接可能已断开" << endl;
            gRunning = false;
            break;
        }
    }
}

// 点对点连接监听线程
void peerListenThread() {
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        perror("❌ 点对点监听Socket创建失败");
        return;
    }

    int opt = 1;
    if (setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("❌ 点对点监听setsockopt失败");
        close(listenFd);
        return;
    }

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(DATA_PORT);
    if (bind(listenFd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("❌ 点对点监听bind失败");
        close(listenFd);
        return;
    }

    if (listen(listenFd, 5) < 0) {
        perror("❌ 点对点监听listen失败");
        close(listenFd);
        return;
    }

    cout << "✅ 点对点数据传输监听已启动，端口：" << DATA_PORT << endl;

    while (gRunning) {
        sockaddr_in peerAddr;
        socklen_t addrLen = sizeof(peerAddr);
        int peerFd = accept(listenFd, (sockaddr*)&peerAddr, &addrLen);
        if (peerFd < 0) {
            perror("❌ 点对点accept失败");
            continue;
        }

        string peerIp = inet_ntoa(peerAddr.sin_addr);
        cout << "\n📥 收到新的点对点连接：IP=" << peerIp << "，FD=" << peerFd << endl;

        setNonblocking(peerFd);
        thread peerMsgThread(handlePeerMsg, peerFd);
        peerMsgThread.detach();
    }

    close(listenFd);
}

// 从服务端接收消息的线程
void recvServerMsgThread() {
    while (gRunning) {
        PacketHeader header;
        vector<char> data;
        if (!recvCompletePacket(gServerFd, header, data)) {
            cerr << "与服务端通信异常，即将退出" << endl;
            gRunning = false;
            break;
        }

        switch (header.msgType) {
            case MsgType::LOGIN_RSP: {
                LoginRsp rsp = deserializeLoginRsp(data);
                if (rsp.success) {
                    gUserId = rsp.userId;
                    cout << "✅ " << rsp.msg << "，你的用户ID：" << gUserId << endl;
                } else {
                    cout << "❌ 登录失败：" << rsp.msg << endl;
                    gRunning = false;
                }
                break;
            }
            case MsgType::USER_ONLINE_NOTIFY: {
                size_t offset = 0;
                UserInfo user;
                user.nickname = deserializeString(data, offset);
                user.avatar = deserializeVector(data, offset);
                user.ip = deserializeString(data, offset);
                memcpy(&user.userId, data.data() + offset, sizeof(int));
                offset += sizeof(int);
                memcpy(&user.dataPort, data.data() + offset, sizeof(uint16_t));
                
                lock_guard<mutex> lock(gMutex);
                gOnlineUsers[user.userId] = user;
                cout << "\n📢 用户上线：" << user.nickname << "（ID：" << user.userId << "）" << endl;
                break;
            }
            case MsgType::USER_OFFLINE_NOTIFY: {
                int userId;
                string nickname;
                memcpy(&userId, data.data(), sizeof(int));
                nickname = string(data.data() + sizeof(int), data.size() - sizeof(int));
                
                lock_guard<mutex> lock(gMutex);
                gOnlineUsers.erase(userId);
                cout << "\n📢 用户下线：" << nickname << "（ID：" << userId << "）" << endl;
                break;
            }
            case MsgType::USER_LIST_RSP: {
                size_t offset = 0;
                size_t userCount;
                memcpy(&userCount, data.data(), sizeof(size_t));
                offset += sizeof(size_t);
                
                lock_guard<mutex> lock(gMutex);
                gOnlineUsers.clear();
                for (size_t i = 0; i < userCount; ++i) {
                    UserInfo user;
                    user.nickname = deserializeString(data, offset);
                    user.avatar = deserializeVector(data, offset);
                    user.ip = deserializeString(data, offset);
                    memcpy(&user.userId, data.data() + offset, sizeof(int));
                    offset += sizeof(int);
                    memcpy(&user.dataPort, data.data() + offset, sizeof(uint16_t));
                    offset += sizeof(uint16_t);
                    gOnlineUsers[user.userId] = user;
                }
                cout << "✅ 已获取在线用户列表，共 " << userCount << " 人" << endl;
                break;
            }
            default:
                cerr << "收到未知类型的服务端消息：" << (int)header.msgType << endl;
        }
    }
}

// 请求在线用户列表
void requestUserList() {
    if (gServerFd == -1) {
        cout << "❌ 未连接到服务端" << endl;
        return;
    }
    if (!sendPacket(gServerFd, MsgType::USER_LIST_REQ, {})) {
        cout << "❌ 请求用户列表失败" << endl;
    }
}

// 选择收信人并确认
int selectRecipient() {
    lock_guard<mutex> lock(gMutex);
    if (gOnlineUsers.empty()) {
        cout << "当前没有在线用户" << endl;
        return -1;
    }

    cout << "\n===== 在线用户列表 =====" << endl;
    for (const auto& [userId, userInfo] : gOnlineUsers) {
        if (userId == gUserId) continue;
        cout << userId << ". " << userInfo.nickname << "（IP：" << userInfo.ip << "）" << endl;
    }

    int targetUserId;
    cout << "请选择收信人ID: ";
    cin >> targetUserId;
    cin.ignore(numeric_limits<streamsize>::max(), '\n');

    auto it = gOnlineUsers.find(targetUserId);
    if (it == gOnlineUsers.end() || targetUserId == gUserId) {
        cout << "无效的用户ID" << endl;
        return -1;
    }

    cout << "你选择的收信人是: " << it->second.nickname << " (ID: " << targetUserId << ")" << endl;
    cout << "确认发送消息给该用户? (y/n): ";
    string confirm;
    getline(cin, confirm);
    if (confirm != "y" && confirm != "Y") {
        cout << "已取消发送" << endl;
        return -1;
    }

    return targetUserId;
}

// 获取点对点连接FD（不存在则创建）
int getPeerFd(int targetUserId) {
    lock_guard<mutex> lock(gMutex);
    auto it = gPeerFds.find(targetUserId);
    if (it != gPeerFds.end()) {
        return it->second;
    }

    auto userIt = gOnlineUsers.find(targetUserId);
    if (userIt == gOnlineUsers.end()) {
        return -1;
    }

    int peerFd = socket(AF_INET, SOCK_STREAM, 0);
    if (peerFd < 0) {
        perror("创建点对点Socket失败");
        return -1;
    }

    sockaddr_in peerAddr;
    memset(&peerAddr, 0, sizeof(peerAddr));
    peerAddr.sin_family = AF_INET;
    peerAddr.sin_port = htons(userIt->second.dataPort);
    if (inet_pton(AF_INET, userIt->second.ip.c_str(), &peerAddr.sin_addr) <= 0) {
        perror("无效的 peer IP");
        close(peerFd);
        return -1;
    }

    if (connect(peerFd, (sockaddr*)&peerAddr, sizeof(peerAddr)) < 0) {
        perror("连接到 peer 失败");
        close(peerFd);
        return -1;
    }

    setNonblocking(peerFd);
    gPeerFds[targetUserId] = peerFd;
    
    thread peerMsgThread(handlePeerMsg, peerFd);
    peerMsgThread.detach();
    
    return peerFd;
}

// 消息输入处理（回车换行，Ctrl+Enter发送）
string inputMessage() {
    cout << "\n请输入消息内容（Ctrl+Enter发送，回车换行）: " << endl;
    string message;
    struct termios oldT, newT;
    
    tcgetattr(STDIN_FILENO, &oldT);
    newT = oldT;
    newT.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newT);

    while (true) {
        char c = getchar();
        if (c == '\n') {
            struct termios tempT;
            tcgetattr(STDIN_FILENO, &tempT);
            if ((tempT.c_cc[VINTR] & 0x1F) == 0x0A) { // Ctrl+Enter
                break;
            } else {
                message += '\n';
                cout << endl;
            }
        } else {
            message += c;
            cout << c;
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &oldT);
    return message;
}

// 发送消息
void sendMessage(int targetUserId) {
    string content = inputMessage();
    if (content.empty()) {
        cout << "消息内容不能为空" << endl;
        return;
    }

    CommonMsg msg;
    msg.fromUserId = gUserId;
    msg.fromNickname = gNickname;
    msg.content = content;

    vector<char> data;
    auto fromNicknameData = serializeString(msg.fromNickname);
    auto contentData = serializeString(msg.content);
    
    data.insert(data.end(), (char*)&msg.fromUserId, (char*)&msg.fromUserId + sizeof(int));
    data.insert(data.end(), fromNicknameData.begin(), fromNicknameData.end());
    data.insert(data.end(), contentData.begin(), contentData.end());

    int peerFd = getPeerFd(targetUserId);
    if (peerFd == -1 || !sendPacket(peerFd, MsgType::COMMON_MSG, data)) {
        cout << "消息发送失败" << endl;
    } else {
        cout << "\n消息已发送，无法修改或删除" << endl;
    }
}

// 登录流程
bool login() {
    cout << "=====================================" << endl;
    cout << "          聊天客户端 - 登录" << endl;
    cout << "=====================================" << endl;

    cout << "请输入你的昵称：";
    cin >> gNickname;
    cin.ignore(numeric_limits<streamsize>::max(), '\n');

    cout << "请输入头像文件路径（如：./avatar.jpg，无头像直接回车）：";
    string avatarPath;
    getline(cin, avatarPath);
    if (!avatarPath.empty()) {
        gAvatar = readFile(avatarPath);
        if (gAvatar.empty()) {
            cout << "⚠️  头像文件读取失败，将使用默认空头像" << endl;
        } else {
            cout << "✅ 头像读取成功，大小：" << gAvatar.size() << "字节" << endl;
        }
    } else {
        cout << "ℹ️  未设置头像，将使用默认空头像" << endl;
    }

    gServerFd = socket(AF_INET, SOCK_STREAM, 0);
    if (gServerFd < 0) {
        perror("❌ 服务端Socket创建失败");
        return false;
    }

    sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr) <= 0) {
        perror("❌ 无效的服务端IP");
        close(gServerFd);
        return false;
    }

    if (connect(gServerFd, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("❌ 连接服务端失败（请确保服务端已启动）");
        close(gServerFd);
        return false;
    }
    cout << "✅ 已连接服务端（IP：127.0.0.1，端口：" << SERVER_PORT << "）" << endl;

    LoginReq req;
    req.nickname = gNickname;
    req.avatar = gAvatar;
    req.dataPort = DATA_PORT;

    vector<char> reqData;
    auto nicknameData = serializeString(req.nickname);
    auto avatarData = serializeVector(req.avatar);
    
    reqData.insert(reqData.end(), nicknameData.begin(), nicknameData.end());
    reqData.insert(reqData.end(), avatarData.begin(), avatarData.end());
    reqData.insert(reqData.end(), (char*)&req.dataPort, (char*)&req.dataPort + sizeof(uint16_t));

    if (!sendPacket(gServerFd, MsgType::LOGIN_REQ, reqData)) {
        cout << "❌ 发送登录请求失败" << endl;
        close(gServerFd);
        return false;
    }

    return true;
}

// 主循环
void mainLoop() {
    while (gRunning) {
        cout << "\n===== 功能菜单 =====" << endl;
        cout << "1. 查看在线用户" << endl;
        cout << "2. 发送消息" << endl;
        cout << "3. 发送文件" << endl;
        cout << "4. 退出" << endl;
        cout << "请选择功能: ";
        
        int choice;
        cin >> choice;
        cin.ignore(numeric_limits<streamsize>::max(), '\n');

        switch (choice) {
            case 1:
                requestUserList();
                break;
            case 2: {
                int targetUserId = selectRecipient();
                if (targetUserId != -1) {
                    sendMessage(targetUserId);
                }
                break;
            }
            case 3:
                cout << "文件发送功能待实现" << endl;
                break;
            case 4:
                gRunning = false;
                break;
            default:
                cout << "无效的选择" << endl;
        }
    }
}

int main() {
    // 登录流程
    if (!login()) {
        return 1;
    }

    // 启动辅助线程
    thread recvThread(recvServerMsgThread);
    thread heartbeatThread(heartbeatSendThread);
    thread peerListenThreadInst(peerListenThread); 

    // 进入主交互循环
    mainLoop();

    // 清理资源
    gRunning = false;
    close(gServerFd);
    
    lock_guard<mutex> lock(gMutex);
    for (auto& [userId, fd] : gPeerFds) {
        close(fd);
    }

    // 等待线程结束
    if (recvThread.joinable()) recvThread.join();
    if (heartbeatThread.joinable()) heartbeatThread.join();
    if (peerListenThreadInst.joinable()) peerListenThreadInst.join();

    cout << "客户端已退出" << endl;
    return 0;
}