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
#include "UserInfo.h"

// ========== 基础类型定义（无QT） ==========
enum class MsgType : uint32_t {
    LOGIN_REQ = 1,        // 登录请求
    LOGIN_RSP = 2,        // 登录响应
    USER_LIST_REQ = 3,    // 用户列表请求
    USER_LIST_RSP = 4,    // 用户列表响应（必须添加！）
    COMMON_MSG = 5,       // 普通消息
    USER_ONLINE_NOTIFY = 6,// 用户上线通知
    USER_OFFLINE_NOTIFY =7,// 用户下线通知
    HEARTBEAT = 10        // 心跳包
};

// 数据包头部（纯C++结构体）
struct PacketHeader {
    uint32_t msgType;
    uint32_t dataLen;
};

// 核心结构体（纯C++）
struct LoginReq {
    std::string nickname;
    std::string avatar;
    uint16_t dataPort;
};

struct LoginRsp {
    bool success;
    int userId;
    std::string msg;
    std::string nickname;
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

// 序列化 std::string：4字节长度（网络字节序）+ 字符串内容
inline std::vector<char> serializeString(const std::string& str) {
    std::vector<char> data;
    uint32_t len = htonl(static_cast<uint32_t>(str.size()));
    
    // 修复：用「起始指针+结束指针」构成迭代器范围
    data.insert(data.end(), reinterpret_cast<char*>(&len), 
                reinterpret_cast<char*>(&len) + sizeof(uint32_t));
    // 字符串内容：str.data()（起始） + str.data()+str.size()（结束）
    data.insert(data.end(), str.data(), str.data() + str.size());
    
    return data;
}

// 反序列化 std::string：先读4字节长度，再读对应长度的内容
inline std::string deserializeString(const std::vector<char>& data, size_t& offset) {
    if (offset + sizeof(uint32_t) > data.size()) {
        throw std::out_of_range("string length overflow");
    }
    // 读取长度并转换为主机字节序
    uint32_t len = ntohl(*reinterpret_cast<const uint32_t*>(data.data() + offset));
    offset += sizeof(uint32_t);

    if (offset + len > data.size()) {
        throw std::out_of_range("string content overflow");
    }
    // 读取字符串内容（data.data()+offset 是起始指针，长度 len）
    std::string str(data.data() + offset, len);
    offset += len;
    return str;
}

// 序列化 std::vector<char>：4字节长度（网络字节序）+ 向量内容
inline std::vector<char> serializeVector(const std::vector<char>& vec) {
    std::vector<char> data;
    uint32_t len = htonl(static_cast<uint32_t>(vec.size()));
    
    // 修复：用「起始指针+结束指针」构成迭代器范围
    data.insert(data.end(), reinterpret_cast<char*>(&len), 
                reinterpret_cast<char*>(&len) + sizeof(uint32_t));
    // 向量内容：vec.data()（起始） + vec.data()+vec.size()（结束）
    data.insert(data.end(), vec.data(), vec.data() + vec.size());
    
    return data;
}

// 反序列化 std::vector<char>
inline std::vector<char> deserializeVector(const std::vector<char>& data, size_t& offset) {
    if (offset + sizeof(uint32_t) > data.size()) {
        throw std::out_of_range("vector length overflow");
    }
    uint32_t len = ntohl(*reinterpret_cast<const uint32_t*>(data.data() + offset));
    offset += sizeof(uint32_t);

    if (offset + len > data.size()) {
        throw std::out_of_range("vector content overflow");
    }
    std::vector<char> vec(data.begin() + offset, data.begin() + offset + len);
    offset += len;
    return vec;
}

// ========== 核心协议序列化/反序列化（纯C++） ==========
inline std::vector<char> serializeLoginReq(const LoginReq& req) {
    std::vector<char> data;
    auto nicknameData = serializeString(req.nickname);
    auto avatarData = serializeString(req.avatar);  // 修复：用 serializeString（string类型）
    uint16_t portNet = htons(req.dataPort);

    data.insert(data.end(), nicknameData.begin(), nicknameData.end());
    data.insert(data.end(), avatarData.begin(), avatarData.end());
    // 端口号也修复 insert 格式（如果之前没修）
    data.insert(data.end(), reinterpret_cast<char*>(&portNet), 
                reinterpret_cast<char*>(&portNet) + sizeof(uint16_t));
    return data;
}

inline LoginReq deserializeLoginReq(const std::vector<char>& data) {
    LoginReq req;
    size_t offset = 0;
    req.nickname = deserializeString(data, offset);
    req.avatar = deserializeString(data, offset);  // 修复：用 deserializeString（string类型）
    req.dataPort = ntohs(*reinterpret_cast<const uint16_t*>(data.data() + offset));
    offset += sizeof(uint16_t);
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

// 序列化 UserInfo（含 avatar 字段，std::string 类型）
inline std::vector<char> serializeUserInfo(const UserInfo& user) {
    std::vector<char> data;

    // 1. 序列化 userId（4字节，网络字节序）
    int userIdNet = htonl(user.userId);
    data.insert(data.end(), reinterpret_cast<char*>(&userIdNet), 
                reinterpret_cast<char*>(&userIdNet) + sizeof(int));

    // 2. 序列化 nickname（长度4字节+内容，std::string 专用）
    uint32_t nickLen = htonl(static_cast<uint32_t>(user.nickname.size()));
    data.insert(data.end(), reinterpret_cast<char*>(&nickLen), 
                reinterpret_cast<char*>(&nickLen) + sizeof(uint32_t));
    data.insert(data.end(), user.nickname.begin(), user.nickname.end());

    // 3. 序列化 avatar（长度4字节+内容，std::string 专用，与 nickname 逻辑一致）
    uint32_t avatarLen = htonl(static_cast<uint32_t>(user.avatar.size()));
    data.insert(data.end(), reinterpret_cast<char*>(&avatarLen), 
                reinterpret_cast<char*>(&avatarLen) + sizeof(uint32_t));
    data.insert(data.end(), user.avatar.begin(), user.avatar.end());

    // 4. 序列化 ip（长度4字节+内容）
    uint32_t ipLen = htonl(static_cast<uint32_t>(user.ip.size()));
    data.insert(data.end(), reinterpret_cast<char*>(&ipLen), 
                reinterpret_cast<char*>(&ipLen) + sizeof(uint32_t));
    data.insert(data.end(), user.ip.begin(), user.ip.end());

    // 5. 序列化 dataPort（2字节，网络字节序）
    uint16_t portNet = htons(static_cast<uint16_t>(user.dataPort));
    data.insert(data.end(), reinterpret_cast<char*>(&portNet), 
                reinterpret_cast<char*>(&portNet) + sizeof(uint16_t));

    return data;
}

// 反序列化 UserInfo（含 avatar 字段，std::string 类型）
inline UserInfo deserializeUserInfo(const std::vector<char>& data, size_t& offset) {
    UserInfo user;
    try {
        // 1. 解析 userId
        if (offset + sizeof(int) > data.size()) throw std::out_of_range("userId out of bounds");
        user.userId = ntohl(*reinterpret_cast<const int*>(data.data() + offset));
        offset += sizeof(int);

        // 2. 解析 nickname（std::string 专用）
        if (offset + sizeof(uint32_t) > data.size()) throw std::out_of_range("nickname len out of bounds");
        uint32_t nickLen = ntohl(*reinterpret_cast<const uint32_t*>(data.data() + offset));
        offset += sizeof(uint32_t);
        if (offset + nickLen > data.size()) throw std::out_of_range("nickname content out of bounds");
        user.nickname = std::string(data.data() + offset, nickLen);
        offset += nickLen;

        // 3. 解析 avatar（std::string 专用，与 nickname 逻辑一致）
        if (offset + sizeof(uint32_t) > data.size()) throw std::out_of_range("avatar len out of bounds");
        uint32_t avatarLen = ntohl(*reinterpret_cast<const uint32_t*>(data.data() + offset));
        offset += sizeof(uint32_t);
        if (offset + avatarLen > data.size()) throw std::out_of_range("avatar content out of bounds");
        user.avatar = std::string(data.data() + offset, avatarLen);  // 直接转换为 std::string
        offset += avatarLen;

        // 4. 解析 ip
        if (offset + sizeof(uint32_t) > data.size()) throw std::out_of_range("ip len out of bounds");
        uint32_t ipLen = ntohl(*reinterpret_cast<const uint32_t*>(data.data() + offset));
        offset += sizeof(uint32_t);
        if (offset + ipLen > data.size()) throw std::out_of_range("ip content out of bounds");
        user.ip = std::string(data.data() + offset, ipLen);
        offset += ipLen;

        // 5. 解析 dataPort
        if (offset + sizeof(uint16_t) > data.size()) throw std::out_of_range("dataPort out of bounds");
        user.dataPort = ntohs(*reinterpret_cast<const uint16_t*>(data.data() + offset));
        offset += sizeof(uint16_t);

        // 服务端专用字段初始化
        user.managePort = 0;
        user.lastHeartbeatTime = 0;
    } catch (const std::exception& e) {
        std::cerr << "deserializeUserInfo failed: " << e.what() << std::endl;
        throw;
    }
    return user;
}

// 序列化用户列表
inline std::vector<char> serializeUserList(const std::map<int, UserInfo>& onlineUsers) {
    std::vector<char> data;
    // 1. 序列化在线用户数量（转网络字节序）
    uint32_t userCount = htonl(static_cast<uint32_t>(onlineUsers.size()));
    data.insert(data.end(), reinterpret_cast<char*>(&userCount), 
                reinterpret_cast<char*>(&userCount) + sizeof(uint32_t));
    
    // 2. 逐个序列化每个用户的信息（依赖 UserInfo 序列化函数）
    for (const auto& [userId, user] : onlineUsers) {
        // 序列化 UserInfo（如果没有该函数，先添加下面的 serializeUserInfo）
        std::vector<char> userData = serializeUserInfo(user);
        data.insert(data.end(), userData.begin(), userData.end());
    }
    return data;
}

// 反序列化用户列表
inline std::map<int, UserInfo> deserializeUserListToMap(const std::vector<char>& data) {
    std::map<int, UserInfo> userMap;
    size_t offset = 0;
    try {
        uint32_t userCount = ntohl(*reinterpret_cast<const uint32_t*>(data.data() + offset));
        offset += sizeof(uint32_t);
        for (uint32_t i = 0; i < userCount; ++i) {
            UserInfo user = deserializeUserInfo(data, offset);
            userMap[user.userId] = user;
        }
    } catch (const std::exception& e) {
        std::cerr << "deserializeUserList failed: " << e.what() << std::endl;
        userMap.clear();
    }
    return userMap;
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

#endif // PROTOCOL_BASE_H
