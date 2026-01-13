#include "LoginWindow.h"
#include "ui_LoginWindow.h"
#include "ChatWindow.h"
#include "protocol.h"
#include "protocol_qt.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QMessageBox>
#include <iostream>
#include <ostream>
#include <QtEndian>  // 必须添加：qFromBigEndian 依赖
#include <QTcpServer>    // 关键：声明QTcpServer类
#include <QHostAddress>
#include <QRandomGenerator>
#include <QThread>
#include <QCloseEvent>

LoginWindow::LoginWindow(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::LoginWindow),  // 初始化UI成员
    m_serverSocket(nullptr), // 初始化为null，每次登录创建新Socket
    m_heartbeatTimer(new QTimer(this)), // 初始化心跳定时器
    m_userId(-1),            // 初始用户ID无效
    m_dataPort(0),            // 初始端口无效
    m_nickname("") // 初始化昵称
{
    ui->setupUi(this);  // 初始化UI
    // 绑定心跳定时器槽函数
    connect(m_heartbeatTimer, &QTimer::timeout, this, &LoginWindow::sendHeartbeat);
    // 初始暂停心跳（登录成功后启动）
    m_heartbeatTimer->stop();
}

LoginWindow::~LoginWindow() {
    // 析构时彻底关闭Socket
    if (m_serverSocket) {
        m_serverSocket->disconnectFromHost();
        m_serverSocket->deleteLater();
    }
    delete ui;  // 释放UI资源
}

// ========== 新增：封装发包函数（处理网络字节序，核心修复） ==========
bool LoginWindow::sendPacket(QTcpSocket* socket, uint32_t msgType, const QByteArray& data) {
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
        QMessageBox::warning(this, "错误", "Socket未连接或无效");
        return false;
    }

    // 构造协议头（转换为网络字节序：大端）
    PacketHeader header;
    // 修复：senderId初始为-1时设为0，避免无效值
    header.senderId = qToBigEndian(static_cast<uint32_t>(m_userId <= 0 ? 0 : m_userId));
    header.msgType = qToBigEndian(msgType);
    header.dataLen = qToBigEndian(static_cast<uint32_t>(data.size()));
    header.msgId = qToBigEndian(0);

    // 关键修复：先发送header，再发送data（协议要求）
    qint64 headerBytes = socket->write((char*)&header, sizeof(PacketHeader));
    if (headerBytes != sizeof(PacketHeader)) {
        QMessageBox::warning(this, "错误", "发送协议头失败");
        return false;
    }

    if (!data.isEmpty()) {
        qint64 dataBytes = socket->write(data);
        if (dataBytes != data.size()) {
            return false;
        }
    }

    // 强制刷出缓冲区
    socket->flush();
    return true;
}

uint16_t LoginWindow::getRandomAvailablePort() {
    // 端口范围：1024-65535（避免使用0-1023的系统保留端口）
    const uint16_t minPort = 1024;
    const uint16_t maxPort = 65535;
    
    // 最多尝试10次（防止极端情况下无限循环）
    for (int i = 0; i < 10; ++i) {
        // 生成随机端口
        uint16_t randomPort = QRandomGenerator::global()->bounded(minPort, maxPort + 1);
        
        // 检查端口是否可用（通过尝试绑定验证）
        QTcpServer tempServer;
        if (tempServer.listen(QHostAddress::Any, randomPort)) {
            // 端口可用，关闭临时服务器释放端口
            tempServer.close();
            return randomPort;
        }
        // 端口已被占用，继续尝试下一个随机数
    }
    
    // 如果多次尝试失败，返回一个默认端口（作为 fallback）
    return 9999;
}

// 登录按钮点击事件实现
void LoginWindow::on_pushButton_login_clicked() {
    QString nickname = ui->lineEdit_nickname->text().trimmed();
    QString avatarPath = ui->lineEdit_avatar->text().trimmed();
    m_dataPort = getRandomAvailablePort();

    if (nickname.isEmpty()) {
        QMessageBox::warning(this, "警告", "请输入昵称！");
        return;
    }

    // 关键修复1：先销毁旧Socket，避免内存泄漏
    if (m_serverSocket) {
        m_serverSocket->disconnectFromHost();
        m_serverSocket->deleteLater();
        m_serverSocket = nullptr;
    }

    // 关键修复2：创建QTcpSocket实例（解决空指针）
    m_serverSocket = new QTcpSocket(this);

    // 绑定错误处理槽函数（此时m_serverSocket非空）
    connect(m_serverSocket, &QTcpSocket::errorOccurred, this, [=](QAbstractSocket::SocketError error) {
        QMessageBox::critical(this, "Socket错误", "连接/通信失败：" + m_serverSocket->errorString());
    });

    // 新增：绑定消息接收槽函数（处理服务端广播）
    connect(m_serverSocket, &QTcpSocket::readyRead, this, &LoginWindow::onReadyRead);

    m_serverSocket->connectToHost("127.0.0.1", 8888);
    if (!m_serverSocket->waitForConnected(5000)) {
        QMessageBox::critical(this, "错误", "连接服务端失败！请检查服务端是否启动");
        // 清理无效Socket
        m_serverSocket->deleteLater();
        m_serverSocket = nullptr;
        return;
    }

    // 构造登录请求（简化，确保字段正确）
    QJsonObject loginObj;
    loginObj["nickname"] = nickname;
    loginObj["dataPort"] = m_dataPort; // 仅保留必要字段
    QJsonDocument doc(loginObj);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);

    // 发送登录请求（增加发送结果检查）
    if (!sendPacket(m_serverSocket, static_cast<uint32_t>(MsgType::LOGIN_REQ), jsonData)) {
        QMessageBox::critical(this, "错误", "发送登录请求失败！");
        m_serverSocket->abort();
        m_serverSocket->deleteLater();
        m_serverSocket = nullptr;
        return;
    }

    // 等待响应（增加超时日志）
    if (!m_serverSocket->waitForReadyRead(5000)) {
        QMessageBox::critical(this, "错误", "接收登录响应超时！\n可能原因：\n1. 服务端未处理请求\n2. 网络阻塞\n3. 昵称已被占用（服务端未返回响应）");
        m_serverSocket->abort();
        m_serverSocket->deleteLater();
        m_serverSocket = nullptr;
        return;
    }

    // 解析响应（增加容错）
    PacketHeader netRspHeader;
    qint64 headerRead = m_serverSocket->read((char*)&netRspHeader, sizeof(PacketHeader));
    if (headerRead != sizeof(PacketHeader)) {
        QMessageBox::critical(this, "错误", QString("读取响应头部失败！期望%1字节，实际%2字节").arg(sizeof(PacketHeader)).arg(headerRead));
        m_serverSocket->abort();
        m_serverSocket->deleteLater();
        m_serverSocket = nullptr;
        return;
    }

    PacketHeader hostRspHeader;
    hostRspHeader.msgType = qFromBigEndian(netRspHeader.msgType);
    hostRspHeader.dataLen = qFromBigEndian(netRspHeader.dataLen);

    if (static_cast<MsgType>(hostRspHeader.msgType) != MsgType::LOGIN_RSP) {
        QMessageBox::critical(this, "错误", QString("收到无效响应类型！期望%1，实际%2").arg(static_cast<int>(MsgType::LOGIN_RSP)).arg(hostRspHeader.msgType));
        m_serverSocket->abort();
        m_serverSocket->deleteLater();
        m_serverSocket = nullptr;
        return;
    }

    QByteArray rspJsonData = m_serverSocket->read(hostRspHeader.dataLen);
    QJsonDocument rspDoc = QJsonDocument::fromJson(rspJsonData);
    if (!rspDoc.isObject()) {
        QMessageBox::critical(this, "错误", "解析登录响应失败！响应数据：" + QString(rspJsonData));
        m_serverSocket->abort();
        m_serverSocket->deleteLater();
        m_serverSocket = nullptr;
        return;
    }

    QJsonObject rspObj = rspDoc.object();
    bool success = rspObj["success"].toBool();
    QString msg = rspObj["msg"].toString();
    m_userId = rspObj["userId"].toInt(); // 保存用户ID
    m_nickname = nickname;               // 保存昵称

    if (success) {
        // 启动心跳定时器
        m_heartbeatTimer->start(5000);
        ChatWindow* chatWindow = new ChatWindow;
        chatWindow->setLoginInfo(m_userId, m_nickname, m_serverSocket);
        chatWindow->show();
        this->hide();
    } else {
        QMessageBox::warning(this, "登录失败", msg);
        // 登录失败：停止心跳+清理Socket
        m_heartbeatTimer->stop();
        m_serverSocket->abort();
        m_serverSocket->deleteLater();
        m_serverSocket = nullptr;
    }
}

// 发送心跳包（5秒一次）
void LoginWindow::sendHeartbeat() {
    // 空指针检查
    if (!m_serverSocket || m_serverSocket->state() != QAbstractSocket::ConnectedState) {
        m_heartbeatTimer->stop();
        return;
    }
    sendPacket(m_serverSocket, static_cast<uint32_t>(MsgType::HEARTBEAT), QByteArray());
}

// 处理服务端消息（状态广播/心跳响应）
void LoginWindow::onReadyRead() {
    if (!m_serverSocket || m_serverSocket->state() != QAbstractSocket::ConnectedState) {
        return;
    }

    // 循环读取（防止粘包）
    while (m_serverSocket->bytesAvailable() >= sizeof(PacketHeader)) {
        PacketHeader netHeader;
        qint64 headerLen = m_serverSocket->read((char*)&netHeader, sizeof(PacketHeader));
        if (headerLen != sizeof(PacketHeader)) {
            break;
        }

        // 转换为主机字节序
        PacketHeader hostHeader;
        hostHeader.msgType = qFromBigEndian(netHeader.msgType);
        hostHeader.dataLen = qFromBigEndian(netHeader.dataLen);

        // 1. 处理状态广播（上线/下线通知，MsgType::STATUS_BROADCAST = 10）
        if (hostHeader.msgType == static_cast<uint32_t>(MsgType::USER_STATUS_NOTIFY)) {
            QByteArray data = m_serverSocket->read(hostHeader.dataLen);
            QJsonDocument notifyDoc = QJsonDocument::fromJson(data);
            if (notifyDoc.isObject()) {
                QJsonObject notifyObj = notifyDoc.object();
                int userId = notifyObj["userId"].toInt();
                QString nickname = notifyObj["nickname"].toString();
                bool isOnline = notifyObj["isOnline"].toBool();
                QString statusMsg = QString("%1（ID：%2）%3").arg(nickname).arg(userId).arg(isOnline ? "上线了" : "下线了");
                QMessageBox::information(this, "系统通知", statusMsg);
            }
            continue;
        }

        // 2. 处理心跳响应（可选，MsgType::HEARTBEAT_RESP = 9）
        if (hostHeader.msgType == static_cast<uint32_t>(MsgType::HEARTBEAT_RESP)) {
            std::cout << "心跳响应正常" << std::endl;
            continue;
        }

        // 3. 其他消息（如登录响应已在登录函数处理，此处留空）
    }
}

// 重写关闭事件（发送主动下线请求）
void LoginWindow::closeEvent(QCloseEvent *event) {
    // 停止心跳
    m_heartbeatTimer->stop();

    if (m_serverSocket && m_serverSocket->state() == QAbstractSocket::ConnectedState) {
        // 发送主动下线请求
        sendPacket(m_serverSocket, static_cast<uint32_t>(MsgType::LOGOUT_REQ), QByteArray());

        // 确保消息发送完成
        m_serverSocket->flush();
        QThread::msleep(100);

        // 断开连接
        m_serverSocket->disconnectFromHost();
        m_serverSocket->waitForDisconnected(500);
    }

    // 允许窗口关闭
    event->accept();
}