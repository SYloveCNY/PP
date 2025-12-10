#ifndef LOGINWINDOW_H
#define LOGINWINDOW_H

#include <QTcpSocket> 
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QTcpSocket>
#include <string>  // 新增
#include <vector>  // 新增
#include "protocol_qt.h"

class LoginWindow : public QWidget {
    Q_OBJECT
public:
    LoginWindow(QWidget *parent = nullptr);
signals:
    void loginSuccess(int userId, const QString &nickname, QTcpSocket *serverSocket);
private slots:
    void onLoginClicked();
    void onConnected();
    void onReadyRead();
private:
    QTcpSocket* m_tcpSocket;  // 新增：声明tcp socket成员变量
    QLineEdit *nicknameEdit;
    QLineEdit *avatarEdit;
    QPushButton *loginBtn;
    QTcpSocket *serverSocket;
};

#endif // LOGINWINDOW_H