#ifndef LOGINWINDOW_H
#define LOGINWINDOW_H

#include <QWidget>
#include <QTcpSocket>  // 新增：m_serverSocket 依赖
#include <QTimer>
#include <QCloseEvent>

namespace Ui {
class LoginWindow;  // UI类声明
}

class LoginWindow : public QWidget {
    Q_OBJECT  // 必须有，支持Qt信号槽

public:
    explicit LoginWindow(QWidget *parent = nullptr);
    ~LoginWindow();

protected:
    // 重写关闭事件：发送主动下线请求
    void closeEvent(QCloseEvent *event) override;

private slots:
    // 声明登录按钮的槽函数（与UI文件中的按钮名称对应）
    void on_pushButton_login_clicked();
    // 处理登录响应的readyRead
    void onLoginResponseReadyRead();
    // 接收服务端消息
    void onReadyRead();
    // 发送心跳包
    void sendHeartbeat();

private:
    Ui::LoginWindow *ui;  // 声明UI成员
    QTcpSocket* m_serverSocket;  // 声明与服务端的连接socket
        QTimer* m_heartbeatTimer;         // 心跳定时器（新增）
    int m_userId;                     // 登录成功后的用户ID（新增）
    QString m_nickname;               // 当前登录昵称（新增）
    uint16_t m_dataPort;              // 数据端口（新增）

    // 封装发包函数（新增：处理网络字节序）
    bool sendPacket(QTcpSocket* socket, uint32_t msgType, const QByteArray& data);
    // 获取随机可用端口
    uint16_t getRandomAvailablePort();
};

#endif // LOGINWINDOW_H
