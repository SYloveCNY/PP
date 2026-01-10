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
    m_serverSocket(new QTcpSocket(this))  // 初始化socket
{
    ui->setupUi(this);  // 初始化UI
}

LoginWindow::~LoginWindow() {
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
    // uint16_t dataPort = 9999;  // 固定数据端口（测试用）
    // 生成随机可用端口（替换固定的9999）
    uint16_t dataPort = getRandomAvailablePort();

    if (nickname.isEmpty()) {
        QMessageBox::warning(this, "警告", "请输入昵称！");
        return;
    }

    // 连接服务端（IP：127.0.0.1，端口：8888）
    m_serverSocket->connectToHost("127.0.0.1", 8888);
    if (!m_serverSocket->waitForConnected(3000)) {
        QMessageBox::critical(this, "错误", "连接服务端失败！");
        return;
    }

    // 构造登录请求JSON
    QJsonObject loginObj;
    loginObj["nickname"] = nickname;
    loginObj["avatar"] = avatarPath;
    loginObj["dataPort"] = dataPort;
    QJsonDocument doc(loginObj);
    QByteArray jsonData = doc.toJson(QJsonDocument::Compact);

    // 发送登录请求（使用 protocol_qt.h 中的 sendPacket 函数）
    if (!sendPacket(m_serverSocket, static_cast<uint32_t>(MsgType::LOGIN_REQ), jsonData)) {
        QMessageBox::critical(this, "错误", "发送登录请求失败！");
        m_serverSocket->disconnectFromHost();
        return;
    }

    // 等待登录响应
    if (!m_serverSocket->waitForReadyRead(3000)) {
        QMessageBox::critical(this, "错误", "接收登录响应超时！");
        m_serverSocket->disconnectFromHost();
        return;
    }

    // 读取并解析登录响应（直接调用 qFromBigEndian）
    PacketHeader netRspHeader;
    m_serverSocket->read((char*)&netRspHeader, sizeof(PacketHeader));
    PacketHeader hostRspHeader;
    hostRspHeader.msgType = qFromBigEndian(netRspHeader.msgType);  // 无需 Qt:: 前缀
    hostRspHeader.dataLen = qFromBigEndian(netRspHeader.dataLen);

    if (static_cast<MsgType>(hostRspHeader.msgType) != MsgType::LOGIN_RSP) {
        QMessageBox::critical(this, "错误", "收到无效响应！");
        m_serverSocket->disconnectFromHost();
        return;
    }

    QByteArray rspJsonData = m_serverSocket->read(hostRspHeader.dataLen);
    QJsonDocument rspDoc = QJsonDocument::fromJson(rspJsonData);
    QJsonObject rspObj = rspDoc.object();
    bool success = rspObj["success"].toBool();
    QString msg = rspObj["msg"].toString();
    int userId = rspObj["userId"].toInt();

    if (success) {
        // 登录成功，打开聊天窗口
        ChatWindow* chatWindow = new ChatWindow;
        chatWindow->setLoginInfo(userId, nickname, m_serverSocket);
        chatWindow->show();
        this->hide();  // 隐藏登录窗口
    } else {
        QMessageBox::warning(this, "登录失败", msg);
        m_serverSocket->disconnectFromHost();
    }
}
