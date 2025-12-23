#ifndef PROTOCOL_QT_H
#define PROTOCOL_QT_H

#include <QTcpSocket>
#include <vector>
#include <string>
#include <iostream>
#include <ostream>
#include <QtEndian>  // 添加头文件
#include "protocol_base.h"

// 发送JSON消息（协议头+JSON数据）- 仅保留JSON传输需要的函数
inline bool sendPacket(QTcpSocket* socket, uint32_t msgType, const QByteArray& jsonData) {
    if (!socket || !socket->isValid()) {
        std::cerr << "[客户端] 发送失败：socket无效" << std::endl;
        return false;
    }

    PacketHeader header;
    header.msgType = msgType;
    header.dataLen = static_cast<uint32_t>(jsonData.size());

    // 用 qToBigEndian 替代 htonl
    PacketHeader netHeader;
    netHeader.msgType = qToBigEndian(header.msgType);
    netHeader.dataLen = qToBigEndian(header.dataLen);

    qint64 sent = socket->write((char*)&netHeader, sizeof(PacketHeader));
    sent += socket->write(jsonData);
    socket->flush();

    if (sent != sizeof(PacketHeader) + jsonData.size()) {
        std::cerr << "[客户端] 发送失败：数据未完全发送" << std::endl;
        return false;
    }
    return true;
}

#endif // PROTOCOL_QT_H

// #ifndef PROTOCOL_QT_H
// #define PROTOCOL_QT_H

// // 仅客户端包含QT头文件
// #include <QTcpSocket>
// #include <QByteArray>
// // 包含核心协议
// #include "protocol_base.h"

// // ========== QT客户端专属sendPacket（带字节序转换修复） ==========
// inline bool sendPacket(QTcpSocket* socket, uint32_t msgType, const std::vector<char>& data) {
//     if (!socket || socket->state() != QTcpSocket::ConnectedState) {
//         qDebug() << "[发送失败] 未连接服务端或socket无效";
//         return false;
//     }

//     PacketHeader header;
//     // 核心修复：网络字节序转换（必须加！）
//     header.msgType = htonl(msgType);   // 本地字节序 → 网络字节序
//     header.dataLen = htonl(static_cast<uint32_t>(data.size())); // 消息体长度转换

//     // 构造完整数据包（包头 + 消息体）
//     QByteArray packet;
//     // 写入包头（8字节）
//     packet.append(reinterpret_cast<const char*>(&header), sizeof(PacketHeader));
//     // 写入消息体（std::vector<char>转QByteArray）
//     packet.append(data.data(), data.size());

//     // 禁用Nagle算法，避免数据缓冲延迟
//     socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
//     // 强制发送并等待完成
//     qint64 sentLen = socket->write(packet);
//     socket->flush();
//     if (!socket->waitForBytesWritten(1000)) { // 1秒超时
//         qDebug() << "[发送超时] " << socket->errorString();
//         return false;
//     }

//     // 打印发送详情（调试用）
//     qDebug() << "[发送成功] msgType=" << msgType << "（网络字节序=" << header.msgType << "）"
//              << "，消息体长度=" << data.size() << "，总字节数=" << sentLen;
//     return sentLen == packet.size();
// }

// // 反序列化用户列表（客户端接收服务端响应）
// inline std::vector<UserInfo> deserializeUserListToVector(const std::vector<char>& data) {
//     std::vector<UserInfo> onlineUsers;
//     size_t offset = 0;

//     try {
//         // 1. 解析用户数量（网络字节序→主机字节序）
//         if (offset + sizeof(uint32_t) > data.size()) {
//             throw std::out_of_range("用户数量字段越界");
//         }
//         uint32_t userCount = ntohl(*reinterpret_cast<const uint32_t*>(data.data() + offset));
//         offset += sizeof(uint32_t);
//         std::cout << "[客户端] 解析到在线用户数量：" << userCount << std::endl;

//         // 2. 逐个解析用户信息
//         for (uint32_t i = 0; i < userCount; ++i) {
//             UserInfo user;
//             // 解析 userId（4字节）
//             if (offset + sizeof(int) > data.size()) throw std::out_of_range("userId字段越界");
//             user.userId = ntohl(*reinterpret_cast<const int*>(data.data() + offset));
//             offset += sizeof(int);

//             // 解析 nickname（长度4字节+内容）
//             if (offset + sizeof(uint32_t) > data.size()) throw std::out_of_range("nickname长度字段越界");
//             uint32_t nickLen = ntohl(*reinterpret_cast<const uint32_t*>(data.data() + offset));
//             offset += sizeof(uint32_t);
//             if (offset + nickLen > data.size()) throw std::out_of_range("nickname内容越界");
//             user.nickname = std::string(data.data() + offset, nickLen);
//             offset += nickLen;

//             // 解析 ip（长度4字节+内容）
//             if (offset + sizeof(uint32_t) > data.size()) throw std::out_of_range("ip长度字段越界");
//             uint32_t ipLen = ntohl(*reinterpret_cast<const uint32_t*>(data.data() + offset));
//             offset += sizeof(uint32_t);
//             if (offset + ipLen > data.size()) throw std::out_of_range("ip内容越界");
//             user.ip = std::string(data.data() + offset, ipLen);
//             offset += ipLen;

//             // 解析 dataPort（2字节）
//             if (offset + sizeof(uint16_t) > data.size()) throw std::out_of_range("dataPort字段越界");
//             user.dataPort = ntohs(*reinterpret_cast<const uint16_t*>(data.data() + offset));
//             offset += sizeof(uint16_t);

//             onlineUsers.push_back(user);
//         }
//     } catch (const std::exception& e) {
//         std::cerr << "[客户端] 解析用户列表失败：" << e.what() << std::endl;
//         onlineUsers.clear();
//     }

//     return onlineUsers;
// }

// #endif // PROTOCOL_QT_H