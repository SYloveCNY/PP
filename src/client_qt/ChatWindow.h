#ifndef CHATWINDOW_H
#define CHATWINDOW_H

#include <QWidget>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QTcpServer>
#include <QFileDialog>
#include <QFile>
#include <QCryptographicHash>
#include <QBuffer>
#include <QPixmap>
#include <QString>
#include <QTimer>
#include <QListWidget>
#include <QCloseEvent>
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

protected:
    void closeEvent(QCloseEvent *event) override; // 新增
    
private slots:

    // 发送心跳包
    void sendHeartbeat();
    // 处理服务器数据
    void onServerReadyRead();
    // 登录函数中，填充P2P TCP随机端口
    void sendLoginReq(const QString& nickname); 
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
    void sendImage();
    void sendImage(const QString& filePath); // 重载版本
    // 接收图片
    // void receiveImage(const ImageMsg& msg);
    // 发送文件
    void sendFile();
    // 分片发送文件
    void sendFileFragment(FileMsg metaMsg);
    // 更新传输进度
    void updateTransferProgress(int progress);
    // 接收文件
    void receiveFile(const FileMsg& metaMsg);
    // 新增处理点对点新连接的槽函数
    void onP2PNewConnection();
    // 新增槽函数：选择在线用户作为接收方
    void onUserItemClicked(QListWidgetItem* item);
    // 处理点对点数据接收
    void onP2PDataReady();
    // 新增UDP接收槽函数
    void onUdpReadyRead();

    void onP2PClientDisconnected();
    void onP2PSocketError(QAbstractSocket::SocketError socketError);
    void onP2PSocketReadyRead();
    void onP2PSocketDisconnected();
  
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
    QTcpSocket* m_tcpP2PSocket;    // 点对点TCP Socket
    QTcpSocket* m_tcpP2PRecvSocket;// 专门用于被动接收连接
    QUdpSocket* m_udpP2PSocket;    // 点对点UDP Socket
    QTcpServer* m_tcpP2PServer;    // 用于接收点对点TCP连接
    QHostAddress m_targetIp;       // 目标IP
    quint16 m_targetTcpPort;       // 目标TCP端口
    quint16 m_targetUdpPort;       // 目标UDP端口
    quint16 m_p2pTcpPort = 0;      // 保存系统分配的P2P TCP随机端口
    quint16 m_udpP2PPort = 0;      // UDP监听端口
    QFile* m_transferFile;         // 传输中的文件
    qint64 m_totalFileSize;        // 文件总大小
    qint64 m_sentFileSize;         // 已发送大小
    int m_selectedUserId = 0;      // 选中的聊天对象ID
    QTcpSocket* m_p2pClientSocket = nullptr; // 专门存储接收方的P2P客户端连接
    ImageMsg m_pendingImageMsg;              // 待接收的图片信息
    QByteArray m_pendingImageData;           // 待接收的图片数据
    QMap<int, QString> m_userPortMap;        // 缓存用户ID-端口映射（解决端口不匹配）
};

#endif // CHATWINDOW_H