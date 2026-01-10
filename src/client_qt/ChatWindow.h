#ifndef CHATWINDOW_H
#define CHATWINDOW_H

#include <QWidget>
#include <QTcpSocket>
#include <QString>
#include <QTimer>
#include <QListWidget>
#include "../../include/protocol.h"

namespace Ui {
class ChatWindow;
}

class QLabel;
class QTextEdit;
class QLineEdit;
class QPushButton;

class ChatWindow : public QWidget {
    Q_OBJECT

public:
    explicit ChatWindow(QWidget *parent = nullptr);
    ~ChatWindow() override;

    // 设置登录信息
    void setLoginInfo(int userId, const QString& nickname, QTcpSocket* socket);

private slots:
    // 发送心跳包
    void sendHeartbeat();
    // 处理服务器数据
    void onServerReadyRead();
    // 发送普通消息
    void sendMessage();
    // 打开图片选择对话框
    void sendImageDialog();
    // 获取在线用户
    void getOnlineUserList();
    // 切换聊天类型（公聊/私聊）
    void switchChatType();

private:
    // 更新用户列表
    void updateUserList(const std::vector<UserInfo>& users);
    // 处理上线/下线通知
    void processNotification(const MsgType& type, const std::string& data);
    // 发送图片
    void sendImage(const QString& filePath);
    // 显示聊天消息（仅一个实现）
    void showMessage(const QString& sender, const QString& content, bool isPrivate = false);

    // 成员变量
    int m_userId = 0;
    QString m_nickname;
    QTcpSocket* m_socket = nullptr;
    QTimer* m_heartbeatTimer = nullptr; // 心跳定时器
    Ui::ChatWindow* ui;
    QLabel* m_userInfoLabel = nullptr;
    bool m_isPrivateChat = false;
};

#endif // CHATWINDOW_H