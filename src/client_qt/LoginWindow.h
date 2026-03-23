#ifndef LOGINWINDOW_H
#define LOGINWINDOW_H

#include <QWidget>
#include <QUdpSocket>
#include <QTcpSocket>
#include <QTimer>
#include <QCloseEvent>
#include <QFileDialog>
#include <QLabel>
#include <QPixmap>

namespace Ui {
class LoginWindow;  // UI类声明
}

class LoginWindow : public QWidget {
    Q_OBJECT  // 必须有，支持Qt信号槽

public:
    explicit LoginWindow(QWidget *parent = nullptr);
    ~LoginWindow();
    void loadAndShowAvatar(int userId);

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
    // 封装发包函数（新增：处理网络字节序）
    bool sendPacket(QTcpSocket* socket, uint32_t msgType, const QByteArray& data);
    // 获取随机可用端口
    uint16_t getRandomAvailablePort();
    // 图片上传函数
    void uploadAvatar();
    // UDP数据接收处理（主要处理ACK)
    void onUdpReadyRead();
    // 发送单个UDP分片
    void sendUdpAvatarChunk();
    // 重传当前分片
    void retryCurrentChunk();
    // 发送上传完成包
    void sendUdpAvatarFinish();
    // =UDP端口绑定函数
    bool bindUdpPort(int startPort);


private:
    Ui::LoginWindow *ui;          // 声明UI成员
    QTcpSocket* m_serverSocket;   // 声明与服务端的连接socket
    QTimer* m_heartbeatTimer;     // 心跳定时器（新增）
    int m_userId;                 // 登录成功后的用户ID（新增）
    QString m_nickname;           // 当前登录昵称（新增）
    uint16_t m_dataPort;          // 数据端口（新增）
    QString m_avatarPath;         // 保存选中的头像路径
    QUdpSocket* m_udpSocket;   // UDP套接字（复用P2P的9102端口）
    QByteArray m_avatarData;   // 头像二进制数据
    int m_totalChunks;         // 总分片数
    int m_currentChunk;        // 当前发送的分片ID
    QString m_fileMd5;         // 文件MD5
    QString m_fileName;        // 文件名
    QTimer* m_udpRetryTimer;   // UDP重传定时器
    const int UDP_CHUNK_SIZE = 1024; // UDP分片大小（1KB）
    const int UDP_RETRY_TIMEOUT = 1000; // 重传超时（1秒）
    int m_currentRetryCount;
    int m_serverUdpPort;
};

#endif // LOGINWINDOW_H
