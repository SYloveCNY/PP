#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <chrono>  // 新增：支持时间戳（心跳检测）

// 消息类型枚举
enum class MsgType {
    LOGIN_REQ = 1,
    LOGIN_RSP = 2,
    USER_LIST_REQ = 3,
    USER_LIST_RSP = 4,
    COMMON_MSG = 5,
    IMAGE_MSG = 6,
    FILE_REQ = 7,
    FILE_DATA = 8,
    FILE_END = 9,
    HEARTBEAT = 10,
    USER_ONLINE_NOTIFY = 11,
    USER_OFFLINE_NOTIFY = 12
};

// 数据包头部
struct PacketHeader {
    MsgType msgType;
    uint32_t dataLen;
};

// 登录请求
struct LoginReq {
    std::string nickname;
    std::vector<char> avatar;
    uint16_t dataPort;
};

// 登录响应
struct LoginRsp {
    bool success;
    int userId;
    std::string msg;
    std::string nickname;
};

// 用户信息（补充服务端需要的字段）
struct UserInfo {
    int userId;
    std::string nickname;
    std::vector<char> avatar;
    std::string ip;
    uint16_t dataPort;          // 客户端点对点端口（原managePort是笔误，统一为dataPort）
    int managePort;             // 新增：服务端管理的客户端连接FD（原缺失）
    std::chrono::system_clock::time_point lastHeartbeat; // 新增：心跳时间戳
};

// 普通消息
struct CommonMsg {
    int fromUserId;
    int toUserId;  // 统一为toUserId，杜绝to_user_id混用
    std::string fromNickname;
    std::string content;
};

// 图片消息
struct ImageMsg {
    int fromUserId;
    int toUserId;
    std::string fromNickname;
    std::string imgName;
    std::vector<char> imgData;
};

// 文件请求
struct FileReq {
    int fromUserId;
    int toUserId;
    std::string fromNickname;
    std::string fileName;
    uint64_t fileSize;
};

// ========== 序列化函数（补充服务端需要的serializeLoginRsp） ==========
inline std::vector<char> serializeString(const std::string& str) {
    std::vector<char> data;
    uint32_t len = str.size();
    data.insert(data.end(), (char*)&len, (char*)&len + sizeof(uint32_t));
    data.insert(data.end(), str.begin(), str.end());
    return data;
}

inline std::string deserializeString(const std::vector<char>& data, size_t& offset) {
    uint32_t len = *(uint32_t*)(data.data() + offset);
    offset += sizeof(uint32_t);
    std::string str(data.begin() + offset, data.begin() + offset + len);
    offset += len;
    return str;
}

inline std::vector<char> serializeVector(const std::vector<char>& vec) {
    std::vector<char> data;
    uint32_t len = vec.size();
    data.insert(data.end(), (char*)&len, (char*)&len + sizeof(uint32_t));
    data.insert(data.end(), vec.begin(), vec.end());
    return data;
}

inline std::vector<char> deserializeVector(const std::vector<char>& data, size_t& offset) {
    uint32_t len = *(uint32_t*)(data.data() + offset);
    offset += sizeof(uint32_t);
    std::vector<char> vec(data.begin() + offset, data.begin() + offset + len);
    offset += len;
    return vec;
}

// 新增：登录响应序列化（服务端需要）
inline std::vector<char> serializeLoginRsp(const LoginRsp& rsp) {
    std::vector<char> data;
    data.insert(data.end(), (char*)&rsp.success, sizeof(bool));
    data.insert(data.end(), (char*)&rsp.userId, sizeof(int));
    auto msgData = serializeString(rsp.msg);
    data.insert(data.end(), msgData.begin(), msgData.end());
    auto nicknameData = serializeString(rsp.nickname);
    data.insert(data.end(), nicknameData.begin(), nicknameData.end());
    return data;
}

inline LoginRsp deserializeLoginRsp(const std::vector<char>& data) {
    LoginRsp rsp;
    size_t offset = 0;
    rsp.success = *(bool*)(data.data() + offset);
    offset += sizeof(bool);
    rsp.userId = *(int*)(data.data() + offset);
    offset += sizeof(int);
    rsp.msg = deserializeString(data, offset);
    rsp.nickname = deserializeString(data, offset);
    return rsp;
}

inline std::map<int, UserInfo> deserializeUserList(const std::vector<char>& data) {
    std::map<int, UserInfo> users;
    size_t offset = 0;
    size_t userCount = *(size_t*)(data.data() + offset);
    offset += sizeof(size_t);

    for (size_t i = 0; i < userCount; ++i) {
        UserInfo user;
        user.nickname = deserializeString(data, offset);
        user.avatar = deserializeVector(data, offset);
        user.ip = deserializeString(data, offset);
        user.userId = *(int*)(data.data() + offset);
        offset += sizeof(int);
        user.dataPort = *(uint16_t*)(data.data() + offset);
        offset += sizeof(uint16_t);
        users[user.userId] = user;
    }
    return users;
}

inline CommonMsg deserializeCommonMsg(const std::vector<char>& data) {
    CommonMsg msg;
    size_t offset = 0;
    msg.fromUserId = *(int*)(data.data() + offset);
    offset += sizeof(int);
    msg.toUserId = *(int*)(data.data() + offset);
    offset += sizeof(int);
    msg.fromNickname = deserializeString(data, offset);
    msg.content = deserializeString(data, offset);
    return msg;
}

inline UserInfo deserializeUserInfo(const std::vector<char>& data) {
    UserInfo user;
    size_t offset = 0;
    user.nickname = deserializeString(data, offset);
    user.avatar = deserializeVector(data, offset);
    user.ip = deserializeString(data, offset);
    user.userId = *(int*)(data.data() + offset);
    offset += sizeof(int);
    user.dataPort = *(uint16_t*)(data.data() + offset);
    return user;
}

#endif // PROTOCOL_H