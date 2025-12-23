#ifndef CHATWINDOW_H
#define CHATWINDOW_H

#include <QWidget>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QByteArray>
#include <QTimer>
#include <map>
#include "UserInfo.h"  // 包含 UserInfo 结构体
#include "protocol_qt.h"  // 包含 MsgType、PacketHeader 等枚举/结构体

class QListWidget;
class QTextEdit;
class QLineEdit;
class QPushButton;

class ChatWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ChatWindow(int userId, const QString& nickname, QTcpSocket* serverSocket, QUdpSocket* udpSocket = nullptr, QWidget* parent = nullptr);
    ~ChatWindow();

private slots:
    void onServerReadyRead();       // 接收服务端数据
    void onDisconnected();          // 处理断开连接
    void sendHeartbeat();           // 发送心跳包
    void sendMessage(const QString& content);  // 发送聊天消息

private:
    void initUI();                  // 初始化UI
    void initTimers();              // 初始化定时器
    void bindSocketSignals();       // 绑定Socket信号槽
    void sendUserListReq();         // 发送用户列表请求
    void updateOnlineUsers(const std::map<int, UserInfo>& onlineUsers);  // 更新用户列表UI
    void showMessage(const QString& sender, const QString& content);     // 显示聊天消息
    void handleMessage(MsgType msgType, const std::vector<char>& payload);  // 处理不同类型消息

private:
    int m_userId;                   // 当前登录用户ID
    QString m_nickname;             // 当前登录用户昵称
    QTcpSocket* m_serverSocket;     // TCP连接Socket（与服务端）
    QUdpSocket* m_udpSocket;        // UDP Socket（点对点通信，可选）
    QByteArray m_recvBuffer;        // 接收数据缓存（处理粘包/半包）
    std::map<int, UserInfo> m_onlineUsers;  // 在线用户列表（key=userId）
    int m_selectedUserId;           // 选中的目标用户ID（0=广播）

    // UI控件
    QListWidget* m_userListWidget;  // 在线用户列表控件
    QTextEdit* m_chatDisplay;       // 聊天显示区域
    QLineEdit* m_msgInput;          // 消息输入框
    QPushButton* m_sendBtn;         // 发送按钮

    // 定时器
    QTimer* m_heartbeatTimer;       // 心跳定时器
    QTimer* m_userListTimeoutTimer; // 用户列表请求超时定时器
};

#endif // CHATWINDOW_H