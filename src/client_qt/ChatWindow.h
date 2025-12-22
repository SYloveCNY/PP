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
#include <QKeyEvent>       // 新增：处理键盘事件（回车发送）
#include <map>             // 新增：用于存储在线用户列表
#include <QLabel> 
#include <QTimer>
#include "protocol_qt.h"   // 包含UserInfo、MsgType等定义

class ChatWindow : public QWidget {
    Q_OBJECT
public:
    // 构造函数（保持不变）
    ChatWindow(int userId, const QString &nickname, QTcpSocket *serverSocket, QUdpSocket *udpSocket, QWidget *parent = nullptr);
    virtual ~ChatWindow();  // 新增：析构函数声明（解决隐式声明错误）

private slots:
    void onSendClicked();       // 发送按钮点击槽函数
    void onServerReadyRead();   // 接收服务端TCP消息
    void onUdpReadyRead();      // 接收UDP点对点消息
    void sendMessage();         // 发送消息核心函数
    void onTextEdited();        // 新增：文本编辑框变化槽函数（声明）
    void onUserSelected(QListWidgetItem *item); // 新增：用户选择槽函数（声明）
    void sendHeartbeat(); // 发送心跳包的槽函数

private:
    // 重写键盘事件（捕捉回车发送）
    void keyPressEvent(QKeyEvent *event) override; // 新增：声明重写的事件函数
    // 显示消息（已有）
    void showMessage(const QString &sender, const QString &content);
    // 新增：更新在线用户列表（声明，与cpp实现匹配）
    void updateOnlineUsers(const std::map<int, UserInfo> &users);

    // 成员变量（补充缺失的m_onlineUsers）
    int m_userId;               // 当前用户ID
    QString m_nickname;         // 当前用户昵称
    int m_selectedUserId = 0;   // 选中的收信人ID（0=广播）
    QTcpSocket *m_serverSocket; // 与服务端通信的TCP Socket
    QUdpSocket *m_udpSocket;    // 点对点通信的UDP Socket
    QListWidget *m_chatList;    // 聊天记录显示列表
    QTextEdit *m_inputEdit;     // 消息输入框
    QPushButton *m_sendBtn;     // 发送按钮
    QListWidget *m_userList;    // 新增：在线用户列表UI控件
    QByteArray m_recvBuffer;    // 新增：缓存接收的数据
    std::map<int, UserInfo> m_onlineUsers; // 新增：存储在线用户（解决未声明错误）
    QTimer *m_heartbeatTimer;   // 定时发送心跳的定时器
};

#endif // CHATWINDOW_H