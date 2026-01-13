#ifndef PROTOCOL_H
#define PROTOCOL_H

// 系统头文件（网络字节序转换+跨平台兼容）
#if defined(__linux__) || defined(__APPLE__)
#include <arpa/inet.h>
#elif defined(_WIN32) || defined(_WIN64)
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include "./json/json.hpp"

// ========== 补全PacketHeader结构体（添加msgId和senderId） ==========
struct PacketHeader {
    uint32_t msgType;    // 消息类型（对应MsgType枚举）
    uint32_t dataLen;    // 数据部分长度
    uint32_t msgId;      // 消息ID（缺失的成员）
    uint32_t senderId;   // 发送者ID（缺失的成员）
};

// 消息类型枚举（包含所有用到的类型，新增私聊类型）
enum class MsgType : uint32_t {
    LOGIN_REQ = 1,          
    LOGIN_RSP = 2,          
    USER_LIST_REQ = 3,      
    USER_LIST_RSP = 4,      
    COMMON_MSG = 5,         
    USER_STATUS_NOTIFY = 6, 
    HEARTBEAT = 7,          
    USER_ONLINE_NOTIFY = 8, 
    USER_OFFLINE_NOTIFY = 9,
    IMAGE_MSG = 11,         
    FILE_MSG = 12,          
    PRIVATE_MSG = 13,       // 新增：私聊消息
    PRIVATE_MSG_RSP = 14,    // 新增：私聊响应
    LOGOUT_REQ = 15,               // 新增：主动下线请求
    HEARTBEAT_RESP = 16,           // 新增：心跳响应
    STATUS_BROADCAST = 17         // 新增：状态广播（上线/下线通知）
};

// 用户信息结构体（包含ip和dataPort）
struct UserInfo {
    int userId;             
    std::string nickname;   
    bool isOnline;          
    std::string ip;         
    int dataPort = 0;       
};

// 登录请求/响应
struct LoginReq {
    std::string nickname;   
};

struct LoginRsp {
    bool success;           
    std::string msg;        
    int userId;             
};

// 用户列表响应
struct UserListRsp {
    std::vector<UserInfo> users; 
};

// 用户状态通知
struct UserStatusNotify {
    int userId;             
    std::string nickname;   
    bool isOnline;          
};

// ========== 新增：私聊消息结构体 ==========
struct PrivateMsg {
    int senderId;       // 发送方ID
    int receiverId;     // 接收方ID
    std::string content;// 消息内容
};

// ========== 新增：私聊响应结构体 ==========
struct PrivateMsgRsp {
    bool success;       // 是否成功
    std::string msg;    // 提示信息（如“用户不在线”）
    int receiverId;     // 接收方ID
};

// 图片消息（包含fileName/fileSize/base64Data）
struct ImageMsg {
    int senderId;           
    int receiverId;         
    std::string imagePath;  
    std::string imageData;  
    std::string fileName;   
    std::string fileSize;   
    std::string base64Data; 
};

// 文件消息结构体
struct FileMsg {
    int receiverId;          // 接收者ID，0表示群发
    std::string fileName;    // 文件名
    std::string fileSize;    // 文件大小
    std::string fileHash;    // 文件哈希值
    bool isComplete;         // 是否为完整文件
    std::string base64Data;  // Base64编码的文件数据片段
};

// JSON序列化特化（所有结构体，新增私聊结构体的特化）
namespace nlohmann {
template <> struct adl_serializer<UserInfo> {
    static void to_json(json& j, const UserInfo& u) {
        j = json{{"userId", u.userId}, {"nickname", u.nickname}, {"isOnline", u.isOnline},
                 {"ip", u.ip}, {"dataPort", u.dataPort}};
    }
    static void from_json(const json& j, UserInfo& u) {
        j.at("userId").get_to(u.userId);
        j.at("nickname").get_to(u.nickname);
        j.at("isOnline").get_to(u.isOnline);
        u.ip = j.contains("ip") ? j["ip"].get<std::string>() : "";
        u.dataPort = j.contains("dataPort") ? j["dataPort"].get<int>() : 0;
    }
};

template <> struct adl_serializer<LoginReq> {
    static void to_json(json& j, const LoginReq& req) { j = json{{"nickname", req.nickname}}; }
    static void from_json(const json& j, LoginReq& req) { j.at("nickname").get_to(req.nickname); }
};

template <> struct adl_serializer<LoginRsp> {
    static void to_json(json& j, const LoginRsp& rsp) {
        j = json{{"success", rsp.success}, {"msg", rsp.msg}, {"userId", rsp.userId}};
    }
    static void from_json(const json& j, LoginRsp& rsp) {
        j.at("success").get_to(rsp.success);
        j.at("msg").get_to(rsp.msg);
        j.at("userId").get_to(rsp.userId);
    }
};

template <> struct adl_serializer<UserListRsp> {
    static void to_json(json& j, const UserListRsp& rsp) { j = json{{"users", rsp.users}}; }
    static void from_json(const json& j, UserListRsp& rsp) { j.at("users").get_to(rsp.users); }
};

template <> struct adl_serializer<UserStatusNotify> {
    static void to_json(json& j, const UserStatusNotify& notify) {
        j = json{{"userId", notify.userId}, {"nickname", notify.nickname}, {"isOnline", notify.isOnline}};
    }
    static void from_json(const json& j, UserStatusNotify& notify) {
        j.at("userId").get_to(notify.userId);
        j.at("nickname").get_to(notify.nickname);
        j.at("isOnline").get_to(notify.isOnline);
    }
};

// ========== 新增：PrivateMsg序列化特化 ==========
template <> struct adl_serializer<PrivateMsg> {
    static void to_json(json& j, const PrivateMsg& msg) {
        j = json{{"senderId", msg.senderId}, {"receiverId", msg.receiverId}, {"content", msg.content}};
    }
    static void from_json(const json& j, PrivateMsg& msg) {
        j.at("senderId").get_to(msg.senderId);
        j.at("receiverId").get_to(msg.receiverId);
        j.at("content").get_to(msg.content);
    }
};

// ========== 新增：PrivateMsgRsp序列化特化 ==========
template <> struct adl_serializer<PrivateMsgRsp> {
    static void to_json(json& j, const PrivateMsgRsp& rsp) {
        j = json{{"success", rsp.success}, {"msg", rsp.msg}, {"receiverId", rsp.receiverId}};
    }
    static void from_json(const json& j, PrivateMsgRsp& rsp) {
        j.at("success").get_to(rsp.success);
        j.at("msg").get_to(rsp.msg);
        j.at("receiverId").get_to(rsp.receiverId);
    }
};

template <> struct adl_serializer<ImageMsg> {
    static void to_json(json& j, const ImageMsg& msg) {
        j = json{{"senderId", msg.senderId}, {"receiverId", msg.receiverId}, {"imagePath", msg.imagePath},
                 {"imageData", msg.imageData}, {"fileName", msg.fileName}, {"fileSize", msg.fileSize},
                 {"base64Data", msg.base64Data}};
    }
    static void from_json(const json& j, ImageMsg& msg) {
        j.at("senderId").get_to(msg.senderId);
        j.at("receiverId").get_to(msg.receiverId);
        j.at("imagePath").get_to(msg.imagePath);
        j.at("imageData").get_to(msg.imageData);
        msg.fileName = j.contains("fileName") ? j["fileName"].get<std::string>() : "";
        msg.fileSize = j.contains("fileSize") ? j["fileSize"].get<std::string>() : "";
        msg.base64Data = j.contains("base64Data") ? j["base64Data"].get<std::string>() : "";
    }
};
} // namespace nlohmann

// 序列化/反序列化工具函数
template <typename T>
std::string serialize(const T& obj) {
    nlohmann::json j = obj;
    return j.dump();
}

template <typename T>
T deserialize(const std::string& jsonStr) {
    nlohmann::json j = nlohmann::json::parse(jsonStr);
    return j.get<T>();
}

// 网络字节序转换函数（包含msgId和senderId）
inline PacketHeader htonHeader(const PacketHeader& header) {
    PacketHeader netHeader = header;
    netHeader.msgType = htonl(header.msgType);
    netHeader.dataLen = htonl(header.dataLen);
    netHeader.msgId = htonl(header.msgId);    // 新增：转换msgId
    netHeader.senderId = htonl(header.senderId); // 新增：转换senderId
    return netHeader;
}

inline PacketHeader ntohHeader(const PacketHeader& netHeader) {
    PacketHeader hostHeader = netHeader;
    hostHeader.msgType = ntohl(netHeader.msgType);
    hostHeader.dataLen = ntohl(netHeader.dataLen);
    hostHeader.msgId = ntohl(netHeader.msgId);    // 新增：转换msgId
    hostHeader.senderId = ntohl(netHeader.senderId); // 新增：转换senderId
    return hostHeader;
}

// Qt客户端sendPacket函数声明
#if defined(QT_CORE_LIB)
#include <QTcpSocket>
bool sendPacket(QTcpSocket* socket, uint32_t msgType, const std::string& data, uint32_t msgId = 0, uint32_t senderId = 0);
bool sendPacket(QTcpSocket* socket, uint32_t msgType, const QByteArray& data);
#endif

#endif // PROTOCOL_H