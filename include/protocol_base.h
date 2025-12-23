#ifndef PROTOCOL_BASE_H
#define PROTOCOL_BASE_H

#include <vector>
#include <string>
#include <cstdint>  // 确保uint32_t等类型可用

// 消息类型（和之前保持一致，重点关注USER_LIST相关）
enum class MsgType : uint32_t {
    LOGIN_REQ = 1,      // 登录请求
    LOGIN_RSP = 2,      // 登录响应
    USER_LIST_REQ = 3,  // 在线用户列表请求
    USER_LIST_RSP = 4,  // 在线用户列表响应
    COMMON_MSG = 5      // 普通聊天消息
};

// 协议头（固定8字节：4字节消息类型 + 4字节JSON数据长度）
struct PacketHeader {
    uint32_t msgType;   // 消息类型（网络字节序）
    uint32_t dataLen;   // JSON数据长度（网络字节序）
};

// UserInfo结构体（在线用户信息，服务端和客户端共用）
struct UserInfo {
    int userId;                // 用户ID
    std::string nickname;      // 昵称
    std::string ip;            // 客户端IP
    uint16_t dataPort;         // 数据端口
};

// 登录请求结构体（JSON序列化用）
struct LoginReq {
    std::string nickname;
    std::string avatar;  // 头像（如果是字符串路径，直接JSON传输；如果是二进制，可转base64）
    uint16_t dataPort;
};

// 登录响应结构体
struct LoginRsp {
    bool success;
    std::string msg;       // 提示信息（如“登录成功”“昵称已存在”）
    int userId;            // 分配的用户ID
};

// 在线用户列表响应结构体
struct UserListRsp {
    std::vector<UserInfo> users;  // 在线用户列表
};

#endif // PROTOCOL_BASE_H