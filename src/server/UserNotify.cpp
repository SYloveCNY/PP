// 处理用户上线通知
void notifyUserOnline(const UserInfo& user) {
    UserStatusNotify notify;
    notify.userId = user.userId;
    notify.nickname = user.nickname;
    notify.isOnline = true;
    
    // 序列化通知消息
    std::string jsonData = serialize(notify);
    
    // 向所有在线用户发送通知
    sendToAllUsers(MsgType::USER_ONLINE_NOTIFY, jsonData);
}

// 处理用户下线通知
void notifyUserOffline(int userId, const std::string& nickname) {
    UserStatusNotify notify;
    notify.userId = userId;
    notify.nickname = nickname;
    notify.isOnline = false;
    
    // 序列化通知消息
    std::string jsonData = serialize(notify);
    
    // 向所有在线用户发送通知
    sendToAllUsers(MsgType::USER_OFFLINE_NOTIFY, jsonData);
}