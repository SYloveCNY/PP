#ifndef PROTOCOL_QT_H
#define PROTOCOL_QT_H

#include <QTcpSocket>
#include <QByteArray>
#include "protocol.h"

// Qt版本的sendPacket（适配完整PacketHeader）
inline bool sendPacket(QTcpSocket* socket, uint32_t msgType, const QByteArray& data) {
    if (!socket || !socket->isValid()) return false;

    PacketHeader header;
    header.msgType = msgType;
    header.dataLen = static_cast<uint32_t>(data.size());
    header.msgId = 0;          // 补充msgId
    header.senderId = 0;       // 补充senderId

    // 转换为网络字节序
    PacketHeader netHeader = htonHeader(header);

    // 发送头部+数据
    socket->write(reinterpret_cast<const char*>(&netHeader), sizeof(PacketHeader));
    socket->write(data);
    return socket->flush();
}

#endif // PROTOCOL_QT_H