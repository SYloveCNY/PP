#ifndef PROTOCOL_BASE_H
#define PROTOCOL_BASE_H

// 仅保留纯C++头文件，无QT依赖
#include <iostream>
#include <ostream>
#include <vector>
#include <string>
#include <map>
#include <stdexcept>
#include <cstdint>
#include <arpa/inet.h>
#include <cstring> // strerror依赖

// ========== 基础类型定义（无QT） ==========
using MsgType = uint32_t;
// 协议常量（删除MsgType::前缀，直接用常量）
const uint32_t LOGIN_REQ = 1;
const uint32_t LOGIN_RSP = 2;
const uint32_t USER_ONLINE_NOTIFY = 11;
const uint32_t USER_OFFLINE_NOTIFY = 12;
const uint32_t USER_LIST_REQ = 3;
const uint32_t USER_LIST_RSP = 4;
const uint32_t HEARTBEAT = 10;
const uint32_t COMMON_MSG = 20;

// 数据包头部（纯C++结构体）
struct PacketHeader {
    MsgType msgType;
    uint32_t dataLen;
};

// 核心结构体（纯C++）
struct LoginReq {
    std::string nickname;
    std::vector<char> avatar;
    uint16_t dataPort;
};

struct LoginRsp {
    bool success;
    int userId;
    std::string msg;
    std::string nickname;
};

struct UserInfo {
    int userId;
    std::string nickname;
    std::vector<char> avatar;
    uint16_t dataPort;
    std::string ip;
    int managePort;
};

struct CommonMsg {
    int fromUserId;
    std::string fromNickname;
    std::string content;
    int toUserId; // 0=广播
};

// ========== 通用工具函数 ==========
inline bool isValidLength(uint32_t len, size_t maxAllowed = 1024 * 1024) {
    return len <= maxAllowed && len != 0xFFFFFFFF;
}

// ========== 基础序列化/反序列化（纯C++） ==========
inline std::vector<char> serializeString(const std::string& str) {
    std::vector<char> data;
    uint32_t len = static_cast<uint32_t>(str.size());
    len = htonl(len); // 网络字节序
    data.insert(data.end(), reinterpret_cast<const char*>(&len),
                reinterpret_cast<const char*>(&len) + sizeof(uint32_t));
    if (len > 0) {
        data.insert(data.end(), str.begin(), str.end());
    }
    return data;
}

inline std::string deserializeString(const std::vector<char>& data, size_t& offset) {
    if (offset + sizeof(uint32_t) > data.size()) {
        throw std::out_of_range("String length field out of bounds");
    }
    uint32_t len = *reinterpret_cast<const uint32_t*>(data.data() + offset);
    len = ntohl(len);
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

inline std::vector<char> serializeVector(const std::vector<char>& vec) {
    std::vector<char> data;
    uint32_t len = static_cast<uint32_t>(vec.size());
    len = htonl(len);
    data.insert(data.end(), reinterpret_cast<const char*>(&len),
                reinterpret_cast<const char*>(&len) + sizeof(uint32_t));
    if (len > 0) {
        data.insert(data.end(), vec.begin(), vec.end());
    }
    return data;
}

inline std::vector<char> deserializeVector(const std::vector<char>& data, size_t& offset) {
    if (offset + sizeof(uint32_t) > data.size()) {
        throw std::out_of_range("Vector length field out of bounds");
    }
    uint32_t len = *reinterpret_cast<const uint32_t*>(data.data() + offset);
    len = ntohl(len);
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

// ========== 核心协议序列化/反序列化（纯C++） ==========
inline std::vector<char> serializeLoginReq(const LoginReq& req) {
    std::vector<char> data;
    auto nicknameData = serializeString(req.nickname);
    data.insert(data.end(), nicknameData.begin(), nicknameData.end());
    auto avatarData = serializeVector(req.avatar);
    data.insert(data.end(), avatarData.begin(), avatarData.end());
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
        req.dataPort = ntohs(port);
        offset += sizeof(uint16_t);
    } catch (const std::exception& e) {
        std::cerr << "Deserialize LoginReq failed: " << e.what() << std::endl;
        req = LoginReq{};
    }
    return req;
}

inline std::vector<char> serializeLoginRsp(const LoginRsp& rsp) {
    std::vector<char> data;
    data.insert(data.end(), reinterpret_cast<const char*>(&rsp.success),
                reinterpret_cast<const char*>(&rsp.success) + sizeof(bool));
    data.insert(data.end(), reinterpret_cast<const char*>(&rsp.userId),
                reinterpret_cast<const char*>(&rsp.userId) + sizeof(int));
    auto msgData = serializeString(rsp.msg);
    data.insert(data.end(), msgData.begin(), msgData.end());
    auto nicknameData = serializeString(rsp.nickname);
    data.insert(data.end(), nicknameData.begin(), nicknameData.end());
    return data;
}

inline LoginRsp deserializeLoginRsp(const std::vector<char>& data) {
    LoginRsp rsp;
    size_t offset = 0;
    try {
        if (offset + sizeof(bool) > data.size()) {
            throw std::out_of_range("Success field out of bounds");
        }
        rsp.success = *reinterpret_cast<const bool*>(data.data() + offset);
        offset += sizeof(bool);
        
        if (offset + sizeof(int) > data.size()) {
            throw std::out_of_range("UserID field out of bounds");
        }
        rsp.userId = *reinterpret_cast<const int*>(data.data() + offset);
        offset += sizeof(int);
        
        rsp.msg = deserializeString(data, offset);
        rsp.nickname = deserializeString(data, offset);
    } catch (const std::exception& e) {
        std::cerr << "Deserialize LoginRsp failed: " << e.what() << std::endl;
        rsp = LoginRsp{};
    }
    return rsp;
}

inline std::vector<char> serializeUserInfo(const UserInfo& user) {
    std::vector<char> data;
    data.insert(data.end(), reinterpret_cast<const char*>(&user.userId),
                reinterpret_cast<const char*>(&user.userId) + sizeof(int));
    auto nicknameData = serializeString(user.nickname);
    data.insert(data.end(), nicknameData.begin(), nicknameData.end());
    auto avatarData = serializeVector(user.avatar);
    data.insert(data.end(), avatarData.begin(), avatarData.end());
    uint16_t port = htons(user.dataPort);
    data.insert(data.end(), reinterpret_cast<const char*>(&port),
                reinterpret_cast<const char*>(&port) + sizeof(uint16_t));
    auto ipData = serializeString(user.ip);
    data.insert(data.end(), ipData.begin(), ipData.end());
    data.insert(data.end(), reinterpret_cast<const char*>(&user.managePort),
                reinterpret_cast<const char*>(&user.managePort) + sizeof(int));
    return data;
}

inline UserInfo deserializeUserInfo(const std::vector<char>& data, size_t& offset) {
    UserInfo user;
    if (offset + sizeof(int) > data.size()) {
        throw std::out_of_range("UserID field out of bounds");
    }
    user.userId = *reinterpret_cast<const int*>(data.data() + offset);
    offset += sizeof(int);
    
    user.nickname = deserializeString(data, offset);
    user.avatar = deserializeVector(data, offset);
    
    if (offset + sizeof(uint16_t) > data.size()) {
        throw std::out_of_range("DataPort field out of bounds");
    }
    uint16_t port = *reinterpret_cast<const uint16_t*>(data.data() + offset);
    user.dataPort = ntohs(port);
    offset += sizeof(uint16_t);
    
    user.ip = deserializeString(data, offset);
    
    if (offset + sizeof(int) > data.size()) {
        throw std::out_of_range("ManagePort field out of bounds");
    }
    user.managePort = *reinterpret_cast<const int*>(data.data() + offset);
    offset += sizeof(int);
    return user;
}

// 重载：无offset的UserInfo反序列化
inline UserInfo deserializeUserInfo(const std::vector<char>& data) {
    size_t offset = 0;
    return deserializeUserInfo(data, offset);
}

inline std::map<int, UserInfo> deserializeUserList(const std::vector<char>& data) {
    std::map<int, UserInfo> userList;
    size_t offset = 0;
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

inline std::vector<char> serializeCommonMsg(const CommonMsg& msg) {
    std::vector<char> data;
    data.insert(data.end(), reinterpret_cast<const char*>(&msg.fromUserId),
                reinterpret_cast<const char*>(&msg.fromUserId) + sizeof(int));
    auto nicknameData = serializeString(msg.fromNickname);
    data.insert(data.end(), nicknameData.begin(), nicknameData.end());
    auto contentData = serializeString(msg.content);
    data.insert(data.end(), contentData.begin(), contentData.end());
    data.insert(data.end(), reinterpret_cast<const char*>(&msg.toUserId),
                reinterpret_cast<const char*>(&msg.toUserId) + sizeof(int));
    return data;
}

inline CommonMsg deserializeCommonMsg(const std::vector<char>& data) {
    CommonMsg msg;
    size_t offset = 0;
    try {
        if (offset + sizeof(int) > data.size()) {
            throw std::out_of_range("fromUserId field out of bounds");
        }
        msg.fromUserId = *reinterpret_cast<const int*>(data.data() + offset);
        offset += sizeof(int);
        
        msg.fromNickname = deserializeString(data, offset);
        msg.content = deserializeString(data, offset);
        
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

// ========== 服务端纯C++ sendPacket ==========
inline bool sendPacket(int fd, uint32_t msgType, const std::vector<char>& data) {
    PacketHeader header;
    header.msgType = msgType;
    header.dataLen = static_cast<uint32_t>(data.size());
    
    std::vector<char> sendData;
    sendData.insert(sendData.end(), reinterpret_cast<const char*>(&header),
                    reinterpret_cast<const char*>(&header) + sizeof(PacketHeader));
    sendData.insert(sendData.end(), data.begin(), data.end());
    
    ssize_t sent = send(fd, sendData.data(), sendData.size(), 0);
    if (sent == -1) {
        std::cerr << "发送数据包失败：" << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

#endif // PROTOCOL_BASE_H
