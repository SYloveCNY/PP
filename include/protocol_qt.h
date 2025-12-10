#ifndef PROTOCOL_QT_H
#define PROTOCOL_QT_H

// 仅客户端包含QT头文件
#include <QTcpSocket>
// 包含核心协议
#include "protocol_base.h"

// ========== QT客户端专属sendPacket ==========
inline bool sendPacket(QTcpSocket* socket, uint32_t msgType, const std::vector<char>& data) {
    PacketHeader header;
    header.msgType = msgType;
    header.dataLen = static_cast<uint32_t>(data.size());
    
    std::vector<char> sendData;
    sendData.insert(sendData.end(), reinterpret_cast<const char*>(&header),
                    reinterpret_cast<const char*>(&header) + sizeof(PacketHeader));
    sendData.insert(sendData.end(), data.begin(), data.end());
    
    qint64 sent = socket->write(sendData.data(), sendData.size());
    return sent == static_cast<qint64>(sendData.size());
}

#endif // PROTOCOL_QT_H
