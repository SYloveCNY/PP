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
    std::chrono::system_clock::time_point lastHeartbeatTime; 
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
// 序列化字符串：先存长度（uint32_t，网络字节序），再存内容
inline std::vector<char> serializeString(const std::string& str) {
    std::vector<char> data;
    uint32_t len = static_cast<uint32_t>(str.size());
    uint32_t networkLen = htonl(len); // 长度转换为网络字节序
    // 写入长度
    data.insert(data.end(), reinterpret_cast<char*>(&networkLen),
                reinterpret_cast<char*>(&networkLen) + sizeof(uint32_t));
    // 写入字符串内容
    if (len > 0) {
        data.insert(data.end(), str.begin(), str.end());
    }
    return data;
}

// 反序列化字符串：先读长度（网络字节序→主机字节序），再读内容
inline std::string deserializeString(const std::vector<char>& data, size_t& offset) {
    if (offset + sizeof(uint32_t) > data.size()) {
        throw std::out_of_range("string length field out of bounds");
    }
    // 读取长度并转换为主机字节序
    uint32_t networkLen = *reinterpret_cast<const uint32_t*>(data.data() + offset);
    uint32_t len = ntohl(networkLen);
    offset += sizeof(uint32_t);

    // 读取字符串内容
    if (offset + len > data.size()) {
        throw std::out_of_range("string content field out of bounds");
    }
    std::string str(data.begin() + offset, data.begin() + offset + len);
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
    // 序列化userId（修正insert参数）
    data.insert(data.end(), 
               reinterpret_cast<const char*>(&user.userId), 
               reinterpret_cast<const char*>(&user.userId) + sizeof(int));
    // 序列化昵称
    auto nicknameData = serializeString(user.nickname);
    data.insert(data.end(), nicknameData.begin(), nicknameData.end());
    // 序列化头像
    auto avatarData = serializeVector(user.avatar);
    data.insert(data.end(), avatarData.begin(), avatarData.end());
    // 序列化dataPort（转网络字节序，修正insert参数）
    uint16_t port = htons(user.dataPort);
    data.insert(data.end(), 
               reinterpret_cast<const char*>(&port), 
               reinterpret_cast<const char*>(&port) + sizeof(uint16_t));
    // 序列化IP
    auto ipData = serializeString(user.ip);
    data.insert(data.end(), ipData.begin(), ipData.end());
    // 序列化managePort（修正insert参数）
    data.insert(data.end(), 
               reinterpret_cast<const char*>(&user.managePort), 
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

inline std::vector<char> serializeUserList(const std::map<int, UserInfo>& users);

// 重载：无offset的UserInfo反序列化
inline UserInfo deserializeUserInfo(const std::vector<char>& data) {
    size_t offset = 0;
    return deserializeUserInfo(data, offset);
}

inline std::map<int, UserInfo> deserializeUserList(const std::vector<char>& data) {
    std::map<int, UserInfo> users;
    if (data.size() < sizeof(uint32_t)) {
        throw std::runtime_error("用户列表数据过短");
    }

    // 关键修复：网络字节序→主机字节序（必须调用ntohl）
    uint32_t networkCount;
    memcpy(&networkCount, data.data(), sizeof(uint32_t));
    uint32_t userCount = ntohl(networkCount); // 漏了这行会导致解析失败！

    size_t offset = sizeof(uint32_t);
    for (uint32_t i = 0; i < userCount; ++i) {
        if (offset + sizeof(UserInfo) > data.size()) { // 或根据实际序列化逻辑判断
            throw std::runtime_error("用户数据不完整");
        }
        // 解析单个用户信息（复用已有的deserializeUserInfo）
        UserInfo user = deserializeUserInfo(data, offset);
        users[user.userId] = user;
    }
    return users;
}

inline std::vector<char> serializeCommonMsg(const CommonMsg& msg) {
    std::vector<char> data;
    // 1. 序列化 fromUserId（int→网络字节序）
    int32_t networkFromUserId = htonl(msg.fromUserId); // 主机字节序→网络字节序
    data.insert(data.end(), reinterpret_cast<char*>(&networkFromUserId),
                reinterpret_cast<char*>(&networkFromUserId) + sizeof(int32_t));

    // 2. 序列化 fromNickname（复用上面的字符串序列化）
    auto nicknameData = serializeString(msg.fromNickname);
    data.insert(data.end(), nicknameData.begin(), nicknameData.end());

    // 3. 序列化 content（复用字符串序列化）
    auto contentData = serializeString(msg.content);
    data.insert(data.end(), contentData.begin(), contentData.end());

    // 4. 序列化 toUserId（int→网络字节序）
    int32_t networkToUserId = htonl(msg.toUserId);
    data.insert(data.end(), reinterpret_cast<char*>(&networkToUserId),
                reinterpret_cast<char*>(&networkToUserId) + sizeof(int32_t));

    std::cout << "[序列化CommonMsg] 长度：" << data.size() 
              << "，fromUserId（网络字节序）：" << networkFromUserId
              << "，toUserId（网络字节序）：" << networkToUserId << std::endl;
    return data;
}

inline CommonMsg deserializeCommonMsg(const std::vector<char>& data) {
    CommonMsg msg;
    size_t offset = 0;
    try {
        // 1. 反序列化 fromUserId（网络字节序→主机字节序）
        if (offset + sizeof(int32_t) > data.size()) {
            throw std::out_of_range("fromUserId field out of bounds（需要" + std::to_string(sizeof(int32_t)) + "字节，剩余" + std::to_string(data.size() - offset) + "字节）");
        }
        int32_t networkFromUserId = 0;
        memcpy(&networkFromUserId, data.data() + offset, sizeof(int32_t));
        msg.fromUserId = ntohl(networkFromUserId);
        offset += sizeof(int32_t);
        // 纯C++日志（服务端识别）
        std::cout << "[反序列化CommonMsg] fromUserId：" << msg.fromUserId << std::endl;

        // 2. 反序列化 fromNickname
        msg.fromNickname = deserializeString(data, offset);
        std::cout << "[反序列化CommonMsg] fromNickname：" << msg.fromNickname << std::endl;

        // 3. 反序列化 content
        msg.content = deserializeString(data, offset);
        std::cout << "[反序列化CommonMsg] content：" << msg.content << std::endl;

        // 4. 反序列化 toUserId（网络字节序→主机字节序）
        if (offset + sizeof(int32_t) > data.size()) {
            throw std::out_of_range("toUserId field out of bounds（需要" + std::to_string(sizeof(int32_t)) + "字节，剩余" + std::to_string(data.size() - offset) + "字节）");
        }
        int32_t networkToUserId = 0;
        memcpy(&networkToUserId, data.data() + offset, sizeof(int32_t));
        msg.toUserId = ntohl(networkToUserId);
        offset += sizeof(int32_t);
        std::cout << "[反序列化CommonMsg] toUserId：" << msg.toUserId << std::endl;

        std::cout << "[反序列化CommonMsg成功] 总偏移量：" << offset << std::endl;
    } catch (const std::exception& e) {
        std::string err = "Deserialize CommonMsg failed: " + std::string(e.what());
        std::cerr << err << std::endl;
        // 移除 Qt 日志（服务端不支持），仅保留纯C++错误输出
        msg = CommonMsg{};
    }
    return msg;
}

// ========== 服务端纯C++ sendPacket ==========

inline bool sendPacket(int targetFd, uint32_t msgType, const std::vector<char>& payload) {
    try {
        // 1. 构造 PacketHeader（确保字段正确）
        PacketHeader header;
        header.msgType = msgType; // 接收 uint32_t，MsgType 枚举可隐式转换
        header.dataLen = static_cast<uint32_t>(payload.size());

        // 2. 构建完整发送缓冲区（包头 +  payload）
        std::vector<char> sendData(sizeof(PacketHeader) + payload.size());
        // 复制包头（memcpy 避免结构体对齐问题）
        memcpy(sendData.data(), &header, sizeof(PacketHeader));
        // 复制 payload（若有数据）
        if (!payload.empty()) {
            memcpy(sendData.data() + sizeof(PacketHeader), payload.data(), payload.size());
        }

        // 3. 循环 send，确保所有数据发送完成（TCP 可能分批发送）
        ssize_t totalSent = 0;
        ssize_t dataLen = sendData.size();
        while (totalSent < dataLen) {
            ssize_t sent = send(targetFd, sendData.data() + totalSent, dataLen - totalSent, 0);
            if (sent == -1) {
                std::cerr << "[sendPacket] 发送失败：fd=" << targetFd << "，错误：" << strerror(errno) << std::endl;
                return false;
            }
            totalSent += sent;
        }

        // 4. 调试日志（可选，方便排查）
        // std::cout << "[sendPacket] 成功：fd=" << targetFd << "，消息类型：" << msgType << "，总字节数：" << dataLen << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[sendPacket] 异常：" << e.what() << std::endl;
        return false;
    }
}

#endif // PROTOCOL_BASE_H
