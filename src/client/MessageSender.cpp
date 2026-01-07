#include "MessageSender.h"
#include <QDateTime>

uint32_t MessageSender::sendMessage(MsgType type, const std::string& data, uint32_t senderId) {
    uint32_t msgId = nextMsgId++;
    
    // 构建数据包（复用protocol.h中的sendPacket）
    bool success = sendPacket(socket, type, data, msgId, senderId);
    if (!success) {
        // 发送失败，加入重传队列
        pendingMessages[msgId] = {time(nullptr), data};
    }
    
    return msgId;
}

void MessageSender::onMessageAck(uint32_t msgId) {
    pendingMessages.erase(msgId);
}

void MessageSender::retransmitMessages() {
    time_t now = time(nullptr);
    for (auto& [msgId, pair] : pendingMessages) {
        if (now - pair.first > 5) { // 5秒未确认重传
            sendPacket(socket, MsgType::COMMON_MSG, pair.second, msgId);
            pair.first = now; // 更新发送时间
        }
    }
}