#ifndef PROTOCOL_H
#define PROTOCOL_H

// 新增：必须的基础头文件
#include <iostream>   // std::cerr
#include <ostream>    // std::endl
#include <vector>
#include <string>
#include <map>
#include <stdexcept>
#include <cstdint>
#include <arpa/inet.h> // 字节序转换（可选，跨平台用）
#include <QTcpSocket>  // 客户端sendPacket需要QT Socket头文件

// 定义MsgType（建议用uint32_t避免枚举字节大小问题）
using MsgType = uint32_t;
const MsgType LOGIN_REQ = 1;
const MsgType LOGIN_RSP = 2;
const MsgType USER_ONLINE_NOTIFY = 11;
const MsgType USER_OFFLINE_NOTIFY = 12;
const MsgType USER_LIST_REQ = 3;
const MsgType USER_LIST_RSP = 4;
const MsgType HEARTBEAT = 10;
const MsgType COMMON_MSG = 20; // 新增：普通聊天消息（客户端需要）

// 数据包头部
struct PacketHeader {
    MsgType msgType;
    uint32_t dataLen;
};

// LoginReq结构体
struct LoginReq {
    std::string nickname;
    std::vector<char> avatar;
    uint16_t dataPort;
};

// LoginRsp结构体
struct LoginRsp {
    bool success;
    int userId;
    std::string msg;
    std::string nickname;
};

// UserInfo结构体
struct UserInfo {
    int userId;
    std::string nickname;
    std::vector<char> avatar;
    uint16_t dataPort;
    std::string ip;
    int managePort; // 服务端管理连接的端口
};

// CommonMsg结构体（客户端聊天消息）
struct CommonMsg {
    int fromUserId;
    std::string fromNickname;
    std::string content;
    int toUserId; // 0表示广播
};

// ========== 补全缺失的核心函数：serializeVector/deserializeVector ==========
// 检查长度合法性（防止超大值）
inline bool isValidLength(uint32_t len, size_t maxAllowed = 1024 * 1024) {
    return len <= maxAllowed && len != 0xFFFFFFFF;
}

// 字符串序列化（基础函数：修复const_cast问题）
inline std::vector<char> serializeString(const std::string& str) {
    std::vector<char> data;
    uint32_t len = static_cast<uint32_t>(str.size());
    len = htonl(len); // 转网络字节序（跨平台必加）
    // 修复：reinterpret_cast<const char*> 匹配const参数
    data.insert(data.end(), reinterpret_cast<const char*>(&len), 
                reinterpret_cast<const char*>(&len) + sizeof(uint32_t));
    if (len > 0) {
        data.insert(data.end(), str.begin(), str.end());
    }
    return data;
}

// 字符串反序列化（基础函数）
inline std::string deserializeString(const std::vector<char>& data, size_t& offset) {
    if (offset + sizeof(uint32_t) > data.size()) {
        throw std::out_of_range("String length field out of bounds");
    }
    uint32_t len = *reinterpret_cast<const uint32_t*>(data.data() + offset);
    len = ntohl(len); // 转主机字节序
    offset += sizeof(uint32_t);
    
    if (!isValidLength(len) || (offset + len) > data.size()) {
        throw std::length_error("Invalid string length: " + std::to_string(len));
    }
    
    std::string str;
    if (len > 0) {
        str.assign(data.data() + offset, data.data() + offset + len);
    }
    offset += len;
    return str;
}

// 二进制vector（头像）序列化（修复const_cast问题）
inline std::vector<char> serializeVector(const std::vector<char>& vec) {
    std::vector<char> data;
    uint32_t len = static_cast<uint32_t>(vec.size());
    len = htonl(len); // 转网络字节序
    // 修复：reinterpret_cast<const char*>
    data.insert(data.end(), reinterpret_cast<const char*>(&len), 
                reinterpret_cast<const char*>(&len) + sizeof(uint32_t));
    if (len > 0) {
        data.insert(data.end(), vec.begin(), vec.end());
    }
    return data;
}

// 二进制vector（头像）反序列化
inline std::vector<char> deserializeVector(const std::vector<char>& data, size_t& offset) {
    if (offset + sizeof(uint32_t) > data.size()) {
        throw std::out_of_range("Vector length field out of bounds");
    }
    uint32_t len = *reinterpret_cast<const uint32_t*>(data.data() + offset);
    len = ntohl(len); // 转主机字节序
    offset += sizeof(uint32_t);
    
    if (!isValidLength(len) || (offset + len) > data.size()) {
        throw std::length_error("Invalid vector length: " + std::to_string(len));
    }
    
    std::vector<char> vec;
    if (len > 0) {
        vec.assign(data.data() + offset, data.data() + offset + len);
    }
    offset += len;
    return vec;
}

// ========== LoginReq序列化/反序列化 ==========
inline std::vector<char> serializeLoginReq(const LoginReq& req) {
    std::vector<char> data;
    // 序列化昵称
    auto nicknameData = serializeString(req.nickname);
    data.insert(data.end(), nicknameData.begin(), nicknameData.end());
    // 序列化头像
    auto avatarData = serializeVector(req.avatar);
    data.insert(data.end(), avatarData.begin(), avatarData.end());
    // 序列化dataPort（转网络字节序 + 修复const_cast）
    uint16_t port = htons(req.dataPort);
    data.insert(data.end(), reinterpret_cast<const char*>(&port), 
                reinterpret_cast<const char*>(&port) + sizeof(uint16_t));
    return data;
}

inline LoginReq deserializeLoginReq(const std::vector<char>& data) {
    LoginReq req;
    size_t offset = 0;
    try {
        req.nickname = deserializeString(data, offset);
        req.avatar = deserializeVector(data, offset);
        
        if (offset + sizeof(uint16_t) > data.size()) {
            throw std::out_of_range("DataPort field out of bounds");
        }
        uint16_t port = *reinterpret_cast<const uint16_t*>(data.data() + offset);
        req.dataPort = ntohs(port); // 转主机字节序
        offset += sizeof(uint16_t);
    } catch (const std::exception& e) {
        std::cerr << "Deserialize LoginReq failed: " << e.what() << std::endl;
        req = LoginReq{}; // 重置为空
    }
    return req;
}

// ========== LoginRsp序列化/反序列化 ==========
inline LoginRsp deserializeLoginRsp(const std::vector<char>& data) {
    LoginRsp rsp;
    size_t offset = 0;
    try {
        // 解析success
        if (offset + sizeof(bool) > data.size()) {
            throw std::out_of_range("Success field out of bounds");
        }
        rsp.success = *reinterpret_cast<const bool*>(data.data() + offset);
        offset += sizeof(bool);
        // 解析userId
        if (offset + sizeof(int) > data.size()) {
            throw std::out_of_range("UserID field out of bounds");
        }
        rsp.userId = *reinterpret_cast<const int*>(data.data() + offset);
        offset += sizeof(int);
        // 解析msg和nickname
        rsp.msg = deserializeString(data, offset);
        rsp.nickname = deserializeString(data, offset);
    } catch (const std::exception& e) {
        std::cerr << "Deserialize LoginRsp failed: " << e.what() << std::endl;
        rsp = LoginRsp{};
    }
    return rsp;
}

inline std::vector<char> serializeLoginRsp(const LoginRsp& rsp) {
    std::vector<char> data;
    // 修复const_cast问题：全部用const char*
    data.insert(data.end(), reinterpret_cast<const char*>(&rsp.success),
                reinterpret_cast<const char*>(&rsp.success) + sizeof(bool));
    data.insert(data.end(), reinterpret_cast<const char*>(&rsp.userId),
                reinterpret_cast<const char*>(&rsp.userId) + sizeof(int));
    // 序列化msg和nickname
    auto msgData = serializeString(rsp.msg);
    data.insert(data.end(), msgData.begin(), msgData.end());
    auto nicknameData = serializeString(rsp.nickname);
    data.insert(data.end(), nicknameData.begin(), nicknameData.end());
    return data;
}

// ========== UserInfo序列化/反序列化 ==========
inline std::vector<char> serializeUserInfo(const UserInfo& user) {
    std::vector<char> data;
    // 修复const_cast问题
    data.insert(data.end(), reinterpret_cast<const char*>(&user.userId),
                reinterpret_cast<const char*>(&user.userId) + sizeof(int));
    // 序列化昵称
    auto nicknameData = serializeString(user.nickname);
    data.insert(data.end(), nicknameData.begin(), nicknameData.end());
    // 序列化头像
    auto avatarData = serializeVector(user.avatar);
    data.insert(data.end(), avatarData.begin(), avatarData.end());
    // 序列化dataPort（转网络字节序）
    uint16_t port = htons(user.dataPort);
    data.insert(data.end(), reinterpret_cast<const char*>(&port),
                reinterpret_cast<const char*>(&port) + sizeof(uint16_t));
    // 序列化IP
    auto ipData = serializeString(user.ip);
    data.insert(data.end(), ipData.begin(), ipData.end());
    // 序列化managePort
    data.insert(data.end(), reinterpret_cast<const char*>(&user.managePort),
                reinterpret_cast<const char*>(&user.managePort) + sizeof(int));
    return data;
}

inline UserInfo deserializeUserInfo(const std::vector<char>& data, size_t& offset) {
    UserInfo user;
    // 解析userId
    if (offset + sizeof(int) > data.size()) {
        throw std::out_of_range("UserID field out of bounds");
    }
    user.userId = *reinterpret_cast<const int*>(data.data() + offset);
    offset += sizeof(int);
    // 解析昵称
    user.nickname = deserializeString(data, offset);
    // 解析头像
    user.avatar = deserializeVector(data, offset);
    // 解析dataPort
    if (offset + sizeof(uint16_t) > data.size()) {
        throw std::out_of_range("DataPort field out of bounds");
    }
    uint16_t port = *reinterpret_cast<const uint16_t*>(data.data() + offset);
    user.dataPort = ntohs(port);
    offset += sizeof(uint16_t);
    // 解析IP
    user.ip = deserializeString(data, offset);
    // 解析managePort
    if (offset + sizeof(int) > data.size()) {
        throw std::out_of_range("ManagePort field out of bounds");
    }
    user.managePort = *reinterpret_cast<const int*>(data.data() + offset);
    offset += sizeof(int);
    return user;
}

// 新增：无offset的deserializeUserInfo（适配客户端调用）
inline UserInfo deserializeUserInfo(const std::vector<char>& data) {
    size_t offset = 0;
    return deserializeUserInfo(data, offset);
}

// ========== UserList反序列化 ==========
inline std::map<int, UserInfo> deserializeUserList(const std::vector<char>& data) {
    std::map<int, UserInfo> userList;
    size_t offset = 0;
    // 解析用户数量
    if (offset + sizeof(uint32_t) > data.size()) {
        throw std::out_of_range("User count field out of bounds");
    }
    uint32_t count = *reinterpret_cast<const uint32_t*>(data.data() + offset);
    count = ntohl(count);
    offset += sizeof(uint32_t);
    
    for (uint32_t i = 0; i < count; ++i) {
        UserInfo user = deserializeUserInfo(data, offset);
        userList[user.userId] = user;
    }
    return userList;
}

// ========== CommonMsg序列化/反序列化（客户端聊天消息） ==========
inline std::vector<char> serializeCommonMsg(const CommonMsg& msg) {
    std::vector<char> data;
    // 严格按照：fromUserId → toUserId → fromNickname → content 的顺序序列化
    data.insert(data.end(), (char*)&msg.fromUserId, (char*)&msg.fromUserId + sizeof(int));
    data.insert(data.end(), (char*)&msg.toUserId, (char*)&msg.toUserId + sizeof(int));
    
    auto nicknameData = serializeString(msg.fromNickname);
    data.insert(data.end(), nicknameData.begin(), nicknameData.end());
    
    auto contentData = serializeString(msg.content);
    data.insert(data.end(), contentData.begin(), contentData.end());
    return data;
}

inline CommonMsg deserializeCommonMsg(const std::vector<char>& data) {
    CommonMsg msg;
    size_t offset = 0;
    try {
        // 解析fromUserId
        if (offset + sizeof(int) > data.size()) {
            throw std::out_of_range("fromUserId field out of bounds");
        }
        msg.fromUserId = *reinterpret_cast<const int*>(data.data() + offset);
        offset += sizeof(int);
        // 解析fromNickname
        msg.fromNickname = deserializeString(data, offset);
        // 解析content
        msg.content = deserializeString(data, offset);
        // 解析toUserId
        if (offset + sizeof(int) > data.size()) {
            throw std::out_of_range("toUserId field out of bounds");
        }
        msg.toUserId = *reinterpret_cast<const int*>(data.data() + offset);
        offset += sizeof(int);
    } catch (const std::exception& e) {
        std::cerr << "Deserialize CommonMsg failed: " << e.what() << std::endl;
        msg = CommonMsg{};
    }
    return msg;
}

// ========== 客户端QT版本sendPacket函数 ==========
inline bool sendPacket(QTcpSocket* socket, uint32_t msgType, const std::vector<char>& data) {
    PacketHeader header;
    header.msgType = msgType;
    header.dataLen = static_cast<uint32_t>(data.size());
    
    // 组装发送数据
    std::vector<char> sendData;
    sendData.insert(sendData.end(), reinterpret_cast<const char*>(&header),
                    reinterpret_cast<const char*>(&header) + sizeof(PacketHeader));
    sendData.insert(sendData.end(), data.begin(), data.end());
    
    // QT发送
    qint64 sent = socket->write(sendData.data(), sendData.size());
    return sent == sendData.size();
}

#endif // PROTOCOL_H