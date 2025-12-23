#ifndef PROTOCOL_QT_H
#define PROTOCOL_QT_H

// 仅客户端包含QT头文件
#include <QTcpSocket>
#include <QByteArray>
// 包含核心协议
#include "protocol_base.h"

// ========== QT客户端专属sendPacket（带字节序转换修复） ==========
inline bool sendPacket(QTcpSocket* socket, uint32_t msgType, const std::vector<char>& data) {
    if (!socket || socket->state() != QTcpSocket::ConnectedState) {
        qDebug() << "[发送失败] 未连接服务端或socket无效";
        return false;
    }

    PacketHeader header;
    // 核心修复：网络字节序转换（必须加！）
    header.msgType = htonl(msgType);   // 本地字节序 → 网络字节序
    header.dataLen = htonl(static_cast<uint32_t>(data.size())); // 消息体长度转换

    // 构造完整数据包（包头 + 消息体）
    QByteArray packet;
    // 写入包头（8字节）
    packet.append(reinterpret_cast<const char*>(&header), sizeof(PacketHeader));
    // 写入消息体（std::vector<char>转QByteArray）
    packet.append(data.data(), data.size());

    // 禁用Nagle算法，避免数据缓冲延迟
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    // 强制发送并等待完成
    qint64 sentLen = socket->write(packet);
    socket->flush();
    if (!socket->waitForBytesWritten(1000)) { // 1秒超时
        qDebug() << "[发送超时] " << socket->errorString();
        return false;
    }

    // 打印发送详情（调试用）
    qDebug() << "[发送成功] msgType=" << msgType << "（网络字节序=" << header.msgType << "）"
             << "，消息体长度=" << data.size() << "，总字节数=" << sentLen;
    return sentLen == packet.size();
}

#endif // PROTOCOL_QT_H