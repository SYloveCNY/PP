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

LoginWindow::LoginWindow(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::LoginWindow),  // 初始化UI成员
    m_serverSocket(nullptr) // 初始化为null，每次登录创建新Socket
{
    ui->setupUi(this);  // 初始化UI
}

LoginWindow::~LoginWindow() {
    // 析构时彻底关闭Socket
    if (m_serverSocket) {
        m_serverSocket->disconnectFromHost();
        m_serverSocket->deleteLater();
    }
    delete ui;  // 释放UI资源
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
    uint16_t dataPort = getRandomAvailablePort();

    if (nickname.isEmpty()) {
        QMessageBox::warning(this, "警告", "请输入昵称！");
        return;
    }

    // ========== 终极修复：完全重置Socket，避免任何旧连接残留 ==========
    if (m_serverSocket) {
        m_serverSocket->abort(); // 强制关闭，不等待
        m_serverSocket->deleteLater();
        m_serverSocket = nullptr;
    }
    m_serverSocket = new QTcpSocket(this);

    // 连接服务端（增加错误处理）
    connect(m_serverSocket, &QTcpSocket::errorOccurred, this, [=](QAbstractSocket::SocketError error) {
        QMessageBox::critical(this, "Socket错误", "连接/通信失败：" + m_serverSocket->errorString());
    });

    m_serverSocket->connectToHost("127.0.0.1", 8888);
    if (!m_serverSocket->waitForConnected(5000)) {
        QMessageBox::critical(this, "错误", "连接服务端失败！请检查服务端是否启动");
        delete m_serverSocket;
        m_serverSocket = nullptr;
        return;
    }

    // 构造登录请求（简化，确保字段正确）
    QJsonObject loginObj;
    loginObj["nickname"] = nickname;
    loginObj["dataPort"] = dataPort; // 仅保留必要字段
    QJsonDocument doc(loginObj);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);

    // 发送登录请求（增加发送结果检查）
    if (!sendPacket(m_serverSocket, static_cast<uint32_t>(MsgType::LOGIN_REQ), jsonData)) {
        QMessageBox::critical(this, "错误", "发送登录请求失败！");
        m_serverSocket->abort();
        delete m_serverSocket;
        m_serverSocket = nullptr;
        return;
    }

    // 等待响应（增加超时日志）
    if (!m_serverSocket->waitForReadyRead(5000)) {
        QMessageBox::critical(this, "错误", "接收登录响应超时！\n可能原因：\n1. 服务端未处理请求\n2. 网络阻塞\n3. 昵称已被占用（服务端未返回响应）");
        m_serverSocket->abort();
        delete m_serverSocket;
        m_serverSocket = nullptr;
        return;
    }

    // 解析响应（增加容错）
    PacketHeader netRspHeader;
    qint64 headerRead = m_serverSocket->read((char*)&netRspHeader, sizeof(PacketHeader));
    if (headerRead != sizeof(PacketHeader)) {
        QMessageBox::critical(this, "错误", QString("读取响应头部失败！期望%1字节，实际%2字节").arg(sizeof(PacketHeader)).arg(headerRead));
        m_serverSocket->abort();
        delete m_serverSocket;
        m_serverSocket = nullptr;
        return;
    }

    PacketHeader hostRspHeader;
    hostRspHeader.msgType = qFromBigEndian(netRspHeader.msgType);
    hostRspHeader.dataLen = qFromBigEndian(netRspHeader.dataLen);

    if (static_cast<MsgType>(hostRspHeader.msgType) != MsgType::LOGIN_RSP) {
        QMessageBox::critical(this, "错误", QString("收到无效响应类型！期望%1，实际%2").arg(static_cast<int>(MsgType::LOGIN_RSP)).arg(hostRspHeader.msgType));
        m_serverSocket->abort();
        delete m_serverSocket;
        m_serverSocket = nullptr;
        return;
    }

    QByteArray rspJsonData = m_serverSocket->read(hostRspHeader.dataLen);
    QJsonDocument rspDoc = QJsonDocument::fromJson(rspJsonData);
    if (!rspDoc.isObject()) {
        QMessageBox::critical(this, "错误", "解析登录响应失败！响应数据：" + QString(rspJsonData));
        m_serverSocket->abort();
        delete m_serverSocket;
        m_serverSocket = nullptr;
        return;
    }

    QJsonObject rspObj = rspDoc.object();
    bool success = rspObj["success"].toBool();
    QString msg = rspObj["msg"].toString();
    int userId = rspObj["userId"].toInt();

    if (success) {
        ChatWindow* chatWindow = new ChatWindow;
        chatWindow->setLoginInfo(userId, nickname, m_serverSocket);
        chatWindow->show();
        this->hide();
    } else {
        QMessageBox::warning(this, "登录失败", msg);
        m_serverSocket->abort();
        delete m_serverSocket;
        m_serverSocket = nullptr;
    }
}