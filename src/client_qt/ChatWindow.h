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
#include <QMap>
#include <QMutex>
#include <QScrollBar>
#include "../../include/protocol.h"

namespace Ui
{
    class ChatWindow;
}

class QLabel;
class QTextEdit;
class QLineEdit;
class QPushButton;

class ChatWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ChatWindow(QWidget *parent = nullptr);
    ~ChatWindow() override;

    // 设置登录信息
    void setLoginInfo(int userId, const QString &nickname, QTcpSocket *socket);
    // 登录函数中，填充P2P TCP随机端口
    void sendLoginReq(const QString &nickname);

public slots:
    // 显示聊天消息（仅一个实现）
    void showMessage(const QString &sender, const QString &content, bool isPrivate = false);
    // 更新传输进度
    void updateTransferProgress(int progress);
    // 获取在线用户
    void getOnlineUserList();
    // 发送普通消息
    void sendMessage();
    // 切换聊天类型（公聊/私聊）
    void switchChatType();
    // 发送图片
    void sendImage();
    // 发送文件
    void sendFile();
    // 处理服务器数据
    void onServerReadyRead();
    // 处理点对点新连接的槽函数
    void onP2PNewConnection();
    // 选择在线用户作为接收方
    void onUserItemClicked(QListWidgetItem *item);
    // 新增UDP接收槽函数
    void onUdpReadyRead();
    void onP2PSocketReadyRead();
    void onP2PSocketDisconnected();
    void onP2PSocketError(QAbstractSocket::SocketError socketError);
    void onP2PClientDisconnected();
    // 处理点对点数据接收
    void onP2PDataReady();

private slots:
    // 发送心跳包
    void sendHeartbeat();    
    // 打开图片选择对话框
    void sendImageDialog();
    // 处理中继TCP数据接收
    void onRelayTcpReadyRead();
    // 处理中继UDP数据接收
    void onRelayUdpReadyRead();

private:
    void sendImage(const QString &filePath); // 重载版本
    // 分片发送文件
    void sendFileFragment(FileMsg metaMsg);
    // 接收文件
    void receiveFile(const FileMsg &metaMsg);
    // 接收中继文件分片
    void receiveFileFragment(const FileMsg& fragMsg);
    // 处理中继分片数据
    void processRelayFragment(int senderId, const FileMsg& fragMsg);
    // 处理上线/下线通知
    void processNotification(const MsgType &type, const std::string &data); 
    // 更新用户列表
    void updateUserList(const std::vector<UserInfo> &users);
    void closeEvent(QCloseEvent *event) override;
    // 禁用跨线程直接调用的危险方法，改为信号触发
    void safeSendFileFragment(FileMsg metaMsg);
    // 新增内网IP判断函数
    bool isPrivateIp(const QHostAddress& addr);
    // 气泡消息工具函数
    void addMessage(const QString& sender, const QString& content, bool isSelf = false);
    // 生成气泡HTML
    QString generateBubbleHtml(const QString& sender, const QString& content, bool isSelf);
      
private:
    // 成员变量
    int m_userId = 0;
    QString m_nickname;
    QTcpSocket *m_socket = nullptr;
    QTimer *m_heartbeatTimer = nullptr; // 心跳定时器
    Ui::ChatWindow *ui;
    QLabel *m_userInfoLabel = nullptr;
    bool m_isPrivateChat = false;
    QTcpSocket *m_tcpP2PSocket;              // 点对点TCP Socket
    QUdpSocket *m_udpP2PSocket;              // 点对点UDP Socket
    QTcpServer *m_tcpP2PServer;              // 用于接收点对点TCP连接
    QHostAddress m_targetIp;                 // 目标IP
    quint16 m_targetTcpPort;                 // 目标TCP端口
    quint16 m_targetUdpPort;                 // 目标UDP端口
    quint16 m_p2pTcpPort = 0;                // 保存系统分配的P2P TCP随机端口
    quint16 m_udpP2PPort = 0;                // UDP监听端口
    QFile *m_transferFile;                   // 传输中的文件
    qint64 m_totalFileSize;                  // 文件总大小
    qint64 m_sentFileSize;                   // 已发送大小
    int m_selectedUserId = 0;                // 选中的聊天对象ID
    QTcpSocket *m_p2pClientSocket = nullptr; // 专门存储接收方的P2P客户端连接
    ImageMsg m_pendingImageMsg;              // 待接收的图片信息
    QByteArray m_pendingImageData;           // 待接收的图片数据
    QMap<int, QString> m_userPortMap;        // 缓存用户ID-端口映射（解决端口不匹配）
    QString m_relayServerIp;                 // 公网中继服务器IP
    quint16 m_relayTcpPort;                  // 中继TCP端口
    quint16 m_relayUdpPort;                  // 中继UDP端口
    QUdpSocket* m_udpRelaySocket;            // 中继专用UDP Socket
    QTcpSocket* m_tcpRelaySocket;            // 中继专用TCP Socket
    QMutex m_fileTransferMutex;              // 文件传输线程安全锁
    // 中继分片缓存（静态成员，全局共享）
    QMap<int, QMap<uint32_t, QByteArray>> m_relayFragMap; // 中继分片缓存
    QMap<int, FileMsg> m_relayMetaMap;                    // 中继文件元信息缓存
};

#endif // CHATWINDOW_H