#ifndef MESSAGESENDER_H
#define MESSAGESENDER_H

#include <QTcpSocket>
#include <map>
#include <ctime>
#include "protocol.h"

class MessageSender
{
private:
    std::map<uint32_t, std::pair<time_t, std::string>> pendingMessages;
    uint32_t nextMsgId = 1;
    QTcpSocket* socket;

public:
    // 构造函数
    MessageSender(QTcpSocket* sock) : socket(sock) {}
    
    // 发送消息（返回消息ID）
    uint32_t sendMessage(MsgType type, const std::string& data, uint32_t senderId = 0);
    
    // 处理消息确认
    void onMessageAck(uint32_t msgId);
    
    // 重传超时未确认的消息
    void retransmitMessages();
};

#endif // MESSAGESENDER_H