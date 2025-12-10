#ifndef CHATWINDOW_H
#define CHATWINDOW_H

#include <QWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QVBoxLayout>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QKeyEvent>   // 新增：声明按键事件
#include <map>         // 新增
#include <vector>      // 新增
#include <string>      // 新增
#include "protocol_qt.h"

class ChatWindow : public QWidget {
    Q_OBJECT
public:
    ChatWindow(int userId, const QString &nickname, QTcpSocket *serverSocket, QWidget *parent = nullptr);
    ~ChatWindow();
protected:
    void keyPressEvent(QKeyEvent *event) override; // 新增：声明按键事件
private slots:
    void onServerReadyRead();
    void onSendClicked();
    void onTextEdited();
    void onUserSelected(QListWidgetItem *item);
private:
    void sendPacket(QTcpSocket *socket, MsgType msgType, const std::vector<char> &data); // 修复：添加std::
    void updateOnlineUsers(const std::map<int, UserInfo> &users); // 修复：添加std::
    void showMessage(const QString &sender, const QString &content);

    int m_userId;
    QString m_nickname;
    QTcpSocket *m_serverSocket;
    QUdpSocket *m_udpSocket;
    int m_selectedUserId = -1;

    // 控件
    QListWidget *m_userList;
    QTextEdit *m_msgDisplay;
    QLineEdit *m_msgInput;
    QPushButton *m_sendBtn;
    std::map<int, UserInfo> m_onlineUsers; // 修复：添加std::
};

#endif // CHATWINDOW_H