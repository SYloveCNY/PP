#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <vector>
#include <string>
#include <chrono>

// 消息类型枚举
enum class MsgType {
    LOGIN_REQ = 1,       // 登录请求
    LOGIN_RSP = 2,       // 登录响应
    USER_ONLINE_NOTIFY = 3,  // 用户上线通知
    USER_OFFLINE_NOTIFY = 4, // 用户下线通知
    USER_LIST_REQ = 5,   // 在线用户列表请求
    USER_LIST_RSP = 6,   // 在线用户列表响应
    COMMON_MSG = 7,      // 普通文本消息
    IMAGE_MSG = 8,       // 图片消息
    FILE_REQ = 9,        // 文件传输请求
    FILE_DATA = 10,      // 文件分片数据
    FILE_END = 11,       // 文件传输结束
    HEARTBEAT = 12       // 心跳包
};

// 数据包头部
struct PacketHeader {
    MsgType msgType;    // 消息类型
    size_t dataLen;     // 数据长度
};

// 登录请求结构体
struct LoginReq {
    std::string nickname;   // 用户昵称
    std::vector<char> avatar; // 头像二进制数据
    uint16_t dataPort;     // 点对点数据传输端口
};

// 登录响应结构体
struct LoginRsp {
    bool success;       // 登录是否成功
    int userId;         // 分配的用户ID
    std::string msg;    // 响应提示信息
};

// 在线用户信息结构体
struct UserInfo {
    int userId;                 // 用户ID
    std::string nickname;       // 昵称
    std::vector<char> avatar;   // 头像数据
    std::string ip;             // 客户端IP地址
    uint16_t dataPort;          // 点对点数据传输端口
    int managePort;             // 服务端管理客户端的连接端口
    std::chrono::time_point<std::chrono::system_clock> lastHeartbeat; // 最后心跳时间
};

// 普通文本消息结构体
struct CommonMsg {
    int fromUserId;       // 发送者用户ID
    std::string fromNickname; // 发送者昵称
    std::string content;    // 消息内容
};

// 图片消息结构体
struct ImageMsg {
    int fromUserId;       // 发送者用户ID
    std::string fromNickname; // 发送者昵称
    std::string imgName;   // 图片文件名
    std::vector<char> imgData; // 图片二进制数据
};

// 文件传输请求结构体
struct FileReq {
    int fromUserId;       // 发送者用户ID
    std::string fromNickname; // 发送者昵称
    std::string fileName;  // 文件名
    uint64_t fileSize;     // 文件总大小
};

// 文件分片数据结构体
struct FileData {
    uint32_t seq;           // 分片序号
    std::vector<char> data; // 分片二进制数据
};

// 序列化字符串
inline std::vector<char> serializeString(const std::string& str) {
    std::vector<char> data;
    size_t len = str.size();
    data.resize(sizeof(size_t));
    memcpy(data.data(), &len, sizeof(size_t));
    data.insert(data.end(), str.begin(), str.end());
    return data;
}

// 反序列化字符串
inline std::string deserializeString(const std::vector<char>& data, size_t& offset) {
    if (offset + sizeof(size_t) > data.size()) return "";
    size_t len = 0;
    memcpy(&len, data.data() + offset, sizeof(size_t));
    offset += sizeof(size_t);
    if (offset + len > data.size()) return "";
    std::string str(data.data() + offset, len);
    offset += len;
    return str;
}

// 序列化vector<char>
inline std::vector<char> serializeVector(const std::vector<char>& vec) {
    std::vector<char> data;
    size_t len = vec.size();
    data.resize(sizeof(size_t));
    memcpy(data.data(), &len, sizeof(size_t));
    data.insert(data.end(), vec.begin(), vec.end());
    return data;
}

// 反序列化vector<char>
inline std::vector<char> deserializeVector(const std::vector<char>& data, size_t& offset) {
    if (offset + sizeof(size_t) > data.size()) return {};
    size_t len = 0;
    memcpy(&len, data.data() + offset, sizeof(size_t));
    offset += sizeof(size_t);
    if (offset + len > data.size()) return {};
    return std::vector<char>(data.data() + offset, data.data() + offset + len);
}

// 序列化LoginRsp
inline std::vector<char> serializeLoginRsp(const LoginRsp& rsp) {
    std::vector<char> data;
    data.push_back(rsp.success ? 1 : 0);
    data.resize(data.size() + sizeof(int));
    memcpy(data.data() + data.size() - sizeof(int), &rsp.userId, sizeof(int));
    auto msgData = serializeString(rsp.msg);
    data.insert(data.end(), msgData.begin(), msgData.end());
    return data;
}

// 反序列化LoginRsp
inline LoginRsp deserializeLoginRsp(const std::vector<char>& data) {
    LoginRsp rsp;
    size_t offset = 0;

    if (offset + 1 > data.size()) {
        rsp.success = false;
        rsp.msg = "反序列化失败：缺少success字段";
        return rsp;
    }
    rsp.success = (data[offset] == 1);
    offset += 1;

    if (offset + sizeof(int) > data.size()) {
        rsp.success = false;
        rsp.msg = "反序列化失败：缺少userId字段";
        return rsp;
    }
    memcpy(&rsp.userId, data.data() + offset, sizeof(int));
    offset += sizeof(int);

    rsp.msg = deserializeString(data, offset);
    return rsp;
}

#endif // PROTOCOL_H