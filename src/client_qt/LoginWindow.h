#ifndef LOGINWINDOW_H
#define LOGINWINDOW_H

#include <QWidget>
#include <QTcpSocket>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include "protocol_qt.h"

class LoginWindow : public QWidget
{
    Q_OBJECT

public:
    explicit LoginWindow(QWidget *parent = nullptr);
    virtual ~LoginWindow();

private slots:
    void onLoginClicked();
    void onSocketError(QAbstractSocket::SocketError error);
    // 新增：接收服务端响应（登录结果）的槽函数
    void onReadyRead();

signals:
    // 声明 loginSuccess 信号（参数传递登录成功后的关键信息）
    void loginSuccess(
        int userId,                  // 用户ID
        const std::string& nickname, // 昵称
        QTcpSocket* socket           // 复用服务端连接
    );

private:
    QLabel *titleLabel;
    QLabel *nicknameLabel;
    QLineEdit *nicknameEdit;
    QLabel *avatarLabel;
    QLineEdit *avatarEdit;
    QPushButton *browseBtn;
    QPushButton *loginBtn;
    QVBoxLayout *mainLayout;
    QHBoxLayout *avatarLayout;

    QTcpSocket *serverSocket;
    std::string avatarPath;
};

#endif // LOGINWINDOW_H