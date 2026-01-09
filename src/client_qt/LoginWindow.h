#ifndef LOGINWINDOW_H
#define LOGINWINDOW_H

#include <QWidget>
#include <QTcpSocket>  // 新增：m_serverSocket 依赖

namespace Ui {
class LoginWindow;  // UI类声明
}

class LoginWindow : public QWidget {
    Q_OBJECT  // 必须有，支持Qt信号槽

public:
    explicit LoginWindow(QWidget *parent = nullptr);
    ~LoginWindow();

private slots:
    uint16_t getRandomAvailablePort();
    // 声明登录按钮的槽函数（与UI文件中的按钮名称对应）
    void on_pushButton_login_clicked();

private:
    Ui::LoginWindow *ui;  // 声明UI成员
    QTcpSocket* m_serverSocket;  // 声明与服务端的连接socket
};

#endif // LOGINWINDOW_H
