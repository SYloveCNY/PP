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

// 传输协议类型（区分TCP/UDP）
enum class TransportProtocol {
    TCP = 0,
    UDP = 1
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
    PRIVATE_MSG = 10,       // 私聊消息
    PRIVATE_MSG_RSP = 11,   // 私聊响应
    LOGOUT_REQ = 12,        // 主动下线请求
    HEARTBEAT_RESP = 13,    // 心跳响应
    STATUS_BROADCAST = 14,  // 状态广播（上线/下线通知）
    IMAGE_MSG = 15,         // 图片消息（请求）
    IMAGE_MSG_RSP = 16,     // 图片消息响应
    FILE_MSG = 17,          // 文件消息（请求）
    FILE_MSG_RSP = 18,      // 文件消息响应
    P2P_ADDR_NOTIFY = 19    // 点对点地址通知（服务端转发对方IP/Port）
};

// 用户信息结构体（包含ip和dataPort）
struct UserInfo {
    int userId;             
    std::string nickname;   
    bool isOnline;          
    std::string ip;         
    int dataPort = 0; 
    int udpPort = 0;      
};

// 登录请求/响应
struct LoginReq {
    std::string nickname;
    int dataPort;       // P2P TCP 端口（对应服务端dataPort）
    int udpPort;        // P2P UDP 端口（对应服务端udpPort）
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

// 图片消息结构体
struct ImageMsg {
    int senderId = 0;           
    int receiverId = 0;         
    std::string fileName;       // 图片文件名
    uint64_t fileSize = 0;      // 图片大小（字节，统一为数值类型）
    std::string base64Data;     // Base64编码的图片数据/分片
    uint32_t fragmentIdx = 0;   // 分片索引（0=元信息）
    uint32_t totalFragments = 1;// 总分片数
    TransportProtocol protocol = TransportProtocol::TCP; // 传输协议
};

// 图片消息响应
struct ImageMsgRsp {
    bool success = false;
    std::string msg;
    int receiverId = 0;
};

// 文件消息结构体（统一字段类型+补充校验）
struct FileMsg {
    int senderId = 0;
    int receiverId = 0;          // 接收者ID，0表示群发
    std::string fileName;        // 文件名
    uint64_t fileSize = 0;       // 文件大小（字节，数值类型）
    std::string fileData;        // 文件数据（Base64）
    std::string fileMd5;         // 文件MD5（校验完整性）
    uint32_t dataLen;            // 数据长度
    bool isComplete = false;     // 是否为完整文件
    std::string base64Data;      // Base64编码的文件数据片段
    uint32_t fragmentIdx = 0;    // 分片索引
    uint32_t totalFragments = 1; // 总分片数
    TransportProtocol protocol = TransportProtocol::TCP; // 传输协议
};

// 文件消息响应
struct FileMsgRsp {
    bool success = false;
    std::string msg;
    int receiverId = 0;
    std::string fileMd5; // 接收方返回的MD5（用于校验）
};

// 点对点地址通知结构体
struct P2PAddrNotify {
    uint32_t targetUserId = 0;  // 目标用户ID
    std::string targetIp;       // 目标用户IP
    uint16_t targetTcpPort = 0; // 目标TCP端口
    uint16_t targetUdpPort = 0; // 目标UDP端口
    uint32_t senderId = 0;      // 发起方ID
};

// JSON序列化特化（补充所有新增结构体）
namespace nlohmann {
template <> struct adl_serializer<UserInfo> {
    static void to_json(json& j, const UserInfo& u) {
        j = json{
            {"userId", u.userId}, 
            {"nickname", u.nickname}, 
            {"isOnline", u.isOnline},
            {"ip", u.ip}, 
            {"dataPort", u.dataPort},
            {"udpPort", u.udpPort} // 新增UDP端口序列化
        };
    }
    static void from_json(const json& j, UserInfo& u) {
        u.userId = j.contains("userId") ? j["userId"].get<int>() : 0;
        u.nickname = j.contains("nickname") ? j["nickname"].get<std::string>() : "";
        u.isOnline = j.contains("isOnline") ? j["isOnline"].get<bool>() : false;
        u.ip = j.contains("ip") ? j["ip"].get<std::string>() : "";
        u.dataPort = j.contains("dataPort") ? j["dataPort"].get<int>() : 0;
        u.udpPort = j.contains("udpPort") ? j["udpPort"].get<int>() : 0;
    }
};

template <> struct adl_serializer<LoginReq> {
    static void to_json(json& j, const LoginReq& req) { 
        // 补全dataPort和udpPort的序列化
        j = json{
            {"nickname", req.nickname},
            {"dataPort", req.dataPort},
            {"udpPort", req.udpPort}
        }; 
    }
    static void from_json(const json& j, LoginReq& req) { 
        req.nickname = j.contains("nickname") ? j["nickname"].get<std::string>() : "";
        // 补全dataPort和udpPort的反序列化，添加默认值0
        req.dataPort = j.contains("dataPort") ? j["dataPort"].get<int>() : 0;
        req.udpPort = j.contains("udpPort") ? j["udpPort"].get<int>() : 0;
    }
};

template <> struct adl_serializer<LoginRsp> {
    static void to_json(json& j, const LoginRsp& rsp) {
        j = json{
            {"success", rsp.success}, 
            {"msg", rsp.msg}, 
            {"userId", rsp.userId}
        };
    }
    static void from_json(const json& j, LoginRsp& rsp) {
        rsp.success = j.contains("success") ? j["success"].get<bool>() : false;
        rsp.msg = j.contains("msg") ? j["msg"].get<std::string>() : "";
        rsp.userId = j.contains("userId") ? j["userId"].get<int>() : -1;
    }
};

template <> struct adl_serializer<UserListRsp> {
    static void to_json(json& j, const UserListRsp& rsp) { 
        j = json{{"users", rsp.users}, {"userCount", rsp.users.size()}}; 
    }
    static void from_json(const json& j, UserListRsp& rsp) {
        rsp.users = j.contains("users") ? j["users"].get<std::vector<UserInfo>>() : std::vector<UserInfo>();
    }
};

template <> struct adl_serializer<UserStatusNotify> {
    static void to_json(json& j, const UserStatusNotify& notify) {
        j = json{
            {"userId", notify.userId}, 
            {"nickname", notify.nickname}, 
            {"isOnline", notify.isOnline}
        };
    }
    static void from_json(const json& j, UserStatusNotify& notify) {
        notify.userId = j.contains("userId") ? j["userId"].get<int>() : 0;
        notify.nickname = j.contains("nickname") ? j["nickname"].get<std::string>() : "";
        notify.isOnline = j.contains("isOnline") ? j["isOnline"].get<bool>() : false;
    }
};

// PrivateMsg序列化
template <> struct adl_serializer<PrivateMsg> {
    static void to_json(json& j, const PrivateMsg& msg) {
        j = json{
            {"senderId", msg.senderId}, 
            {"receiverId", msg.receiverId}, 
            {"content", msg.content}
        };
    }
    static void from_json(const json& j, PrivateMsg& msg) {
        msg.senderId = j.contains("senderId") ? j["senderId"].get<int>() : 0;
        msg.receiverId = j.contains("receiverId") ? j["receiverId"].get<int>() : 0;
        msg.content = j.contains("content") ? j["content"].get<std::string>() : "";
    }
};

// PrivateMsgRsp序列化
template <> struct adl_serializer<PrivateMsgRsp> {
    static void to_json(json& j, const PrivateMsgRsp& rsp) {
        j = json{
            {"success", rsp.success}, 
            {"msg", rsp.msg}, 
            {"receiverId", rsp.receiverId}
        };
    }
    static void from_json(const json& j, PrivateMsgRsp& rsp) {
        rsp.success = j.contains("success") ? j["success"].get<bool>() : false;
        rsp.msg = j.contains("msg") ? j["msg"].get<std::string>() : "";
        rsp.receiverId = j.contains("receiverId") ? j["receiverId"].get<int>() : 0;
    }
};

// ImageMsg序列化（适配TransportProtocol）
template <> struct adl_serializer<ImageMsg> {
    static void to_json(json& j, const ImageMsg& msg) {
        j = json{
            {"senderId", msg.senderId},
            {"receiverId", msg.receiverId},
            {"fileName", msg.fileName},
            {"fileSize", msg.fileSize},
            {"base64Data", msg.base64Data},
            {"fragmentIdx", msg.fragmentIdx},
            {"totalFragments", msg.totalFragments},
            {"protocol", static_cast<uint32_t>(msg.protocol)}
        };
    }
    static void from_json(const json& j, ImageMsg& msg) {
        msg.senderId = j.contains("senderId") ? j["senderId"].get<int>() : 0;
        msg.receiverId = j.contains("receiverId") ? j["receiverId"].get<int>() : 0;
        msg.fileName = j.contains("fileName") ? j["fileName"].get<std::string>() : "";
        msg.fileSize = j.contains("fileSize") ? j["fileSize"].get<uint64_t>() : 0;
        msg.base64Data = j.contains("base64Data") ? j["base64Data"].get<std::string>() : "";
        msg.fragmentIdx = j.contains("fragmentIdx") ? j["fragmentIdx"].get<uint32_t>() : 0;
        msg.totalFragments = j.contains("totalFragments") ? j["totalFragments"].get<uint32_t>() : 1;
        uint32_t proto = j.contains("protocol") ? j["protocol"].get<uint32_t>() : 0;
        msg.protocol = static_cast<TransportProtocol>(proto);
    }
};

// ImageMsgRsp序列化
template <> struct adl_serializer<ImageMsgRsp> {
    static void to_json(json& j, const ImageMsgRsp& rsp) {
        j = json{
            {"success", rsp.success},
            {"msg", rsp.msg},
            {"receiverId", rsp.receiverId}
        };
    }
    static void from_json(const json& j, ImageMsgRsp& rsp) {
        rsp.success = j.contains("success") ? j["success"].get<bool>() : false;
        rsp.msg = j.contains("msg") ? j["msg"].get<std::string>() : "";
        rsp.receiverId = j.contains("receiverId") ? j["receiverId"].get<int>() : 0;
    }
};

// FileMsg序列化
template <> struct adl_serializer<FileMsg> {
    static void to_json(json& j, const FileMsg& msg) {
        j = json{
            {"senderId", msg.senderId},
            {"receiverId", msg.receiverId},
            {"fileName", msg.fileName},
            {"fileSize", msg.fileSize},
            {"fileMd5", msg.fileMd5},
            {"isComplete", msg.isComplete},
            {"base64Data", msg.base64Data},
            {"fragmentIdx", msg.fragmentIdx},
            {"totalFragments", msg.totalFragments},
            {"protocol", static_cast<uint32_t>(msg.protocol)}
        };
    }
    static void from_json(const json& j, FileMsg& msg) {
        msg.senderId = j.contains("senderId") ? j["senderId"].get<int>() : 0;
        msg.receiverId = j.contains("receiverId") ? j["receiverId"].get<int>() : 0;
        msg.fileName = j.contains("fileName") ? j["fileName"].get<std::string>() : "";
        msg.fileSize = j.contains("fileSize") ? j["fileSize"].get<uint64_t>() : 0;
        msg.fileMd5 = j.contains("fileMd5") ? j["fileMd5"].get<std::string>() : "";
        msg.isComplete = j.contains("isComplete") ? j["isComplete"].get<bool>() : false;
        msg.base64Data = j.contains("base64Data") ? j["base64Data"].get<std::string>() : "";
        msg.fragmentIdx = j.contains("fragmentIdx") ? j["fragmentIdx"].get<uint32_t>() : 0;
        msg.totalFragments = j.contains("totalFragments") ? j["totalFragments"].get<uint32_t>() : 1;
        uint32_t proto = j.contains("protocol") ? j["protocol"].get<uint32_t>() : 0;
        msg.protocol = static_cast<TransportProtocol>(proto);
    }
};

// FileMsgRsp序列化
template <> struct adl_serializer<FileMsgRsp> {
    static void to_json(json& j, const FileMsgRsp& rsp) {
        j = json{
            {"success", rsp.success},
            {"msg", rsp.msg},
            {"receiverId", rsp.receiverId},
            {"fileMd5", rsp.fileMd5}
        };
    }
    static void from_json(const json& j, FileMsgRsp& rsp) {
        rsp.success = j.contains("success") ? j["success"].get<bool>() : false;
        rsp.msg = j.contains("msg") ? j["msg"].get<std::string>() : "";
        rsp.receiverId = j.contains("receiverId") ? j["receiverId"].get<int>() : 0;
        rsp.fileMd5 = j.contains("fileMd5") ? j["fileMd5"].get<std::string>() : "";
    }
};

// P2PAddrNotify序列化
template <> struct adl_serializer<P2PAddrNotify> {
    static void to_json(json& j, const P2PAddrNotify& notify) {
        j = json{
            {"targetUserId", notify.targetUserId},
            {"targetIp", notify.targetIp},
            {"targetTcpPort", notify.targetTcpPort},
            {"targetUdpPort", notify.targetUdpPort},
            {"senderId", notify.senderId}
        };
    }
    static void from_json(const json& j, P2PAddrNotify& notify) {
        notify.targetUserId = j.contains("targetUserId") ? j["targetUserId"].get<uint32_t>() : 0;
        notify.targetIp = j.contains("targetIp") ? j["targetIp"].get<std::string>() : "";
        notify.targetTcpPort = j.contains("targetTcpPort") ? j["targetTcpPort"].get<uint16_t>() : 0;
        notify.targetUdpPort = j.contains("targetUdpPort") ? j["targetUdpPort"].get<uint16_t>() : 0;
        notify.senderId = j.contains("senderId") ? j["senderId"].get<uint32_t>() : 0;
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

// ========== 新增：兼容原有调用的反序列化封装函数 ==========
// 替代deserializeImageMsg，解决undefined reference
inline ImageMsg deserializeImageMsg(const std::string& jsonStr) {
    return deserialize<ImageMsg>(jsonStr);
}

// 替代deserializeFileMsg，解决undefined reference
inline FileMsg deserializeFileMsg(const std::string& jsonStr) {
    return deserialize<FileMsg>(jsonStr);
}

// 替代deserializeP2PAddrNotify，解决undefined reference
inline P2PAddrNotify deserializeP2PAddrNotify(const std::string& jsonStr) {
    return deserialize<P2PAddrNotify>(jsonStr);
}

inline ImageMsgRsp deserializeImageMsgRsp(const std::string& jsonStr) {
    return deserialize<ImageMsgRsp>(jsonStr);
}

inline FileMsgRsp deserializeFileMsgRsp(const std::string& jsonStr) {
    return deserialize<FileMsgRsp>(jsonStr);
}

// ========== 新增：兼容原有调用的序列化封装函数 ==========
inline std::string serializeImageMsg(const ImageMsg& msg) {
    return serialize<ImageMsg>(msg);
}

inline std::string serializeFileMsg(const FileMsg& msg) {
    return serialize<FileMsg>(msg);
}

inline std::string serializeP2PAddrNotify(const P2PAddrNotify& notify) {
    return serialize<P2PAddrNotify>(notify);
}

inline std::string serializeImageMsgRsp(const ImageMsgRsp& rsp) {
    return serialize<ImageMsgRsp>(rsp);
}

// Qt客户端sendPacket函数声明
#if defined(QT_CORE_LIB)
#include <QTcpSocket>
#include <QByteArray>
// 重载1：适配std::string数据
inline bool sendPacket(QTcpSocket* socket, uint32_t msgType, const std::string& data, uint32_t msgId, uint32_t senderId) {
    if (!socket || socket->state() != QTcpSocket::ConnectedState) {
        return false;
    }

    PacketHeader header;
    header.msgType = msgType;
    header.dataLen = static_cast<uint32_t>(data.size());
    header.msgId = msgId;
    header.senderId = senderId;

    // 转换为网络字节序
    PacketHeader netHeader = htonHeader(header);

    // 发送头部+数据
    socket->write(reinterpret_cast<const char*>(&netHeader), sizeof(PacketHeader));
    socket->write(data.c_str(), data.size());
    return socket->flush();
}

// 重载2：适配QByteArray数据
inline bool sendPacket(QTcpSocket* socket, uint32_t msgType, const QByteArray& data) {
    return sendPacket(socket, msgType, data.toStdString(), 0, 0);
}
#endif

#endif // PROTOCOL_H