#ifndef CHATWINDOW_H
#define CHATWINDOW_H

#include <QWidget>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QListWidget>
#include <QTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QKeyEvent>
#include <map>
#include <QLabel>
#include <QTimer>
#include "protocol_qt.h"   // 包含UserInfo、MsgType等协议定义

class ChatWindow : public QWidget {
    Q_OBJECT
public:
    // 构造函数：接收用户ID、昵称、TCP/UDP Socket（生命周期由ChatWindow接管）
    ChatWindow(int userId, const QString &nickname, QTcpSocket *serverSocket, QUdpSocket *udpSocket, QWidget *parent = nullptr);
    virtual ~ChatWindow();  // 析构函数（QT父对象自动管理子控件，无需额外实现）

private slots:
    void onSendClicked();       // 发送按钮点击槽函数
    void onServerReadyRead();   // 接收服务端TCP消息（核心数据接收）
    void onUdpReadyRead();      // 接收UDP消息（预留扩展）
    void sendMessage(const QString& content);  // 发送消息核心逻辑
    void onTextEdited();        // 输入框文本变化（控制发送按钮启用/禁用）
    void onUserSelected(QListWidgetItem *item); // 选择在线用户（更新目标ID）
    void sendHeartbeat();       // 定时发送心跳包（维持连接）

private:
    void keyPressEvent(QKeyEvent *event) override; // 重写键盘事件（回车发送）
    void showMessage(const QString &sender, const QString &content); // 显示聊天消息
    void updateOnlineUsers(const std::map<int, UserInfo> &users); // 更新在线用户列表UI

    // 成员变量（与cpp实现严格对应）
    int m_userId;               // 当前登录用户ID（服务端分配）
    QString m_nickname;         // 当前用户昵称
    int m_selectedUserId = 0;   // 选中的目标用户ID（0=广播，默认值）
    QTcpSocket *m_serverSocket; // TCP Socket（与服务端通信）
    QUdpSocket *m_udpSocket;    // UDP Socket（预留点对点通信）
    QListWidget *m_chatList;    // 聊天记录显示列表
    QTextEdit *m_inputEdit;     // 消息输入框（支持多行输入）
    QPushButton *m_sendBtn;     // 发送按钮
    QListWidget *m_userList;    // 在线用户列表UI
    QByteArray m_recvBuffer;    // TCP接收缓存（处理粘包/半包）
    std::map<int, UserInfo> m_onlineUsers; // 本地缓存在线用户列表
    QTimer *m_heartbeatTimer;   // 心跳定时器（3秒一次）
};

#endif // CHATWINDOW_H