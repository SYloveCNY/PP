// 客户端心跳发送
void startHeartbeat(QTcpSocket* socket) {
    QTimer* timer = new QTimer();
    QObject::connect(timer, &QTimer::timeout, [socket]() {
        if (socket->state() == QAbstractSocket::ConnectedState) {
            PacketHeader header;
            header.msgType = htonl(static_cast<uint32_t>(MsgType::HEARTBEAT));
            header.dataLen = 0;
            header.msgId = 0;
            header.senderId = htonl(currentUserId);
            
            socket->write(reinterpret_cast<const char*>(&header), sizeof(PacketHeader));
        }
    });
    timer->start(5000); // 每5秒发送一次心跳
}