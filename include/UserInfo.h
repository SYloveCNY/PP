#ifndef USERINFO_H
#define USERINFO_H

#include <string>
#include <cstdint>  // 用于 uint16_t 等类型
#include <ctime>

// 用户信息结构体（服务端和客户端共用，字段必须与序列化/反序列化函数一致）
struct UserInfo {
    int userId;                // 用户唯一ID
    std::string nickname;      // 昵称
    std::string avatar;        // 新增：头像（与登录请求的 req.avatar 对应）
    std::string ip;            // 客户端IP地址
    uint16_t dataPort;         // 客户端数据端口
    int managePort;            // 客户端管理端口（服务端用）
    time_t lastHeartbeatTime;  // 最后心跳时间（服务端用）
};

#endif // USERINFO_H
