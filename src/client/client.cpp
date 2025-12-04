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
#include <cstdio>
#include <csignal>
#include <cctype>
#include "protocol.h"

using namespace std;
namespace fs = filesystem;

// 客户端配置常量
const int BUF_SIZE = 4096;
const int SERVER_PORT = 8888;
const int DATA_PORT = 9999;
const int FILE_CHUNK_SIZE = 4096;
const int HEARTBEAT_INTERVAL = 10;

// 全局共享数据（统一驼峰命名，与现有代码一致）
mutex gMutex;
atomic<bool> gRunning = true;
int gServerFd = -1;
int gUserId = -1;
string gNickname;
vector<char> gAvatar;
map<int, UserInfo> gOnlineUsers;
map<int, int> gPeerFds;

// 终端模式全局变量
static struct termios oldTermios, newTermios;

// 函数原型声明（统一驼峰命名）
void signalHandler(int signum);
void saveTerminalMode();
void restoreTerminalMode();
bool isCtrlEnter();
string readInputWithHotkeys();
void setNonBlocking(int fd);
bool sendPacket(int fd, MsgType msgType, const vector<char>& data);
bool recvCompletePacket(int fd, PacketHeader& header, vector<char>& data);
vector<char> readFile(const string& filePath);
bool saveFile(const string& filePath, const vector<char>& data);
void heartbeatSendThread();
void peerListenThread();
bool login();
void showOnlineUsers();
int getValidTargetId();
int connectPeer(int targetUserId);
void sendCommonMsg();
void sendImageMsg();
void sendFile();
void handleServerMsg();
void handlePeerMsg(int peerFd);

// 信号处理函数（程序退出时恢复终端模式）
void signalHandler(int signum) {
    gRunning = false;
    restoreTerminalMode();
    cout << "\n📢 收到退出信号，程序正在安全退出..." << endl;
    exit(signum);
}

// 保存终端原始模式
void saveTerminalMode() {
    tcgetattr(STDIN_FILENO, &oldTermios);
    newTermios = oldTermios;
    // 禁用规范模式+回显，逐字符读取
    newTermios.c_lflag &= ~(ICANON | ECHO);
    newTermios.c_cc[VMIN] = 1;
    newTermios.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &newTermios);
}

// 恢复终端原始模式
void restoreTerminalMode() {
    tcsetattr(STDIN_FILENO, TCSANOW, &oldTermios);
}

// 1. 修正：检测Ctrl+Enter组合键（先读'\r'，再读'\n'）
bool isCtrlEnter() {
    char next = 0;
    // 非阻塞读取下一个字符（判断是否为'\n'）
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    
    ssize_t ret = read(STDIN_FILENO, &next, 1);
    
    // 恢复阻塞模式
    fcntl(STDIN_FILENO, F_SETFL, flags);
    
    // Ctrl+Enter的特征：先读到'\r'，下一个字符是'\n'
    if (ret == 1 && next == '\n') {
        return true;
    } else if (ret == 1) {
        ungetc(next, stdin); // 放回非'\n'的字符
    }
    return false;
}

// 2. 修正：读取输入的主逻辑（核心修复）
string readInputWithHotkeys() {
    saveTerminalMode();
    string input;
    char c;
    
    while (true) {
        // 逐字符读取输入
        if (read(STDIN_FILENO, &c, 1) != 1) {
            break;
        }
        
        // 处理退格键（兼容ASCII 127和'\b'）
        if (c == 127 || c == '\b') {
            if (!input.empty()) {
                input.pop_back();
                // 屏幕回显：删除最后一个字符（\b移光标→空格覆盖→\b再移光标）
                cout << "\b \b" << flush;
            }
            continue;
        }
        
        // 核心逻辑：区分普通回车和Ctrl+Enter
        if (c == '\n') { 
            // 普通回车（仅'\n'）→ 结束输入，触发发送
            break;
        } else if (c == '\r') { 
            // 检测到'\r'→ 可能是Ctrl+Enter，检查下一个字符是否为'\n'
            if (isCtrlEnter()) {
                input += '\n'; // 插入换行符
                cout << endl << flush; // 屏幕显示换行
            }
            continue;
        }
        
        // 普通可见字符→ 加入输入，手动回显
        if (isprint(c) || c == '\t') {
            input += c;
            cout << c << flush;
        }
    }
    
    // 输入结束后，换行显示提示
    cout << endl;
    restoreTerminalMode();
    return input;
}

// 设置Socket为非阻塞模式
void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL failed");
        return;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL failed");
    }
}

// 发送完整数据包（头部+数据）
bool sendPacket(int fd, MsgType msgType, const vector<char>& data) {
    PacketHeader header;
    header.msgType = msgType;
    header.dataLen = data.size();

    // 发送头部
    if (send(fd, &header, sizeof(PacketHeader), 0) != sizeof(PacketHeader)) {
        perror("send header failed");
        return false;
    }

    // 发送数据
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

// 接收完整数据包（处理粘包）
bool recvCompletePacket(int fd, PacketHeader& header, vector<char>& data) {
    // 读取头部
    ssize_t ret = recv(fd, &header, sizeof(PacketHeader), 0);
    if (ret == -1) {
        perror("recv header failed");
        return false;
    } else if (ret != sizeof(PacketHeader)) {
        cerr << "recv header incomplete" << endl;
        return false;
    }

    // 读取数据
    data.resize(header.dataLen);
    size_t recved = 0;
    while (recved < header.dataLen) {
        ret = recv(fd, data.data() + recved, header.dataLen - recved, 0);
        if (ret == -1) {
            perror("recv data failed");
            return false;
        } else if (ret == 0) {
            cerr << "connection closed" << endl;
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

    cout << "✅ 文件已保存至：" << filePath << endl;
    return true;
}

// 心跳发送线程
void heartbeatSendThread() {
    while (gRunning && gServerFd != -1) {
        this_thread::sleep_for(chrono::seconds(HEARTBEAT_INTERVAL));
        if (!sendPacket(gServerFd, MsgType::HEARTBEAT, {})) {
            cout << "❌ 心跳包发送失败，连接已断开" << endl;
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

    // 端口复用
    int opt = 1;
    if (setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("❌ setsockopt失败");
        close(listenFd);
        return;
    }

    // 绑定端口
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(DATA_PORT);
    if (bind(listenFd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("❌ bind失败");
        close(listenFd);
        return;
    }

    // 开始监听
    if (listen(listenFd, 5) < 0) {
        perror("❌ listen失败");
        close(listenFd);
        return;
    }

    cout << "✅ 点对点监听已启动，端口：" << DATA_PORT << endl;

    // 接受连接
    while (gRunning) {
        sockaddr_in peerAddr;
        socklen_t addrLen = sizeof(peerAddr);
        int peerFd = accept(listenFd, (sockaddr*)&peerAddr, &addrLen);
        if (peerFd < 0) {
            perror("❌ accept失败");
            continue;
        }

        string peerIp = inet_ntoa(peerAddr.sin_addr);
        cout << "\n📥 新点对点连接：IP=" << peerIp << "，FD=" << peerFd << endl;

        setNonBlocking(peerFd);
        thread peerMsgThread(handlePeerMsg, peerFd);
        peerMsgThread.detach();
    }

    close(listenFd);
}

// 登录流程
bool login() {
    cout << "=====================================" << endl;
    cout << "          聊天客户端 - 登录" << endl;
    cout << "=====================================" << endl;

    // 输入昵称
    cout << "请输入你的昵称：";
    cin >> gNickname;
    cin.ignore(numeric_limits<streamsize>::max(), '\n');

    // 输入头像
    cout << "请输入头像文件路径（无头像直接回车）：";
    string avatarPath;
    getline(cin, avatarPath);
    if (!avatarPath.empty()) {
        gAvatar = readFile(avatarPath);
        if (gAvatar.empty()) {
            cout << "⚠️  头像读取失败，使用默认头像" << endl;
        }
    } else {
        cout << "ℹ️  未设置头像" << endl;
    }

    // 创建服务端Socket
    gServerFd = socket(AF_INET, SOCK_STREAM, 0);
    if (gServerFd < 0) {
        perror("❌ 服务端Socket创建失败");
        return false;
    }

    // 连接服务端
    sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr) <= 0) {
        perror("❌ 无效服务端IP");
        close(gServerFd);
        return false;
    }

    if (connect(gServerFd, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("❌ 连接服务端失败");
        close(gServerFd);
        return false;
    }
    cout << "✅ 已连接服务端（127.0.0.1:" << SERVER_PORT << "）" << endl;

    // 发送登录请求
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
        cout << "❌ 登录请求发送失败" << endl;
        close(gServerFd);
        return false;
    }

    // 接收登录响应
    PacketHeader header;
    vector<char> rspData;
    if (!recvCompletePacket(gServerFd, header, rspData) || header.msgType != MsgType::LOGIN_RSP) {
        cout << "❌ 登录响应接收失败" << endl;
        close(gServerFd);
        return false;
    }

    LoginRsp rsp = deserializeLoginRsp(rspData);
    if (!rsp.success) {
        cout << "❌ 登录失败：" << rsp.msg << endl;
        close(gServerFd);
        return false;
    }

    gUserId = rsp.userId;
    cout << "🎉 登录成功！用户ID：" << gUserId << "，提示：" << rsp.msg << endl;

    // 请求在线用户列表
    if (!sendPacket(gServerFd, MsgType::USER_LIST_REQ, {})) {
        cout << "❌ 用户列表请求失败" << endl;
        close(gServerFd);
        return false;
    }

    return true;
}

// 显示在线用户列表
void showOnlineUsers() {
    lock_guard<mutex> lock(gMutex);
    cout << "\n=====================================" << endl;
    cout << "           在线用户列表" << endl;
    cout << "=====================================" << endl;
    if (gOnlineUsers.empty()) {
        cout << "⚠️  当前无在线用户" << endl;
        cout << "=====================================" << endl;
        return;
    }

    for (auto& [userId, user] : gOnlineUsers) {
        if (userId == gUserId) {
            cout << "ID：" << userId << "（自己） | 昵称：" << user.nickname << " | IP：" << user.ip << endl;
        } else {
            cout << "ID：" << userId << " | 昵称：" << user.nickname << " | IP：" << user.ip << " | 端口：" << user.dataPort << endl;
        }
    }
    cout << "=====================================" << endl;
}

// 获取有效的目标用户ID
int getValidTargetId() {
    int targetUserId = -1;
    while (true) {
        cout << "请输入目标用户ID（-1取消）：";
        cin >> targetUserId;

        if (cin.fail()) {
            cin.clear();
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
            cout << "❌ 输入错误，请输入整数ID" << endl;
            continue;
        }

        if (targetUserId == -1) {
            cout << "ℹ️  已取消" << endl;
            return -1;
        }

        if (targetUserId == gUserId) {
            cout << "❌ 不能选择自己" << endl;
            continue;
        }

        lock_guard<mutex> lock(gMutex);
        if (gOnlineUsers.find(targetUserId) == gOnlineUsers.end()) {
            cout << "❌ 用户不存在或已下线" << endl;
            showOnlineUsers();
            continue;
        }

        cin.ignore(numeric_limits<streamsize>::max(), '\n');
        return targetUserId;
    }
}

// 建立点对点连接
int connectPeer(int targetUserId) {
    lock_guard<mutex> lock(gMutex);

    auto userIt = gOnlineUsers.find(targetUserId);
    if (userIt == gOnlineUsers.end()) {
        cout << "❌ 目标用户不在线" << endl;
        return -1;
    }

    auto peerIt = gPeerFds.find(targetUserId);
    if (peerIt != gPeerFds.end()) {
        return peerIt->second;
    }

    // 创建连接
    int peerFd = socket(AF_INET, SOCK_STREAM, 0);
    if (peerFd < 0) {
        perror("❌ 点对点Socket创建失败");
        return -1;
    }

    sockaddr_in peerAddr;
    memset(&peerAddr, 0, sizeof(peerAddr));
    peerAddr.sin_family = AF_INET;
    peerAddr.sin_port = htons(userIt->second.dataPort);
    if (inet_pton(AF_INET, userIt->second.ip.c_str(), &peerAddr.sin_addr) <= 0) {
        perror("❌ 无效IP");
        close(peerFd);
        return -1;
    }

    if (connect(peerFd, (sockaddr*)&peerAddr, sizeof(peerAddr)) < 0) {
        perror("❌ 连接失败");
        close(peerFd);
        return -1;
    }

    cout << "✅ 已连接用户：" << userIt->second.nickname << "（ID:" << targetUserId << "）" << endl;
    gPeerFds[targetUserId] = peerFd;
    return peerFd;
}

// 发送普通消息（回车发送，Ctrl+Enter换行）
void sendCommonMsg() {
    cout << "\n=====================================" << endl;
    cout << "           发送普通消息" << endl;
    cout << "=====================================" << endl;

    showOnlineUsers();
    int targetUserId = getValidTargetId();
    if (targetUserId == -1) return;

    int peerFd = connectPeer(targetUserId);
    if (peerFd < 0) return;

    cout << "\n请输入消息内容（回车发送，Ctrl+Enter换行）：" << endl;
    cout << "-------------------------------------" << endl;
    string content = readInputWithHotkeys(); // 调用修复后的输入函数
    cout << "-------------------------------------" << endl;

    if (content.empty()) {
        cout << "❌ 消息内容不能为空" << endl;
        return;
    }

    // 构造消息（确保to_user_id字段与protocol.h一致）
    CommonMsg msg;
    msg.fromUserId = gUserId;
    msg.fromNickname = gNickname;
    msg.toUserId = targetUserId; // 重点：与protocol.h的字段名保持一致！
    msg.content = content;

    // 序列化消息
    vector<char> msgData;
    auto nicknameData = serializeString(msg.fromNickname);
    auto contentData = serializeString(msg.content);
    msgData.insert(msgData.end(), (char*)&msg.fromUserId, (char*)&msg.fromUserId + sizeof(int));
    msgData.insert(msgData.end(), (char*)&msg.toUserId, (char*)&msg.toUserId + sizeof(int));
    msgData.insert(msgData.end(), nicknameData.begin(), nicknameData.end());
    msgData.insert(msgData.end(), contentData.begin(), contentData.end());

    if (sendPacket(peerFd, MsgType::COMMON_MSG, msgData)) {
        cout << "✅ 消息发送成功！" << endl;
    } else {
        cout << "❌ 消息发送失败" << endl;
        gPeerFds.erase(targetUserId);
        close(peerFd);
    }
}

// 发送图片消息
void sendImageMsg() {
    cout << "\n=====================================" << endl;
    cout << "           发送图片消息" << endl;
    cout << "=====================================" << endl;

    showOnlineUsers();
    int targetUserId = getValidTargetId();
    if (targetUserId == -1) return;

    int peerFd = connectPeer(targetUserId);
    if (peerFd < 0) return;

    cout << "请输入图片路径（exit取消）：";
    string imgPath;
    getline(cin, imgPath);
    if (imgPath == "exit") {
        cout << "ℹ️  已取消" << endl;
        return;
    }

    vector<char> imgData = readFile(imgPath);
    if (imgData.empty()) return;

    fs::path path(imgPath);
    string imgName = path.filename().string();

    // 构造消息
    ImageMsg msg;
    msg.fromUserId = gUserId;
    msg.fromNickname = gNickname;
    msg.toUserId = targetUserId;
    msg.imgName = imgName;
    msg.imgData = imgData;

    // 序列化
    vector<char> msgData;
    auto nicknameData = serializeString(msg.fromNickname);
    auto imgNameData = serializeString(msg.imgName);
    auto imgDataData = serializeVector(msg.imgData);
    msgData.insert(msgData.end(), (char*)&msg.fromUserId, (char*)&msg.fromUserId + sizeof(int));
    msgData.insert(msgData.end(), (char*)&msg.toUserId, (char*)&msg.toUserId + sizeof(int));
    msgData.insert(msgData.end(), nicknameData.begin(), nicknameData.end());
    msgData.insert(msgData.end(), imgNameData.begin(), imgNameData.end());
    msgData.insert(msgData.end(), imgDataData.begin(), imgDataData.end());

    if (sendPacket(peerFd, MsgType::IMAGE_MSG, msgData)) {
        cout << "✅ 图片发送成功！" << endl;
    } else {
        cout << "❌ 图片发送失败" << endl;
        gPeerFds.erase(targetUserId);
        close(peerFd);
    }
}

// 发送文件
void sendFile() {
    cout << "\n=====================================" << endl;
    cout << "             发送文件" << endl;
    cout << "=====================================" << endl;

    showOnlineUsers();
    int targetUserId = getValidTargetId();
    if (targetUserId == -1) return;

    int peerFd = connectPeer(targetUserId);
    if (peerFd < 0) return;

    cout << "请输入文件路径（exit取消）：";
    string filePath;
    getline(cin, filePath);
    if (filePath == "exit") {
        cout << "ℹ️  已取消" << endl;
        return;
    }

    vector<char> fileData = readFile(filePath);
    if (fileData.empty()) return;

    fs::path path(filePath);
    string fileName = path.filename().string();
    uint64_t fileSize = fileData.size();

    // 发送文件请求
    FileReq req;
    req.fromUserId = gUserId;
    req.fromNickname = gNickname;
    req.toUserId = targetUserId;
    req.fileName = fileName;
    req.fileSize = fileSize;

    vector<char> reqData;
    auto nicknameData = serializeString(req.fromNickname);
    auto fileNameData = serializeString(req.fileName);
    reqData.insert(reqData.end(), (char*)&req.fromUserId, (char*)&req.fromUserId + sizeof(int));
    reqData.insert(reqData.end(), (char*)&req.toUserId, (char*)&req.toUserId + sizeof(int));
    reqData.insert(reqData.end(), nicknameData.begin(), nicknameData.end());
    reqData.insert(reqData.end(), fileNameData.begin(), fileNameData.end());
    reqData.insert(reqData.end(), (char*)&req.fileSize, (char*)&req.fileSize + sizeof(uint64_t));

    if (!sendPacket(peerFd, MsgType::FILE_REQ, reqData)) {
        cout << "❌ 文件请求发送失败" << endl;
        gPeerFds.erase(targetUserId);
        close(peerFd);
        return;
    }

    // 分片发送文件
    uint32_t seq = 0;
    size_t totalChunks = (fileSize + FILE_CHUNK_SIZE - 1) / FILE_CHUNK_SIZE;
    for (size_t i = 0; i < totalChunks; ++i) {
        size_t chunkSize = min((size_t)FILE_CHUNK_SIZE, fileSize - i * FILE_CHUNK_SIZE);
        vector<char> chunkData(fileData.begin() + i * FILE_CHUNK_SIZE, 
                                fileData.begin() + i * FILE_CHUNK_SIZE + chunkSize);

        vector<char> pktData;
        pktData.resize(sizeof(uint32_t) + chunkSize);
        uint32_t currentSeq = seq++;
        memcpy(pktData.data(), &currentSeq, sizeof(uint32_t));
        memcpy(pktData.data() + sizeof(uint32_t), chunkData.data(), chunkSize);

        if (!sendPacket(peerFd, MsgType::FILE_DATA, pktData)) {
            cout << "❌ 分片 " << (i + 1) << "/" << totalChunks << " 发送失败" << endl;
            gPeerFds.erase(targetUserId);
            close(peerFd);
            return;
        }

        // 显示进度
        cout << "📤 传输中：" << (i + 1) << "/" << totalChunks 
             << "（" << (i + 1) * 100 / totalChunks << "%）" << "\r" << flush;
    }

    // 发送结束通知
    sendPacket(peerFd, MsgType::FILE_END, {});
    cout << "\n✅ 文件传输完成！" << endl;
}

// 处理服务端消息
void handleServerMsg() {
    while (gRunning) {
        PacketHeader header;
        vector<char> data;
        if (!recvCompletePacket(gServerFd, header, data)) {
            cout << "\n❌ 与服务端连接断开" << endl;
            gRunning = false;
            break;
        }

        switch (header.msgType) {
            case MsgType::USER_LIST_RSP: {
                lock_guard<mutex> lock(gMutex);
                gOnlineUsers.clear();
                size_t offset = 0;

                // 读取用户数量
                size_t userCount = 0;
                if (offset + sizeof(size_t) <= data.size()) {
                    memcpy(&userCount, data.data() + offset, sizeof(size_t));
                    offset += sizeof(size_t);
                }
                cout << "\nℹ️  在线用户数：" << userCount << endl;

                // 读取每个用户
                for (size_t i = 0; i < userCount; ++i) {
                    UserInfo user;
                    user.nickname = deserializeString(data, offset);
                    user.avatar = deserializeVector(data, offset);
                    user.ip = deserializeString(data, offset);

                    if (offset + sizeof(int) <= data.size()) {
                        memcpy(&user.userId, data.data() + offset, sizeof(int));
                        offset += sizeof(int);
                    }
                    if (offset + sizeof(uint16_t) <= data.size()) {
                        memcpy(&user.dataPort, data.data() + offset, sizeof(uint16_t));
                        offset += sizeof(uint16_t);
                    }

                    gOnlineUsers[user.userId] = user;
                    cout << "ℹ️  加载用户：ID=" << user.userId << "，昵称=" << user.nickname << endl;
                }

                cout << "\n✅ 在线用户列表已更新" << endl;
                showOnlineUsers();
                break;
            }

            case MsgType::USER_ONLINE_NOTIFY: {
                lock_guard<mutex> lock(gMutex);
                UserInfo user;
                size_t offset = 0;

                user.nickname = deserializeString(data, offset);
                user.avatar = deserializeVector(data, offset);
                user.ip = deserializeString(data, offset);
                if (offset + sizeof(int) <= data.size()) {
                    memcpy(&user.userId, data.data() + offset, sizeof(int));
                    offset += sizeof(int);
                }
                if (offset + sizeof(uint16_t) <= data.size()) {
                    memcpy(&user.dataPort, data.data() + offset, sizeof(uint16_t));
                    offset += sizeof(uint16_t);
                }

                gOnlineUsers[user.userId] = user;
                cout << "\n📢 【上线通知】" << user.nickname << "（ID:" << user.userId << "）已上线" << endl;
                showOnlineUsers();
                break;
            }

            case MsgType::USER_OFFLINE_NOTIFY: {
                lock_guard<mutex> lock(gMutex);
                if (data.size() < sizeof(int)) break;

                int userId = 0;
                memcpy(&userId, data.data(), sizeof(int));
                string nickname(data.data() + sizeof(int), data.size() - sizeof(int));

                gOnlineUsers.erase(userId);
                gPeerFds.erase(userId);

                cout << "\n📢 【下线通知】" << nickname << "（ID:" << userId << "）已下线" << endl;
                showOnlineUsers();
                break;
            }

            default:
                cout << "\n❓ 未知服务端消息类型：" << (int)header.msgType << endl;
                break;
        }
    }
}

// 处理点对点消息
void handlePeerMsg(int peerFd) {
    while (gRunning) {
        PacketHeader header;
        vector<char> data;
        if (!recvCompletePacket(peerFd, header, data)) {
            cout << "\n❌ 点对点连接已断开" << endl;
            // 清理连接
            lock_guard<mutex> lock(gMutex);
            for (auto it = gPeerFds.begin(); it != gPeerFds.end(); ) {
                if (it->second == peerFd) {
                    it = gPeerFds.erase(it);
                } else {
                    ++it;
                }
            }
            close(peerFd);
            break;
        }

        switch (header.msgType) {
            case MsgType::COMMON_MSG: {
                size_t offset = 0;
                int fromUserId = 0;
                int toUserId = 0;
                memcpy(&fromUserId, data.data() + offset, sizeof(int));
                offset += sizeof(int);
                memcpy(&toUserId, data.data() + offset, sizeof(int));
                offset += sizeof(int);
                string fromNickname = deserializeString(data, offset);
                string content = deserializeString(data, offset);

                // 只处理发给自己的消息
                if (toUserId == gUserId) {
                    cout << "\n📩 【消息】来自 " << fromNickname << "（ID:" << fromUserId << "）：" << endl;
                    cout << content << endl;
                    // 重新显示菜单提示
                    cout << "\n请输入操作序号（1-5）：";
                    cout.flush();
                }
                break;
            }

            case MsgType::IMAGE_MSG: {
                size_t offset = 0;
                int fromUserId = 0;
                int toUserId = 0;
                memcpy(&fromUserId, data.data() + offset, sizeof(int));
                offset += sizeof(int);
                memcpy(&toUserId, data.data() + offset, sizeof(int));
                offset += sizeof(int);
                string fromNickname = deserializeString(data, offset);
                string imgName = deserializeString(data, offset);
                vector<char> imgData = deserializeVector(data, offset);

                if (toUserId == gUserId) {
                    cout << "\n📩 【图片消息】来自 " << fromNickname << "（ID:" << fromUserId << "）" << endl;
                    saveFile("./recv_" + imgName, imgData);
                    cout << "\n请输入操作序号（1-5）：";
                    cout.flush();
                }
                break;
            }

            case MsgType::FILE_REQ: {
                size_t offset = 0;
                int fromUserId = 0;
                int toUserId = 0;
                memcpy(&fromUserId, data.data() + offset, sizeof(int));
                offset += sizeof(int);
                memcpy(&toUserId, data.data() + offset, sizeof(int));
                offset += sizeof(int);
                string fromNickname = deserializeString(data, offset);
                string fileName = deserializeString(data, offset);
                uint64_t fileSize = 0;
                memcpy(&fileSize, data.data() + offset, sizeof(uint64_t));
                offset += sizeof(uint64_t);

                if (toUserId == gUserId) {
                    cout << "\n📩 【文件请求】来自 " << fromNickname << "（ID:" << fromUserId << "）" << endl;
                    cout << "文件名：" << fileName << " | 大小：" << fileSize << "字节" << endl;
                    cout << "是否接收？（y/n）：";
                    char choice;
                    cin >> choice;
                    cin.ignore(numeric_limits<streamsize>::max(), '\n');

                    if (choice != 'y' && choice != 'Y') {
                        cout << "ℹ️  已拒绝" << endl;
                        gPeerFds.erase(fromUserId);
                        close(peerFd);
                        break;
                    }

                    // 接收文件分片
                    vector<char> recvFileData;
                    recvFileData.reserve(fileSize);
                    bool fileComplete = false;

                    cout << "📥 开始接收文件..." << endl;
                    while (gRunning && !fileComplete) {
                        PacketHeader fileHeader;
                        vector<char> fileData;
                        if (!recvCompletePacket(peerFd, fileHeader, fileData)) {
                            cout << "❌ 文件传输中断" << endl;
                            break;
                        }

                        switch (fileHeader.msgType) {
                            case MsgType::FILE_DATA: {
                                if (fileData.size() >= sizeof(uint32_t)) {
                                    vector<char> chunkData(fileData.begin() + sizeof(uint32_t), fileData.end());
                                    recvFileData.insert(recvFileData.end(), chunkData.begin(), chunkData.end());

                                    // 显示进度
                                    cout << "📥 接收中：" << recvFileData.size() << "/" << fileSize 
                                         << "（" << recvFileData.size() * 100 / fileSize << "%）" << "\r" << flush;
                                }
                                break;
                            }

                            case MsgType::FILE_END: {
                                fileComplete = true;
                                cout << "\n✅ 文件接收完成！" << endl;
                                saveFile("./recv_" + fileName, recvFileData);
                                cout << "\n请输入操作序号（1-5）：";
                                cout.flush();
                                break;
                            }

                            default:
                                cout << "❓ 未知文件消息类型" << endl;
                                break;
                        }
                    }
                }
                break;
            }

            default:
                cout << "\n❓ 未知点对点消息类型：" << (int)header.msgType << endl;
                break;
        }
    }
}

// 主函数
int main() {
    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // 登录
    if (!login()) {
        cout << "❌ 登录失败，程序退出" << endl;
        restoreTerminalMode();
        return -1;
    }

    // 启动后台线程
    thread heartbeatT(heartbeatSendThread);
    thread peerListenT(peerListenThread);
    thread serverMsgT(handleServerMsg);

    // 线程分离
    heartbeatT.detach();
    peerListenT.detach();
    serverMsgT.detach();

    // 主菜单循环
    while (gRunning) {
        cout << "\n=====================================" << endl;
        cout << "          聊天客户端 - 主菜单" << endl;
        cout << "=====================================" << endl;
        cout << "1. 发送普通消息" << endl;
        cout << "2. 发送图片消息" << endl;
        cout << "3. 发送文件" << endl;
        cout << "4. 查看在线用户列表" << endl;
        cout << "5. 退出程序" << endl;
        cout << "=====================================" << endl;
        cout << "请输入操作序号（1-5）：";
        cout.flush();

        int choice;
        cin >> choice;
        cin.ignore(numeric_limits<streamsize>::max(), '\n');

        switch (choice) {
            case 1:
                sendCommonMsg();
                break;
            case 2:
                sendImageMsg();
                break;
            case 3:
                sendFile();
                break;
            case 4:
                showOnlineUsers();
                break;
            case 5:
                gRunning = false;
                cout << "ℹ️  正在退出程序..." << endl;
                break;
            default:
                cout << "❌ 输入错误！请输入1-5之间的序号。" << endl;
                break;
        }
    }

    // 资源释放
    {
        lock_guard<mutex> lock(gMutex);
        for (auto& [userId, peerFd] : gPeerFds) {
            close(peerFd);
        }
        gPeerFds.clear();
        gOnlineUsers.clear();
    }

    if (gServerFd != -1) {
        close(gServerFd);
        gServerFd = -1;
    }

    restoreTerminalMode();
    cout << "✅ 程序已安全退出" << endl;
    return 0;
}